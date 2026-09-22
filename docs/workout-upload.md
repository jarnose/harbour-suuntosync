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

## Still open

Three things, and the first is the real blocker:

1. **`workoutBinary`'s format is unknown.** It is produced by
   `BinaryFileRepository`, it is not SBEM, and it appears to be mandatory.
   This is Sports Tracker's own container and nothing in this project decodes
   or produces it yet. Next step: decompile `BinaryFileRepository` and
   whatever writes the file.
2. **`sml.zip`'s contents are unverified.** A zip, but of what - the watch's
   raw SBEM bytes, or Suunto's XML/JSON SML? The cloud's *download* side
   (`GET workouts/{key}/sml`, which this project now parses) returns JSON,
   which is suggestive but not proof that the upload side matches.
3. **Whether the SML part may be omitted**, as the log line hints.

Until (1) is answered, the upload cannot be built - knowing the endpoint is
not the same as being able to fill it.
