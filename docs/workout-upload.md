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

### ✅ Working, 2026-09-23

Fourth attempt returned 200, and the workout shows up in the official
Android app. The full chain - BLE fetch, Heatshrink, SBEM decode, JSON
build, zip, multipart POST - works from Sailfish with no Android app
involved.

Four bugs stood between the first attempt and this one, and it's worth
recording that **none of them was the one I predicted**. I flagged the
omitted nil readings as "the first thing to look at"; the upload succeeds
without them, so that difference is now confirmed harmless. What actually
broke it:

1. A locale-dependent decimal separator (`1,35`) - invisible to every test
   here, because the tests and the fixtures shared the same C locale.
2. `summary.json` missing entirely, because a /Summary payload has no clock
   chunk and every entry got skipped.
3. Full float precision where the schema's `precision=` says to round.
4. `local64` timestamps written as numbers instead of ISO strings.

Three of the four were things the captured upload already showed plainly;
I had the evidence and read past it. The fourth (locale) no capture could
have revealed. The lesson that generalises: a golden vector proves the
*structure* of what you build, and proves nothing about the environment
you build it in.

### Health upload works (2026-09-23)

`POST 247.sports-tracker.com/v1/<kind>` accepted a watch-decoded sleep
entry. One bug on the way, and the server named it exactly: *"'quality'
with value 2.55 is outside of range 0.0...1.0"*. 2.55 is 255/100, and 0xFF
is the sentinel for a byte field the watch didn't measure - three of
twelve captured nights carry it. Those keys are now omitted rather than
sent.

A note on bookkeeping, because the first successful run reported only one
entry and that looked wrong. It wasn't: a cloud sync had already brought
those nights down, so they were demonstrably in the cloud and needed no
upload. Only the newest night - on the watch but never synced by the
official app - was actually pending. That is the case this whole path
exists for.

The conflict rule is now directional, which the first version got only
half right:

- from the **cloud**: clear the pending flag, even if a watch sync set it.
  The row is up there.
- from the **watch**: update the payload, leave the flag alone. A row the
  cloud already gave us stays not-pending; a genuinely new one keeps the
  `pending = 1` it was inserted with.

## Laps, from the cloud's own sample data (2026-10-05)

`GET /v1/workouts/{key}/sml` carries the laps, and not where the curves
are. The response is

```
{ "Data":    { "Samples": [ ... 12974 per-sample points ... ] },
  "Summary": { "Samples": [ ... 10 ... ] } }
```

and the laps are in **Summary**. Each summary sample has
`Attributes["suunto/sml"]`, and the interesting ones hold a `Windows` array
whose entries carry a `Type`:

| `Type` | what it is |
|---|---|
| `Autolap` | an automatic lap |
| `Lap` | one the wearer pressed for |
| `Activity` | the whole activity |
| `Move` | the whole recording |

Captured from a walk on 2026-10-02 with both kinds on it - three of each:

```
Autolap  1076.0 s  1000 m      Lap  1446.5 s  1349 m
Autolap   996.3 s  1000 m      Lap   365.8 s   270 m
Autolap   590.7 s   830 m      Lap   850.8 s  1211 m
```

**The two kinds are parallel partitions of the same workout, not a
sequence.** Each set sums to the whole on its own - 2830 m and 2663 s both
ways, matching `Move` exactly. So they are numbered within their own kind;
interleaving them would double every distance.

`Distance` and `Duration` are each window's own rather than cumulative,
which is the opposite of how the watch's own lap markers work - those are
absolute and the decoder differences them. `TimeISO8601` on the sample is
where the window ended.

A window also carries the lap's `Altitude`, `Cadence`, `HR`, `Power`,
`Speed`, `Temperature` and `VerticalSpeed` as `{Avg, Max, Min}`, plus
`Ascent`, `Descent`, `Energy` and a long row of dive and running-dynamics
fields that are null on a walk. None of that is read yet; the four fields
the lap list shows are.

One unit worth noting because it confirms an older question: `HR` here is
**hertz** - `Avg: 1.29` for a walk, which is 77 bpm - and `Energy` is joules,
333688 for the first lap, the same unit the watch uses.

The names map onto the vocabulary the BLE decoder already uses for the
watch's own markers: `Autolap` is a distance lap and `Lap` is a manual one.
Both sources therefore produce the same words in the lap list.

## The workout list is paginated, and was not being paged (2026-10-05)

`GET /v1/workouts?since=0&limit=100&offset=0` - with the offset hard-coded.
So the hundred most recent workouts arrived and nothing older ever did. The
database here had 104 cloud workouts only because several syncs over weeks
each brought the latest hundred and the older ones stayed.

It matters for more than tidiness. A chronic training load is an
exponential average with a 42-day time constant, so days four months back
still carry about six per cent of today's figure - and six per cent of a
CTL of 15 is most of the 1.2 that this app's figure was short of the
Android app's.

Paged now: ask for 100, store the page, and ask again at the next offset
while a full page keeps coming back, to a ceiling of ten thousand so a
server that always answers with a full page cannot loop for ever. Pages
already stored stay stored if a later one fails.

**What paging actually brought.** 104 cloud workouts became **1162**, with
history back to 2010-09-20 - and the six-month hole in early 2026 that
looked like a training break was simply the hundred-row cut. Recomputed over
the whole history: CTL 14.58, ATL 4.68, TSB +9.47, against the Android
app's 15, 5 and 9.

**Confirmed on the phone**: with the paged sync actually finished, the app
shows 15, 5 and +9 - the same three figures the official app shows, from
Suunto's own per-workout stress scores through the standard 42- and 7-day
averages. Nothing is being approximated.

**Checked a second time, 2026-10-05**, after the incremental sync below and
a few more workouts: the official app showed 17, 12 and +4, and so did this
one. That is the more informative of the two checks. A single agreement can
come from a level that happens to coincide; agreement after ATL has more
than doubled and TSB has swung from +9 to +4 says the decay constants
themselves are right.

Worth knowing about the first such sync: it fetches twelve pages and more
than a thousand workouts. Every later sync used to re-fetch the lot,
because `since` was hard-coded to 0 - what it meant had not been
established, and guessing it would quietly drop edited workouts. A capture
settled it; see the next section.

**A wrong explanation, recorded because it was stated confidently.** The
first account of that 1.2 was that the database's newest workout was three
days old and the Android app was counting newer ones. Jarno pointed out
what that overlooks: CTL *decays* without training, which this
implementation already does - 14.8 on the day of the last workout, 13.8
three days later. If there was nothing newer to count then both figures are
today's and the gap is real. The missing history is the better candidate,
and unlike the first one it is measurable.

## What `since` means, and where Progress comes from (2026-10-05)

A second capture, the same bind-mounted-CA recipe as before, with the
official app's own cold start and then every screen scrolled through:
238 flows, 91 of them to `api.sports-tracker.com`.

### `since` is the cloud's clock, not the workout's

```
GET /v1/workouts?since=1791180021266&limit=50&offset=0
  -> metadata: { "workoutcount": "7", "until": "1791205512579" }
```

`since` = 5.10 09:00:21. The seven workouts that came back **started**
between 25.9 and 5.10 - so it is not filtering on `startTime`. All seven
were `created` 5.10 15:48, which is when they reached the cloud. So `since`
is a server-side ingest time, and a workout recorded a fortnight ago but
uploaded this afternoon still arrives.

**Which field: `lastModified`. Measured, 2026-10-06.** The payload carries
both `created` (second granularity - it is the ObjectId's own timestamp)
and `lastModified` (milliseconds). The capture could not separate them,
because every workout in it was newer than the cursor by both. So the
experiment was run instead: a workout recorded on 30 September, already in
the phone's database as 1321 s, was edited in the official app to 1322 s,
with the phone's cursor standing at 6 October 08:54 - months after that
workout was created. An ordinary incremental sync brought it back, and the
local copy now reads 1322.

That is the answer worth having, because it is the good one: **an
incremental sync picks up edits**, not only new workouts. Nothing has to
change, and the full re-fetch is for a local copy that is damaged rather
than merely behind.

The cursor the app carries forward is `metadata.until`, which is the
server's own clock at the moment it answered - 16:05:12.579, against a
request sent at 16:05:12.535. Not the newest workout's timestamp: a workout
created between the query and the reply would fall in the gap between the
two and never be asked for again. Both numbers arrive as JSON **strings**,
not numbers.

`metadata.workoutcount` was 7, for 7 returned out of a possible 50 - which
is exactly as consistent with "all matches" as with "this page", and the
app only ever asked for one page. So this client carries it for diagnostics
and pages off a full page coming back, as before.

**Confirmed on the phone, 2026-10-05.** The first sync after the change
still fetched twelve pages, because the stored cursor starts at zero; the
next one finished almost immediately, which is the cursor surviving a write
and a read and the server agreeing that nothing is newer. Progress stayed
at the official app's own 17 / 12 / +4 across both, so the shorter request
is not a shorter answer.

**Implemented**: `CloudAccount.workoutCursor` (unix ms, its own column,
*not* `lastSync` - that one is this phone's clock and the comparison happens
on the server's), stored only when a sync finishes, and only ever moved
forward. The first page's `until` is the one stored, not the last page's,
for the same gap reason. Settings has "Fetch all workouts again" for when
the local copy is suspect rather than merely stale.

### There is no Progress endpoint

This is the question the capture was taken to answer, and the answer is a
negative worth having. With the home screen's eleven widgets rendered and
every page scrolled, the official app fetched:

- `workouts?since=…` (7 workouts),
- `247.sports-tracker.com/v1/{activity,recovery,sleep,sleepstages}/export`,
- `GET /apiserver/v2/personal/best/records?statsVersion=V2&tz=Europe/Helsinki`,
- and configuration: `/v1/user/settings`, `/v1/user/appconfig`.

Nothing that returns a fitness, fatigue or form figure. `/v1/user/appconfig`
confirms what the widgets even are - the server sends the home screen's
layout, `FitnessProgressTrend` and `TrainingTrend` among them - but sends no
data for them. **So CTL, ATL, TSB and the "Kunto pysyy samana" verdict are
computed on the device, from the per-workout stress scores in the local
database.** Which is what this app does, and why the three figures matched
exactly rather than approximately.

The relevant settings the app computes against, for the record:
`hr_max: 192`, `hr_rest: 45`, `hr_threshold_2: null`,
`preferredTssCalculationMethods: null`, `intensityZones.heartRateZoneType:
"max"`, and goals (`weeklyTrainingDuration: 10800`, `dailySteps: 8000`).

### `tssList`: two scores per workout, and the list field is one of them

```json
"tss":     {"calculationMethod":"PACE","trainingStressScore":10.29,
            "intensityFactor":0.194,"averageGradeAdjustedPace":1.87},
"tssList": [ {...PACE, 10.29...}, {"calculationMethod":"MET",
             "trainingStressScore":31.12} ]
```

The two methods disagree by a factor of three, so which one the load is
built from is not a detail. `tss` is the PACE one here, and `tss` is what
this app reads - confirmed correct by the match against the official app's
own 15 / 5 / +9 rather than by reasoning about it.

### Another new endpoint: `GET /v2/personal/best/records`

```
GET /apiserver/v2/personal/best/records?statsVersion=V2&tz=Europe%2FHelsinki
```

Note the **v2** - a v1 of this path was not observed and is not assumed to
exist. The timezone is not decoration: a year boundary decides what counts
as "this year", and the app sends the phone's own IANA zone, slash
percent-encoded.

The payload is an array, one entry per activity:

```json
{ "activityId": 1,
  "types": ["LongestDistance","FastestPace","KM5","KM10",
            "HalfMarathon","FullMarathon"],
  "records":  { "KM5": {"type":"KM5","value":1552.9,
                        "workoutKey":"5b34fef3b99d960f7b252e21",
                        "date":1445090887000}, ... },
  "thisYear": { ... same shape, this year's bests ... } }
```

`types` is the only ordering information in the response, and it matters:
`records` is a JSON object, and reading its keys back alphabetically puts
the marathon before the 5 km.

**Units: partly established, and deliberately not guessed for the rest.**

- **Durations** - `LongestDuration`, `KM5`, `KM10`, `KM20`, `KM40`,
  `KM180`, `HalfMarathon`, `FullMarathon` - are **seconds**. This is
  arithmetic rather than assumption: 5 km in 1552.9 s is 5:11/km and
  10 km in 3728.5 s is 6:13/km, a correctly-ordered pair that no other
  unit produces.
- **`LongestDistance`** is **metres** (10130.0 for a run), matching the
  workout list's own `totalDistance`.
- **`MaxAscent`** is metres by the same reading.
- **`FastestPace`, `MaxSpeed` and `MaxAvgSpeed` are all metres per
  second** - measured 2026-10-06, and `FastestPace` is a *speed* despite
  its name. Each record also carries the `date` of the workout it was set
  in, and that date joins straight onto a local workout's `start_time`, so
  no identifier was needed: seven records were compared against their own
  workout's distance over its time, and all seven matched to three decimal
  places.

  ```
  FastestPace act1 this year   2.64        d/t = 2.643 m/s
  FastestPace act1 all time    3.22        d/t = 3.218 m/s
  MaxAvgSpeed act2 this year   6.44        d/t = 6.440 m/s
  MaxAvgSpeed act2 all time    7.42        d/t = 7.416 m/s
  MaxSpeed    act2 this year  65.04847     max_speed 65.04848
  MaxSpeed    act2 all time  100.0         max_speed 100.0
  MaxSpeed    act10 all time   8.38        max_speed 8.38
  ```

  This also settles the contradiction that made the question look hard:
  3.22 all-time against 2.64 this year is correctly ordered once bigger
  means faster. The three `MaxSpeed` values are the workout list's own
  `maxSpeed` field exactly, which this client already treats as m/s.

  So `FastestPace` is shown as minutes per kilometre and the other two as
  km/h - both exact conversions of the same measured figure, read the way
  the activity usually is.

  **The absurd ones are bad data, not a bad unit.** 100.0 m/s is 360 km/h
  and 65.05 m/s is 234 km/h; both are single-sample GPS glitches that the
  cloud has faithfully recorded as records, and the official app shows the
  same. Nothing here clamps them: a record the cloud holds is what this
  page is for.

- **`MaxAvgPower` is still shown bare.** There is no power meter on this
  account, so every value of it is null and there was nothing to check
  against. Watts is the obvious guess, which is why it is not made.

### A new endpoint: `POST /v1/workout/extensions/<workoutKey>`

Not a GET. The body is a JSON array of the extension types wanted:

```json
["SummaryExtension","FitnessExtension","SkiExtension","IntensityExtension",
 "DiveHeaderExtension","SwimmingHeaderExtension","WeatherExtension",
 "WeatherStreamExtension","JumpRopeExtension"]
```

The app fires one per workout on the list - 50 of them at startup. What
comes back is everything the list response does not carry:

- **FitnessExtension**: `maxHeartRate`, `vo2Max`, `estimatedVo2Max`,
  `fitnessAge`.
- **IntensityExtension**: `zones.heartRate.zone1..5` as
  `{totalTime, lowerLimit}`, so the zone boundaries come from the server
  rather than being derived from `hr_max`.
- **SummaryExtension**: `pte`, `peakEpoc`, `recoveryTime`, `ascentTime`,
  `descentTime`, min/avg/max temperature **in kelvin** (299.7, 300.6,
  303.3), `heartRateRecovery`, and `gear` - manufacturer, `displayName`,
  `serialNumber`, `softwareVersion`, `hardwareVersion`. Also `apps`: the
  SuuntoPlus app outputs, with localised names ("Rasva", "Hiilihydraatit").

`{"extensions": []}` for a workout that has none - an empty list is a 200,
so an absent extension document is not what the 403s below are.

**Wired up, 2026-10-05.** `loadCloudDetails()` now chains this after the
detail GET and merges both into the same flattened field map, so there is
one write and one signal. Either half may fail without losing the other;
the extensions half failing is logged rather than shown, because the page
is already drawn and this is an enrichment.

The nine type names are sent verbatim and in the captured order. A subset
would very likely work - the server is being told what to include, not
matched against a signature - but there is no measurement saying so, and
sending what was observed costs nothing.

VO2max (`estimatedVo2Max`, the decimal one; `vo2Max` is the same figure
rounded, kept as a fallback), the fitness age and the five heart-rate zones
are new on the workout page. The zone *boundaries* matter more than they
look: they come from the server rather than being derived from the
configured maximum, and 0/131/146/161/176 against an `hr_max` of 192 is not
an even division of anything. They are also **per workout**, not per
account - another on the same phone reads 0/126/135/144/152 - so there was
never a single set to cache and reuse. EPOC, PTE and the recovery time were already
arriving from the detail GET, so those rows are unchanged.

**Confirmed on hardware, 2026-10-05**: a cloud workout shows VO2max, the
fitness age and the five zones with time in each.

One thing the flattening still drops: `gear`, whose fields are strings, and
`flattenJson()` only keeps numbers and booleans. The recording watch's
serial and firmware version are therefore captured and documented but not
displayed.

### Nine workouts answer 403: deleted test uploads (answered)

Of 50 unique workout keys, 41 answered 200 and **nine answered
`403 Forbidden`**, retried and refused again. Their ObjectId timestamps -
the server's own creation time, in the key's first four bytes - cluster in
evening pairs: 22.9 21:29-21:39, 23.9 21:10 and 21:35, 25.9 21:17. Those
are the evenings this project's upload path was being tested, and the
200-answering keys are bulk bursts from the official app instead.

**Confirmed 2026-10-06**: they are workouts this app uploaded during that
testing, which Jarno then deleted from the account. The official Android app
kept local stubs of them, still asks for their extensions on every start,
and gets 403 - not 404 - for a workout that is gone.

So the 403s say nothing about whether this app's uploads are well formed,
which is what the question was really about. They are the official app
asking after the dead.

**Two attempts to settle this, both recorded because both were wrong about
something.**

The first version of this section said no upload here ever kept the key the
server returned. It does - `WorkoutStore::markSmlUploaded()` writes
`workout_sml.uploaded_key`. So the second version said the question was one
query away. It is not, and the query is what showed why.

The phone's database has exactly **one** upload on record, and the key it
kept is `6vuh9b4s5jlblmhi` - sixteen characters, not a twenty-four-digit
ObjectId. Which brings out something this project had backwards:

### `key` and `workoutKey` are two different identifiers, and not the way round it looks

```json
"key":        "29sc0l5ro6vvnfpo",
"workoutKey": "6ac39c973e7e17771cad991b"
```

`key` is the short one. `workoutKey` is the ObjectId - whose first four
bytes are a unix timestamp, which is where the creation times used above
come from. An earlier pass through this capture printed `workoutKey` under
the label "key" and reasoned from that; the mix-up is recorded rather than
quietly fixed because the two look nothing alike and the error was in the
labelling, not the data.

This client stores `key`, the short one, and the upload returns one of
those too. So `uploaded_key` could not be compared with the nine ObjectIds
at all. `Workout::workoutKey` was added for exactly this, and with it
filled in the nine were still absent from all 1171 workouts - which looked
like the hypothesis collapsing, and was in fact only the consequence of
their having been deleted. Three steps to an answer, two of them wrong
about why: the first about what was stored, the second about what absence
from the list meant.

**Measured while looking**: `POST /v1/workout/extensions/<key>` accepts the
**short** key, not only the ObjectId the official app sends. Nine of the
thirteen detail rows on the phone carry a `FitnessExtension` and an
`IntensityExtension` fetched that way. Worth having written down, because
the capture alone would suggest the ObjectId is required.

## Still open

1. **The rest of a lap window.** Nine `{Avg, Max, Min}` groups and a dozen
   scalars per lap, of which four fields are read. Nothing needs them yet.
2. **`workoutBinary`'s exact field order** - `HeaderSerializer.c`,
   `ServiceHeaderSerializer.b` and the four sub-serializers. Readable, not
   read. **Not needed**: a watch-recorded upload sends SML instead, and
   that path works. This stays documented in case a phone-recorded workout
   ever matters.
3. **Whether omitting nil readings matters.** The upload succeeds without
   them, so the difference is confirmed harmless - but it has never been
   tested the other way.
4. **`Header.TraingingLoadPeak`** is in the descriptor table but has never
   been observed non-zero on this watch.
5. **`MaxAvgPower`'s unit** - and this one will stay open. It needs a
   workout recorded with a power meter, and there is no power meter here to
   record one with. Watts is the obvious guess; the records page prints the
   figure bare rather than making it, and that is the end state rather than
   a placeholder.
6. **`FitnessExtension` does not always carry VO2max and the fitness age** -
   and this one is answered rather than open. The captured workout has
   `vo2Max`, `estimatedVo2Max` and `fitnessAge`; nine real ones on the phone
   carry only `maxHeartRate`. Jarno's explanation, which the data cannot
   give: those nine came into the Suunto cloud from a *Garmin* account
   through SyncMyTracks, so Suunto never computed its own fitness analysis
   for them. Nothing was measured on a Suunto watch, so there is nothing to
   report.

   Worth knowing because it changes what an absent row means. It is not a
   gap in this client, nor a field the cloud sometimes omits: it is a
   workout Suunto's own analysis never ran on. The "Recorded with" line
   added at the same time is the visible counterpart - a workout whose gear
   is not one of the two watches here is one that came in from somewhere
   else.

## Setting up HTTPS interception again (2026-09-25)

Written down because it was not, last time, and that cost an evening: the
2026-09-22 capture recorded two values only as `https://api.sports-
tracker.com/apiserver...` and `WatchUserKey A2d6...`, and by the time the
full values were wanted the capture was gone and the CA mount had died
with a reboot.

The device is a rooted LineageOS 21 (Android 14) phone with Magisk. The
system CA store lives in the Conscrypt APEX, which is mounted read-only
and, worse, is mounted per-namespace - so it is not enough to change it
once, it has to be changed inside every namespace whose processes should
see it.

```sh
# 1. The CA, named by its subject hash. openssl -subject_hash_old, not
#    -subject_hash: Android wants the old algorithm.
openssl x509 -inform PEM -subject_hash_old -in ~/.mitmproxy/mitmproxy-ca-cert.cer -noout
cp ~/.mitmproxy/mitmproxy-ca-cert.cer <hash>.0
adb push <hash>.0 /data/local/tmp/

# 2. Stage a full copy of the store plus the new certificate. The SELinux
#    label matters as much as the mode; without it apps get an empty store.
adb shell su -c '
  rm -rf /data/local/tmp/cacerts && mkdir -p /data/local/tmp/cacerts
  cp /apex/com.android.conscrypt/cacerts/* /data/local/tmp/cacerts/
  cp /data/local/tmp/<hash>.0 /data/local/tmp/cacerts/
  chown root:root /data/local/tmp/cacerts/*
  chmod 644 /data/local/tmp/cacerts/*
  chcon u:object_r:system_file:s0 /data/local/tmp/cacerts/*'

# 3. Bind it in, in init's namespace and in every zygote - apps fork from
#    zygote, so that is the one that decides what new processes see.
adb shell su -c '
  for p in 1 $(pgrep -f zygote); do
    nsenter --mount=/proc/$p/ns/mnt -- mount --bind /data/local/tmp/cacerts /apex/com.android.conscrypt/cacerts
  done'

# 4. Proxy over USB rather than the network: no IP to get wrong, and it
#    stops working the moment the cable is out, which is a feature.
mitmdump --listen-port 8080 -w capture.flows &
adb reverse tcp:8080 tcp:8080
adb shell su -c 'settings put global http_proxy 127.0.0.1:8080'

# 5. Apps already running kept the old namespace. Restart the one you want.
adb shell su -c 'am force-stop com.stt.android.suunto'
```

**Check it with an app, not with `curl`.** `curl` on the device carries its
own CA bundle and fails with `ssl_verify_result=20` no matter how correct
the system store is; that failure means nothing. What means something is a
real app getting a 200 through the proxy - `api.sports-tracker.com`
answering 200 is the proof that the Suunto app is interceptable and does
not pin.

**Undo**, in the order that leaves the phone working:

```sh
adb shell su -c 'settings put global http_proxy :0'
adb reverse --remove tcp:8080
```

Everything else is a tmpfs bind mount and disappears on reboot. Leaving the
proxy set while the cable is out, or across a reboot, breaks networking for
every app on the phone until somebody notices - so unset it before walking
away, even if the capture is not finished.
