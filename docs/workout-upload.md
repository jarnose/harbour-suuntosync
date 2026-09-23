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

## The watch-synced capture (2026-09-22, second run)

Paired the watch to the same device and synced. This is the case the first
capture couldn't reach, and it overturns the conclusion drawn from it.

**A watch-synced upload has no `workoutBinary` at all.** Both requests
carried exactly two parts:

| part | content |
|---|---|
| `workoutExtensions` | `[]` |
| `sml` | a zip, 2.3-2.5 kB |

Both returned 200. So neither `workoutBinary` nor `sml` is mandatory -
they are alternatives. A phone-recorded workout sends the binary; a
watch-recorded one sends SML. **This project therefore never needs to
produce Sports Tracker's legacy binary format at all**, and the
`HeaderSerializer` transcription described above is not on the critical
path. The fixtures and the analysis stay because they document the format,
not because we need to write it.

### What is in `sml.zip`

Two JSON files - not the binary SBEM the watch serves:

```
samples.json    {"Samples": [ ... per-sample entries ... ]}
summary.json    {"Samples": [ ... Windows ..., Header ]}
```

A samples entry:

```json
{"Attributes": {"suunto/sml": {"Sample": {"HR": 1.35}}},
 "Source": "suunto-2352D0000247",
 "TimeISO8601": "2026-09-22T15:20:28.430+03:00"}
```

`Source` is `"suunto-"` plus the watch serial. Events appear the same way
(`Sample.Events[].Lap.Type`, `.Activity.ActivityType`). `summary.json`
carries `Windows` entries plus a `Header` whose field names match the ones
`Summary::decode()` already reads off the watch (`ActivityType`,
`Altitude.Max/Min`, `Ascent`, `DateTime`, `Device.Info`, ...).

The field names and units are the watch's own: `HR: 1.35` is hertz, the
same canonical unit `sbemdescriptors.cpp` uses. That also closes the
hertz-vs-bpm question left open in `appcontroller.cpp`'s cloud series
parser - the cloud stores hertz because the watch uploads hertz.

**So the upload path is a format conversion this project is already most of
the way through**: `Sml::decode()` yields (descriptor, value, timestamp)
triples and `Summary::decode()` yields the header fields; both need
emitting as these two JSON documents, zipped, and posted. No new protocol
work, no FIT encoder, no legacy binary.

Fixtures: `tests/fixtures/cloud_sml_samples.json`, `cloud_sml_summary.json`.

## Health data: `247.sports-tracker.com`

The same sync also pushed the watch's round-the-clock data to a second
host, over `AskoTimelineRestApi`:

```
POST v1/sleep         @Body List    GET v1/sleep/export?since=<ms>
POST v1/sleepstages   @Body List    GET v1/sleepstages/export?since=<ms>
POST v1/recovery      @Body List    GET v1/recovery/export?since=<ms>
POST v1/activity      @Body List    GET v1/activity/export?since=<ms>
```

Authentication is the same `sttauthorization` session key, `content-type:
application/json; charset=UTF-8`, nothing else. Reads stream back NDJSON
(one JSON object per line, no ASKO envelope); writes take a plain JSON
array. Every entry has the same two-key shape:

```json
{"timestamp": "2026-09-21T22:54:00.000+03:00", "entryData": { ... }}
```

Per endpoint, from the real capture (fixtures in `tests/fixtures/cloud_247_*.json`):

- **sleep** - one entry per night: `deepSleepDuration`, `lightSleepDuration`,
  `remSleepDuration`, `duration`, `hrAvg`, `hrMin` (hertz), `quality`,
  `sleepId` (a unix timestamp in seconds, same convention as a logbook id),
  `maxSpo2`, `altitude`, `avgHrv`, `avgHrvSampleCount`, `isNap`,
  `sleepOnsetLatencyDuration`, `wakeAfterSleepOnsetDuration`,
  `wakeBeforeOffBedDuration`.
- **sleepstages** - 21 entries for one night: `stage` (`LIGHT`, `AWAKE`,
  `DEEP`, and presumably `REM`) and `duration` seconds.
- **recovery** - 61 entries at 30-minute spacing: `balance`, `stressState`.
- **activity** - 182 entries at 10-minute spacing: `hr` (hertz),
  `stepCount`, `energyConsumption`.

Jarno's immediate question - why last night's sleep never reached the
cloud - is answered by the capture itself: the `POST /v1/sleep` in it
carried a `2026-09-21T22:54` entry. The data was sitting on the watch
waiting for a sync that hadn't happened, not missing. Getting it out over
BLE is a separate matter: the resources exist (`/Sleep/<x>/Entries` and
`/Activity/<x>/Entries` were both in the very first string dump) but
neither has been fetched or decoded by this project.

## First real upload attempt: two bugs (2026-09-23)

The server answered `500` with
`{"error":{"code":"523","description":"Workout could not be saved"}}`. That
the multipart parsed and the session key was accepted at all is useful - it
narrowed the problem to the content. Pulling the stored zip off the phone
(`~/.local/share/io.github.jarnose/suuntosync/suuntosync.sqlite`, table
`workout_sml`) showed both causes immediately, neither of them a guess:

1. **The JSON was invalid.** Every double came out as `"HR":1,35`.
   `snprintf`'s `%g` honours the C library's current locale, and the phone
   runs a Finnish one. The tests here never saw it because they run under a
   C locale - a class of bug that no golden vector catches, because the
   vector and the test agree with each other. Fixed by normalising the
   separator after formatting (not by touching the process locale, which
   would be a thread-unsafe side effect in a Qt app), and
   `tests/test_smljson.cpp` now runs the whole document build under `fi_FI`
   and asserts no digit-comma-digit sequence survives.

2. **`summary.json` was missing entirely** - the zip held only
   samples.json. A `/Summary` payload has no clock chunk, so every entry in
   it had `timeMs == 0` and the writer skipped them all. The captured
   upload stamps summary.json's entries at the *end* of the workout, so
   `buildDocument` gained a `fallbackTimeMs` and the caller passes the
   workout's stop time.

Worth recording: the earlier "known difference" about nil readings being
omitted rather than written as `null` was **not** the cause, and remains
untested either way.

### Third attempt: `local64` fields are ISO strings

With summary.json finally in the payload, the server's answer changed from
a flat 523 to a specific one:

```
422 {"error":{"code":"322","description":
     "Header DateTime is not a valid ISO datetime: 1790186340390"}}
```

Which is exactly right: `Header.DateTime` is `<FRM>local64`, this writer
was emitting it as the raw millisecond number, and the captured upload has
`"DateTime": "2026-09-22T15:20:28.420+03:00"`.

The rule is by format, not by field name - four descriptors are `local64`:

| id | field |
|---|---|
| 60 | `Sample.UTC` |
| 74 | `Sample.GpsRef.utc` |
| 136 | `Header.DateTime` |
| 211 | `Header.Settings.SgeeEpoTimestamp` |

(32 and 33 are also local64 but are envelope fields, already filtered out.)

All four are now written as ISO strings with the document's offset, the
same way the entry's own `TimeISO8601` is. Worth noting how much better a
422 is than a 500 here: the server validates field by field and names the
field and the value, so each round trip now costs one bug rather than a
guess.

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
