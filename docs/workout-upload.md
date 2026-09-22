# Uploading a workout to the Suunto cloud

How the official Android app (`com.stt.android.suunto` 6.7.12) pushes a
workout it just pulled off the watch back up to the cloud. Recovered from the
APK on 2026-09-22; every claim below is read out of the app's own bytecode,
and the "Still open" section at the end says plainly what is *not* known.

This matters because the app's private API is the one this project already
authenticates against (`api.sports-tracker.com/apiserver/v1/`,
`STTAuthorization` session key - see `src/cloud/suuntocloudclient.cpp`). The
upload therefore needs no new auth work: it is the same session key on the
same host.

## Getting past R8

The APK's Retrofit annotations are obfuscated - the string `retrofit2/http`
does not appear in any of the ten dex files, so neither the HTTP verbs nor
the paths can be read directly, and androguard exposes annotation *types*
but not their values.

The way through: Retrofit's own `RequestFactory.Builder.parseMethodAnnotation`
does an `instanceof` against each annotation class and passes the verb along
as a string literal. That method survives obfuscation as
`Lm51/w$a;->e(Ljava/lang/annotation/Annotation;)V`, and reading its bytecode
gives the mapping directly:

| obfuscated class | annotation |
|---|---|
| `Lr51/b;` | `@DELETE` |
| `Lr51/f;` | `@GET` |
| `Lr51/g;` | `@HEAD` |
| `Lr51/n;` | `@PATCH` |
| `Lr51/o;` | `@POST` |
| `Lr51/p;` | `@PUT` |
| `Lr51/m;` | `@OPTIONS` |

The `instanceof` order matches Retrofit's source exactly (DELETE, GET, HEAD,
PATCH, POST, PUT, OPTIONS), which is the cross-check that this is that
method rather than a coincidence. Parameter annotations were read the same
way: `s` = `@Path`, `t` = `@Query`, `i` = `@Header`, `a` = `@Body`,
`q` = `@Part`, `l` = `@Multipart`.

The tooling is in this session's scratchpad (`dexannots.py` reads the dex
annotation tables androguard skips; `dexclass.py` dumps one interface in
full). Both are throwaway analysis scripts, deliberately not part of the
app build.

## The request

`com.stt.android.remote.workout.WorkoutRestApi`:

```
@Multipart
@POST("workout")
saveWorkout(@Part MultipartBody.Part, @Part MultipartBody.Part, @Part MultipartBody.Part)
```

So: `POST /apiserver/v1/workout`, `multipart/form-data`, exactly three parts.
The response is the ordinary ASKO envelope this project already unwraps
(`AskoResponse.e()` is called on the result).

`WorkoutRemoteApi.n()` (unobfuscated name `saveWorkout`, confirmed by the
`WorkoutRemoteApi$saveWorkout$1` continuation class) builds the parts from a
`RemoteUnsyncedWorkout(workoutFile: File, remoteWorkoutExtensions: List, smlZipFile: File)`:

| part name | filename | source | content type |
|---|---|---|---|
| `workoutBinary` | `binary` | `workoutFile` | `application/octet-stream` |
| `workoutExtensions` | `extension` | Moshi `toJson` of the extensions list | `application/json;charset=` |
| `sml` | `sml.zip` | `smlZipFile` | `application/zip` |

Content types come from `com.stt.android.remote.MediaTypes` (`a()` json,
`b()` octet-stream, `c()` zip).

The caller is
`com.stt.android.data.workout.sync.SyncNewWorkout.g(WorkoutHeader, List)`,
which gets `workoutFile` from
`com.stt.android.data.workout.binary.BinaryFileRepository.c(WorkoutHeader)`
and logs `"Syncing a new workout. Has SML: <x>, extensions: <n>"` - the
phrasing of that log line is the reason to think the SML part is optional
while the binary part is not.

## What this means for this project

The third part is SML, which is what the watch already hands us - so a
watch-to-cloud path does **not** need a FIT encoder, and does not need the
official partner API at `cloudapi.suunto.com` (OAuth2 +
`Ocp-Apim-Subscription-Key`, a registered `client_id`/`client_secret`, and a
FIT file - see `jmallach/suunto-garmin-sync`, whose own docstring marks
several of its paths as unconfirmed). Everything here rides on the session
key this app already has.

## `workoutBinary`: Sports Tracker's legacy container

Traced on the same day. The write path is
`FsBinaryFileRepository.n(Workout)` -> `.m()` ->
`WorkoutBinaryController.e()` -> `.d()`, and `.d()` writes three sections
back to back into one `DataOutputStream` - so the whole file is Java's
`DataOutput` encoding: big-endian, and `writeUTF` is modified UTF-8 with a
two-byte length prefix.

```
workoutBinary := HeaderSerializer.c(out, LegacyHeader, appVersion)
                 ServiceHeaderSerializer.b(out, LegacyServiceHeader)
                 LegacyWorkoutSerializer.c(out, LegacyWorkout)
```

The read side (`WorkoutBinaryController.b()`) consumes the same three in the
same order, and the original method name survives as a Kotlin null-check
message: `readLegacyWorkoutBinary`.

**`LegacyWorkoutSerializer.c` is simple** - eight blocks, three of which are
hardcoded zeroes:

| # | content |
|---|---|
| 1 | `writeInt(n)` + n x `EventSerializer.b` |
| 2 | `writeInt(n)` + n x `LocationEventSerializer.c` (delta-coded: each call gets the previous event) |
| 3 | `writeInt(0)` - constant |
| 4 | `writeInt(0)` - constant |
| 5 | `writeInt(n)` + n x `HeartrateEventSerializer.b` |
| 6 | `writeInt(n)` + n x `LocationEventSerializer.e` |
| 7 | `writeInt(n)` + n x `MediaEventSerializer.b` |
| 8 | `writeInt(0)` - constant |

With every list empty that section is 32 zero bytes. The reader's count
validation (`b()`) accepts 0 and rejects negatives or anything above
2,000,000, so an empty section is legal by its own rules.

That matters because of how a **manually added** workout is stored:
`FsBinaryFileRepository$create$2` builds its `WorkoutData` from
`emptyList()` throughout and logs "Unable to store binary file for manually
added workout %d" on failure. So the app itself writes binaries with no
track, no heart rate and no events - which is the shape a watch-synced
upload could plausibly use, letting the SML part carry the actual data.
**Plausibly** - this has not been tested against the server.

**`HeaderSerializer.c` is the bulk of the work**: roughly forty scalar
fields (`writeInt`/`writeShort`/`writeByte`/`writeDouble`/`writeUTF`, with
the name truncated to 256 chars and two other strings to 32), then two
`WorkoutGeoPoint`s via `CoordinatesSerializer.e`, five `Statistics` blocks
via `StatisticsSerializer.b`, and a `LegacyHeartRateData` via
`HeartRateDataSerializer.b`. Every one of those is readable the same way;
none of it has been transcribed field by field yet.

## What a third reverse-engineering adds: `x-totp`

`Marius-Ar/suunto-api-wrapper` (TypeScript, no license file) targets this
same private API. It does **not** implement the workout upload - it covers
reads plus the social and SuuntoPlus surfaces - but two things in it are
worth having:

- Its TOTP key material (`PART1`, and the XOR key
  `Bh8nsTyCeC0Ql2drMen78awk84AE3ZxW`) matches `suuntoauth.cpp`'s constants
  byte for byte. Those were ported from `tajchert/suuntool`'s Go, so this is
  an independent third party arriving at the same bytes - a real
  cross-check on the one piece of this project's auth that came from someone
  else's work.
- Its `AuthSession` sends **`x-totp`** on *every* request, not just at
  login. The APK corroborates that the header exists: `WorkoutRestApi`
  declares it as an explicit `@Header("x-totp")` on
  `fetchCompetitionWorkoutResult`.

Reads demonstrably work without it (everything this project does today), so
it is not a fix for anything broken. It is groundwork: if the upload turns
out to require it, the alternative is debugging a 401 with no idea which of
several unknowns caused it. `SuuntoCloudClient::authorizedRequest()` now
sends it whenever the account email is known.

Also noted for later, not acted on: that wrapper fetches a workout via
`GET /apiserver/v2/workouts/{username}/{workoutKey}/combined` with
`extensions` and `additionalData` query parameters, which would collapse
this project's separate detail and extensions calls into one.

## Confirmed by a real capture (2026-09-22)

Captured off a rooted LineageOS 21 device running the same APK version
(6.7.12), mitmproxy's CA bind-mounted over the Conscrypt APEX store. Two
workouts created **on the phone** - one added by hand, one recorded with the
app's own tracker - both accepted with 200.

What the real requests settle:

- **There are only two parts, not three.** Neither upload carried an `sml`
  part at all, and both succeeded. So the minimum accepted body is
  `workoutBinary` + `workoutExtensions`, and `[]` is a valid extensions
  value (the tracked workout sent one `WeatherExtension`). This confirms
  the reading of the `"Has SML:"` log line: SML is optional, and a
  phone-recorded workout has none to send. A watch-synced upload should
  add it as the third part; that case is still uncaptured.
- **The headers are exactly the four this project already sends**:
  `sttauthorization`, `user-agent`, `accept-language`, plus the multipart
  `content-type`. No signature, no timestamp, no salt - the session key
  alone authenticates a write.
- **No `x-totp`.** See below.
- **`workoutBinary` is small**: 672 bytes for the manual workout, 880 for
  the tracked one. Both are checked in as
  `tests/fixtures/workout_binary_manual.bin` and
  `..._tracked.bin` - the golden vectors this format was missing.

A first pass over the manual one against `HeaderSerializer.c`'s field order
lands its three strings exactly where the bytecode says they should be:
offset 28 `"testi"` (the description typed in), 49 `"jarnoselnp"`
(username), 61 `"9/22/26 9:28 PM"` (workout name). The order read out of the
bytecode is therefore right; the remaining scalar fields just need
transcribing, now against bytes that can prove it.

### The x-totp correction

This project briefly sent `x-totp` on every authenticated request, on the
strength of `Marius-Ar/suunto-api-wrapper` doing so. The capture measured
it: the real app sends it on **2 of 122** requests - a user email-status
check and a settings POST - and not on the workout upload nor on any read
this project makes. That change has been reverted;
`SuuntoAuth::generateTotp()` stays for whichever endpoint eventually wants
it. A third-party client's habit is not the app's behaviour, and only the
capture could tell them apart.

## Still open

1. **The exact field order of `HeaderSerializer.c`, `ServiceHeaderSerializer.b`
   and the four sub-serializers.** Readable, not yet read.
2. **`sml.zip`'s contents.** A zip, but of what - the watch's raw SBEM
   bytes, or Suunto's XML/JSON SML? The cloud's *download* side
   (`GET workouts/{key}/sml`, which this project now parses) returns JSON,
   which is suggestive but not proof that the upload side matches.
3. ~~Whether the SML part may be omitted~~ - **answered: yes**, see above.
   What an SML part looks like when present is still uncaptured, since that
   needs a watch-synced workout.

~~There is no golden vector for any of this.~~ **There are two now** - see
the capture section above. What is still missing is a *watch-synced*
upload, which is the only way to see an `sml` part; the two captured here
were both recorded on the phone.
