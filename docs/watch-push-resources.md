# Pushing data *to* the watch: health, GPS and weather

Scouting notes for items 3-6 of the 2026-09-22 list. Nothing here is
implemented; this is what is known and, more usefully, what is still
missing and how to get it.

## Item 3 - reading health data from the watch over BLE

Two MDS resources exist, from the APK's own string table:

```
suunto://MDS/Sleep/%s/Entries
suunto://MDS/Activity/%s/Entries
```

`%s` is the watch serial, the same substitution `/Logbook/%s/Entries` uses.
So the addressing is familiar; what isn't known is the response structure.

This matters less than it looks. `/Logbook/Entries` took three attempts and
a decompilation pass to crack, because its response is a `protocol_v9`
structure walk rather than a payload (see docs/logbook-data-format.md).
These two are very likely the same shape - the same `SDS::WB::Sync<>::op()`
serves every Logbook verb - but "very likely" is exactly the kind of claim
this project has been wrong about before, and the only way to settle it is
to call them on real hardware and look.

**What would settle it**: `AppController::testEntriesFetch()` already does
the handle-fetch dance for `/Logbook/Entries`. Pointing the same code at
`/Sleep/<serial>/Entries` is a one-line probe, and whatever comes back -
data, an error, or a timeout - narrows it immediately. That needs the watch
in hand.

Worth noting: the cloud already has this data (item 1, now implemented), so
reading it from the watch directly is an improvement rather than the only
route - it would mean sleep without a Suunto account round-trip.

### What the probe settled (2026-09-23)

Ran on real hardware. Three paths, one request each, same code path:

| path | bytes | first byte |
|---|---|---|
| `/Logbook/Entries` (control) | 170 | `f0` |
| `/Sleep/Entries` | 6 | `f5` |
| `/Sleep/<serial>/Entries` | 6 | `f5` |

So **`f5 01 00 80 00 00` is this protocol's error reply** - six bytes, and
identical for two different resources. `f0` is the success marker, the same
one the request itself carries. Worth knowing generally: a short `f5` body
means the request was rejected, not that the resource is empty.

Then the APK said why. `SpartanBt.fetchSleepSamples(long since)`:

```java
path = String.format("suunto://MDS/Sleep/%s/Entries", watchBt.getSerial())
body = moshi.adapter(SuuntoSleepDataContract.class)
            .toJson(new SuuntoSleepDataContract(since))
MdsRx.getWithHeader(path, body)
```

Two corrections to what this doc assumed:

1. **`%s` really is the serial.** The original path spelling was right; the
   serial-less guess (reasoning by analogy from `/Logbook/Entries`) was
   wrong.
2. **The request carries a JSON body.** `getWithHeader`, not a plain GET.
   `SuuntoSleepDataContract` is a single `long`, and its Moshi adapter
   names the field **`NewerThan`** - so the body is
   `{"NewerThan": <timestamp>}`, the incremental cursor.

`/Logbook/Entries` needs no body, which is why the existing code never had
to send one.

**What is still missing**: how a body is framed on the wire. This
project's `Mds::encodeGetRequest()` builds
`[0x01 verb][0x80 0x00][PATHLEN][path]` and stops there; where the JSON
goes after that - appended, length-prefixed, or a different message type
entirely - is not recoverable from the APK, because `getWithHeader` is
implemented inside `libmds.so`.

The cheapest way to find out is a `btmon` capture on the S7 while the
official app syncs, since that device already has the app and the watch
paired. One sleep fetch in that trace shows the exact bytes, the same way
the original capture cracked `/Logbook/Entries`. Decompiling `libmds.so`'s
`getWithHeader` is the fallback, and slower.

## The 2026-09-23 HCI capture: all three answered

A full `btsnoop_hci.log` taken on the S7 during one official-app sync,
decoded into 1284 Whiteboard frames. It settles items 3, 4 and 5 at once,
and each answer is different from what this doc had assumed.

Frame types seen: `0x0a`/`0x02` GET+ack (100), `0x0d`/`0x05` handle fetch
(363), `0x10`/`0x08` stream start (14), **`0x0e`/`0x07` PUT (161)**,
`0x01`/`0x0b`/`0x03` bulk. `0x0e` is the write verb this project has never
needed until now.

### Item 3: sleep is a file, not a resource

`/Sleep/<serial>/Entries` is an MDS-library abstraction on the Android
side. Over BLE the exchange is:

```
GET  /Daily/Sleep/Timeline/Data     -> ack
0x0d handle fetch, body "mdsSlp.sbem"   (watch writes the file)
GET  /Dev/FileSystem/Stream         -> ack
0x0d reads at increasing offsets, body "mdsSlp.sbm" + offset
     -> payload begins "SBEM0102", contains "suunto-247-sleep-<serial>"
GET  /Dev/FileSystem/FileDelete     -> cleanup
0x0e "mdsSlp.sbem"
```

So the watch renders its sleep timeline into a temporary file and the app
streams it off, then deletes it. Note the container is **SBEM0102**, not
the SBEM0103 a workout uses - close, but the version differs and shouldn't
be assumed identical.

The same run also shows `/Activity/Moments/Sync/Data` and
`/Activity/TrendData` for daily activity, and `/Activity/TrainingLab/
StressBalance` - the recovery figure.

### Recovery, and what activity is not (2026-09-24)

`/Activity/Moments/Sync/Data` is the **recovery** series, and it needs no
rendered file - the reply carries the records directly, in the same paged
framing `/Summary` uses. The request is a one-parameter fetch whose cursor
is unix **seconds**, where the sleep fetch passes milliseconds. Same type
code, different unit.

Eight bytes per record, cross-checked against the cloud's own entries for
the same half-hours:

| bytes | meaning |
|---|---|
| 0-3 | timestamp, unix seconds |
| 4 | resource balance, percent (cloud reports 0..1) |
| 5 | stress state |
| 6-7 | vary between records; unidentified |

The cloud says `21:30+03:00 -> balance 0.72, stressState 1`; the watch says
`18:30 UTC -> [72, 1]`.

Two things the decoder had to handle that the format does not announce:

- The reply has eleven bytes before the record array. Rather than hard-code
  that from one sample, it finds the start by looking for two consecutive
  plausible timestamps **exactly half an hour apart**. Plausibility alone
  locked on two bytes early, because four bytes straddling a record
  boundary happened to read as a number in range.
- Unused trailing entries are zero-filled, which would otherwise decode as
  1970.

**Daily activity is still open.** `mdsAct.sbm`, guessed by analogy with the
sleep file, does not exist - the capture contains only `mdsSlp.sbm` and
`sysevt.sbm`. `/Activity/TrendData` returns a different structure with no
unix timestamps in it (a longer-term trend, not the ten-minute series), and
the ten-minute step/energy/HR series that the app POSTs to the cloud was
not seen coming off the watch in this capture at all. It may need a
resource that only appears when there is unsynced activity, which there was
not - the watch had been synced the day before.

### Daily activity: `libmds.so` answers it (2026-09-25)

**The paragraph above is wrong about `/Activity/TrendData`**, and the
reason it was wrong is worth keeping. That reading came from one capture in
which the watch had nothing unsynced to send. libmds.so has a specific
error string for exactly that case - `"Empty array was returned"` - so what
was read as "a different structure with no timestamps in it" was an empty
reply. An empty array has no timestamps in it either.

`SDS::Activity` is the class behind `suunto://MDS/Activity/<serial>/
Entries`, and its four methods say the whole mechanism:

| symbol | what the disassembly shows |
|---|---|
| `SDS::Activity::get` | reads `NewerThan` out of the request body, **multiplies it by 1000**, and calls `getFragment` in a loop |
| `SDS::Activity::getFragment` | builds `"/net/" + serial + "/Activity/TrendData"`, sends one parameter named `timestamp`, 3000 ms timeout |
| `SDS::Activity::entriesToJson` | emits `Timestamp`, `Steps`, `Energy`, `TimeISO8601` |
| `SDS::Activity::getTimeZoneOffset` | reads `/net/<serial>/Timezone/...`, which is how `TimeISO8601` gets its offset |

So:

- **The resource is `/Activity/TrendData`** - the same path the earlier
  note dismissed.
- **The cursor is milliseconds.** `NewerThan` arrives in seconds from the
  app and is multiplied by 1000 before the fetch. Sleep counts
  milliseconds, recovery counts seconds, and this one counts milliseconds.
  That is three neighbouring resources and two different units, which is
  why the probe tries both rather than picking one.
- **The reply carries records directly**, like recovery and unlike sleep -
  one whiteboard op, no rendered file, no filesystem paging.
- **It is fragmented, and the continue code is not the usual one.** Status
  `200` is the last fragment; status **`202`** means call again. Every
  other paged resource here uses `100` for continue, so `readPages()`
  would treat `202` as "done" and quietly return the first fragment as if
  it were everything. Not a subtle failure to hunt for later: a short
  answer that looks complete.
- **The next cursor is taken from an entry of the fragment just received**,
  not from a byte offset. Which entry, and which of its fields, is not
  readable with confidence from the assembly - the list-splice code reads
  a field at +8 in the first node - so pagination is deliberately not
  implemented on a guess.

What the disassembly does **not** give is the parameter's wire width, or
the record layout: both are decided by the watch's own metadata, which the
JSON layer in between hides. `AppController::testActivityTrendFetch()`
tries int64-ms, int64-s and int32-s in turn, stops at the first the watch
does not answer with the six-byte `f5` rejection, and writes the whole
reply - paged header included, because the status word in it is half the
point - to `<cache>/trenddata.bin`.

There is deliberately no int32-milliseconds attempt: epoch milliseconds do
not fit in 32 bits, so it could only send a truncated cursor, and the risk
is not that it fails but that the watch answers anyway and the sweep
reports a working encoding that is asking for the wrong window.

**Ground truth for the decode is already in hand**: the cloud's own
`/v1/activity/export` carries the same ten-minute series
(`{hr, stepCount, energyConsumption}` per entry, see
`cloud_247_v1_activity.json`), so the watch's records can be checked
against the cloud's figures for the same ten minutes - the same way the
recovery decoder was.

One thing the cloud series has that `entriesToJson` does not: **`hr`**. The
MDS layer emits only Timestamp/Steps/Energy, so either the watch's record
carries a heart rate that libmds drops, or the app fills it from somewhere
else. Unresolved, and worth looking for in the raw bytes.

### The bytes, off a real watch (2026-09-25)

The probe ran on Jarno's Race the same day. Everything above held, and the
one open question answered itself.

- **The cursor is int64 milliseconds**, accepted first try.
- **The cursor is not a strict lower bound**, and the first reading of this
  got it wrong. The probe asked for `1790157250530`, which is 09:54:10.530
  UTC; the reply's first record is 09:00:00.480, fifty-four minutes
  *earlier*, and the last is 10:30. So the reply straddles the cursor.
  Whether the watch rounds the cursor down, or returns whole blocks of ten
  and the cursor picks the block, cannot be told from one sample - and the
  coincidence that made "inclusive" look obvious was only that both
  numbers start `09:`. Not established; do not build on it.
- **Status 202**, in the same halfword `/Summary`'s 100/200 lives in.
  Confirmed on the wire, not just in the disassembly.
- **426 bytes: a 26-byte header and ten 40-byte records**, ten minutes
  apart. Every byte accounted for.
- **The heart rate is there** - libmds simply drops it. It is one byte, in
  **bpm**, and the cloud is what divides by sixty. That also closes a
  question `docs/` had deliberately left open: the watch's own unit is
  beats per minute, and the hertz in the cloud's JSON is the cloud's doing.

```
 0..3    float32  energy      (the cloud's energyConsumption, same unit)
 4..5    uint16   steps
 6..7    uint16   zero in every record seen
 8..15   uint64   timestamp, unix MILLISECONDS
16       uint8    0x01 in every record seen
17       uint8    heart rate, BPM
18..39   ?        varies, sometimes all zero, often looks like stale buffer
```

Cross-checked against the cloud's own entries for the same ten buckets:
**thirty values, thirty matches** - `92109.625` came back to the
fraction. `tests/test_activitydecoder.cpp` reads its expected values out
of `cloud_247_v1_activity_trend.json` rather than having them typed in, so
the check is against something that has never seen this decoder.

One detail that is not cosmetic: **the watch's timestamps are not on the
ten-minute mark** - `:00.480`, `:00.030`, `:00.200`. The cloud's are,
exactly. `ActivityTrend::decode()` therefore snaps to the grid, because
otherwise the same ten minutes read from the watch and read from the cloud
would be two different rows of `health_entries` and would show up twice.
The raw value is kept alongside for the fetch loop, which has to advance
its cursor *past* the last record: advancing by the snapped value would ask
for an instant slightly before a record already in hand, and get it back
forever.

**Pagination** is implemented from the 202 marker plus a cursor of
`last raw timestamp + 1`, with a stop on any of: status not 202, an empty
fragment, a cursor that did not advance, or 250 fragments.

**Confirmed end to end on the Race, same day.** A real sync pulled **79
consecutive buckets, 00:00 through 13:00 with no gaps** - roughly eight
fragments, so the loop ran and terminated on its own. Cross-checked against
the watch's own screen rather than against the cloud this time:

| | app | watch |
|---|---|---|
| steps today | 1553 | 1553 |
| energy today | 410306 J / 4184 = 98.1 | 98 kcal |

That also settles the **energy unit: joules**. The 4184 divisor was
previously an inference from the cloud's numbers; a day's worth of watch
bytes landing on the watch's own kcal figure is a second, independent
source for it.

Every timestamp landed on the ten-minute grid and none was duplicated, so
the snapping works.

Still unidentified: bytes 18..39, and whether the `0x01` at 16 is a record
type or a validity flag.

### Item 4: the watch downloads its own ephemeris

There is no ephemeris blob on the BLE link at all. What the app does is
hand the watch credentials and let it fetch over WiFi:

```
GET  /Settings/Wifi/Cloud/OfflineMaps/Url     -> ack
0x0e "https://api.sports-tracker.com/apiserver..."
GET  /Device/GNSS/ExtendedEphemerisData/Date  -> "2026-09-23T00:00:00"
GET  /Settings/Wifi/Cloud/STTAuthorization    -> ack
0x0e "WatchUserKey A2d6..."
```

"Syncing GPS performance" on the watch's screen is this handshake. The
Date read is the freshness check - it already said today's date, which is
why nothing was downloaded. **This is much less work than expected**: no
ephemeris format to decode, no large transfer to implement. Writing the
URL and the session key is two PUTs.

### Item 5: weather, partially

`GET /Weather/Sync` appears, followed by a `0x10` stream start and a short
`0x08` reply - but no forecast payload. Most likely the same
fetch-it-yourself pattern as the ephemeris, given the cloud credentials
were just written, but that is an inference and not established: a
`WeatherUpdateModel.updateWeatherToDevice` exists on the app side, so a
direct push may happen when the forecast is actually stale. Worth one more
capture with the weather view opened and the watch's forecast expired.

### What is needed to implement any of this

`0x0e` (PUT) is unimplemented here - `mdswirecodec.cpp` only encodes GET,
handle fetch, stream start/stop. The capture has 161 real PUT frames to
build it against, which is the same position the GET work started from.

Note the capture is **not checked in**: it contains the account's
`STTAuthorization` value in clear.

## Item 4 - how the watch's GPS data is updated

This is **extended ephemeris**, i.e. A-GPS: a few days of predicted
satellite orbits, so the watch gets a fix in seconds instead of minutes.
Three resources name it:

```
suunto://MDS/GNSS/%s/EphemerisData
suunto://%s/Device/GNSS/ExtendedEphemerisData/Date
suunto://%s/Device/GNSS/ExtendedEphemerisData/Format
```

The Date/Format pair reads what the watch currently holds; `EphemerisData`
is where the new blob goes. The app side is
`com.suunto.connectivity.location.*` (`GnssDate`, `GnssTime`,
`getLatestGnssVersion`, `getGnssVersionPath`, and the log line "No GNSS
version fetched, force an update from the cloud").

**Not in the capture.** The 2026-09-22 watch sync fetched SuuntoPlus
features, sport modes (a 2.2 MB zip from sportmode.sports-tracker.com) and
a firmware note - but no ephemeris. That is expected: ephemeris is good for
days, so it updates on its own schedule rather than every sync. The
download URL is built at runtime rather than stored as a string, so it
isn't recoverable from the APK's string table either.

## Item 5 - how the watch's weather is updated

The source is **OpenWeatherMap**, through the app's own API surface:

```
GET weather/v2/combined        OpenWeatherMapRestApiV2.getForecastWeatherV2AtLocation
GET weather/combined           OpenWeatherMapRestApiV2.getForecastWeatherAtLocation
GET find                       OpenWeatherMapRestApi.getWeatherConditionsAtLocation
GET onecall/timemachine        OpenWeatherMapRestApi.getHistoricalWeatherConditionsAtLocation
```

The push side is a proper sync step, not an ad-hoc write:
`WeatherResource.sync(WatchBt, builder)` calls
`WeatherUpdateModel.updateWeatherToDevice(WatchBt, ...)` and reports through
`SpartanSyncResult.Builder.weatherResult(...)`, with "Weather sync failed"
as its error. There is also a `WatchWeatherVersion`, so the watch tracks
which forecast it already has.

**Also not in the capture**, and for the same reason - the sync that was
recorded didn't include a weather refresh. `WeatherUpdateModel`'s own
methods carry no path strings, so the MDS resource it writes to is built at
runtime.

## Item 6 - implementing either

Blocked on the same thing both times: **a capture that contains the
exchange**. Neither is recoverable by reading the APK alone, because both
build their URLs and resource paths at runtime.

What would unblock it, in rough order of effort:

1. **A longer capture.** Leave mitmproxy running across a watch sync where
   the watch has been off the app for a day or more, so the ephemeris is
   stale enough to refresh. Opening the app's weather view during the same
   session should force the weather path too.
2. **A `btmon` capture alongside it**, since the HTTPS side only shows the
   download; the write to the watch is BLE and needs the same treatment the
   Logbook work got.
3. Failing both, decompiling `WeatherUpdateModel.updateWeatherToDevice` and
   the GNSS equivalent properly with Ghidra, the way `protocol_v9` was
   done - slower, and it still ends in a hardware test.

Until then this is scouting, not a plan. Worth saying plainly: items 4-6
were the ones I could make least progress on tonight, and it is the capture
that is missing rather than the analysis.


## The 2026-09-25 capture: items 4 and 5 settled, and three decoders checked

A btsnoop log taken on the S7 during one ordinary Suunto-app sync with the
Race. 1325 Whiteboard frames, 49 GET paths, 159 PUTs. The watch showed
"syncing SuuntoPlus", "optimizing GPS performance", "syncing activities"
and "weather" in that order, so the whole sequence is in there.

The log is **not** in this repository - it carries the account's
STTAuthorization value in clear. It is in `suuntosync-fixtures-private/`
as `btsnoop_race_sync_2026-09-25.log`, and `tools/` has no reader for it
because the analysis was ad hoc; the framing is in this document.

### Item 4: the ephemeris handshake, in full

Both values the earlier note had truncated:

```
GET  /Settings/Wifi/Cloud/OfflineMaps/Url
0x0e "https://api.sports-tracker.com/apiserver/"     (the whole value)
GET  /Device/GNSS/ExtendedEphemerisData/Date
GET  /Settings/Wifi/Cloud/STTAuthorization
0x0e "WatchUserKey <16 characters>"
```

So implementing this is two `putString()` calls, which already exist.

**Open**: where the 16-character token comes from. It is not the account's
userKey - that is 32 characters and a different value - and it does not
appear in the 2026-09-22 HTTPS capture. Until that is known, this cannot
be implemented for a fresh account, only replayed. The next HTTPS capture
during a watch sync should show which endpoint mints it.

### Item 5: the watch fetches its own weather

`GET /Weather/Sync` again produced an ack, a `0x10` stream start and a
short `0x08` reply - **no forecast payload**, on a second capture now. What
settles the mechanism is the watch's own event log, read back over
`/Analytics/Data` during the same sync:

```
Weather sync TO
Weather sync ok d:0 f:0
Weather sync ok d:60 f:0
Weather sync ok d:1500 f:0
Weather sync ok d:3600 f:0
Weather sync ok d:5640 f:1
Weather sync ok d:17760 f:5
synced city:Tampere
```

The watch logs its own weather syncs, knows the city name, and records
timeouts - which is not the shape of a phone pushing it a blob. Same
pattern as the ephemeris: the phone hands over cloud credentials and the
watch fetches over WiFi. `d:` and `f:` are unidentified; the increasing
`d:` values look like the age of the forecast being replaced.

This means weather and GPS are **one feature, not two**: write the URL and
the STTAuthorization, and the watch does the rest. It also means the
16-character token above is the blocker for both.

### The health requests, checked byte for byte

The same capture carries the official app's own requests for the three
resources this project implemented from inference. Every one matches:

| resource | type code | unit | matches |
|---|---|---|---|
| `/Activity/TrendData` | `0x0008` int64 | **milliseconds** | `fetchActivityTrend()` |
| `/Activity/Moments/Sync/Data` | `0x0008` int64 | **seconds** | `fetchRecoveryMoments()` |
| `/Daily/Sleep/Timeline/Data` | `0x0008` int64 + `0x000c` string | milliseconds + `mdsSlp.sbm` | `fetchTimelineFile()` |

The same type code carrying seconds for one resource and milliseconds for
its neighbour is not a misreading after all - the app does exactly that.

### And the activity pagination, confirmed

The app made **29** consecutive `/Activity/TrendData` fetches, each reply
426 bytes: a 26-byte header, ten 40-byte records, status **202**. Every
cursor after the first is the previous reply's **last record's raw
timestamp**, jitter included - `...800090`, `...800270`, `...800100`.

The cursor is **exclusive**: with cursor 19:30:00.090 the next reply starts
at 19:40:00.340, one interval later. This project passes last + 1 ms, which
lands in the same place, so the extra millisecond costs nothing.

One thing the first request shows that the others do not: given a cursor
that is *not* one of the watch's own record timestamps, the reply starts
*before* it - 18:20:00.000 returned records from 18:00:00.740, and an
earlier probe with 09:54:10.530 got 09:00:00.480. Both round down to the
hour, which is a guess from two samples and not worth relying on. Re-read
buckets are harmless: `health_entries` upserts on (kind, timestamp).


## The 9 Baro capture: the ephemeris push, in full (2026-09-25)

Jarno's observation is what produced this: **the Suunto 9 Baro has no
WiFi**, so it cannot do what the Race does. The data has to come over BLE,
and that is the mechanism this project can actually implement.

`libmds.so` names it before the capture even confirms it:
`OBI2::LegacyDeviceGNSS::putEphemerisData` - "legacy device" being exactly
this generation.

The capture is `suuntosync-fixtures-private/btsnoop_9baro_sync_2026-09-25.log`
(1.4 MB, 2490 Whiteboard frames on handles 0x000e/0x0010 - the Baro uses a
different GATT layout from the Race's 0x0012/0x0015, so a capture holding
both watches must be split by handle before anything is reassembled).

**Not one `/Settings/Wifi/Cloud/*` path appears.** The whole
fetch-it-yourself handshake the Race does is simply absent, replaced by:

```
GET  /Device/GNSS/ExtendedEphemerisData/Date     -> "N/A"
GET  /Device/GNSS/NavigationSystem
GET  /Device/GNSS/ExtendedEphemerisData/Format   -> enum value 2
GET  /Device/GNSS/ExtendedEphemerisData/Upload/0 -> ack (handle)
0x0e PUT, no parameters                              "begin"
0x0e PUT x136, one chunk each
GET  /Device/GNSS/ExtendedEphemerisData/Load     -> ack (handle)
0x0e PUT, no parameters                          -> 202 Accepted
```

The `Date` read is the same freshness check the Race does, and this is what
a stale watch answers: **`N/A`**. The Race answered with today's date and
nothing followed. So the check is real and the push is conditional on it.

`Format` is an enum whose four values the structure walk spells out -
**SGEE, EPO, CEP, LLE** - and the watch answered **2**, which is `CEP` if
the enum is numbered in the order enumerated. Worth confirming before
relying on it; the value matters only for choosing what to fetch.

### A wire type this project did not have

The chunks carry parameter type code **`0x000D`**, a byte array. Until now
only `0x0006` (int32), `0x0008` (int64) and `0x000C` (NUL-terminated
string) had been seen.

Each chunk PUT body is:

```
[ack body, 6 bytes]
01            one parameter
0d 00         type 0x000D, byte array
02            tag, constant across all 136 chunks
<u24 LE>      total transfer size - 61440 in every chunk
<u24 LE>      bytes transferred including this chunk
<u16 LE>      this chunk's length
<data>
```

135 chunks of 453 bytes and a final one of 285. Verified rather than
assumed: the length field equals the actual data length in every chunk, the
cumulative field is the running sum in every chunk, and the last one's
cumulative equals the total field exactly. 135 x 453 + 285 = 61440.

### The payload

**61440 bytes = 60 KiB exactly, of which the last 1605 are zero.** So the
transfer pads to a fixed 60 KiB buffer rather than the file being that size
by coincidence. Entropy 7.39 bits/byte - packed binary, not compressed
text. Saved as `ephemeris_9baro_2026-09-25.bin` in the private fixtures.

### What is still missing, and it is only one thing

Where the phone gets those 61440 bytes. It is not in `libmds.so` - the blob
arrives from the Java side through `suunto://MDS/GNSS/%s/EphemerisData` -
and the URL is built at runtime, so it is not in the APK's string table
either. The dex has exactly three strings containing "ephemeris", all of
them resource paths.

That needs an HTTPS capture during a sync of a watch whose ephemeris is
stale. It was deliberately not attempted alongside this one: the mitmproxy
CA has to be bind-mounted into Android 14's Conscrypt APEX again after every
reboot, and a broken TLS setup would have meant no download, therefore no
push, therefore no capture of the mechanism above - which is the half that
cannot be obtained any other way.

**Implementable today**: everything except the source of the bytes. The
chunking, the framing, the byte-array type code and the begin/commit
sequence are all pinned down.

## Which watches this protocol covers, and which it does not

Three Suunto watches were available while this was written, and they fall
into three different places.

| watch | link to the phone | this project |
|---|---|---|
| Suunto Race | Whiteboard over BLE, own WiFi for cloud fetches | fully supported |
| Suunto 9 Baro | Whiteboard over BLE, no WiFi at all | supported; needs its own descriptor table, and gets ephemeris pushed |
| Suunto 7 | **Google's Wear OS Data Layer** | out of scope over BLE |

The Suunto 7 is a Wear OS watch and is not part of the Movesense/Whiteboard
family at all. The APK says so - `isWearOsNode`, `isWearOsPaired`,
`Suunto7Capability`, 57 references to `com.google.android.gms.wearable`
including `CAPABILITY_CHANGED` and `NODE_MIGRATED`, and **not one string
tying Suunto7 to MDS, Whiteboard, Movesense or Spartan**.

Confirmed on hardware rather than left as a reading of the strings: the
official Suunto app on a phone paired with a Suunto 7 **does not find the
watch at all**. Its companion channel is Google Play Services', which a
Sailfish app cannot speak and which is not this protocol.

So a BLE capture of a Suunto 7 would show Wear OS companion traffic and
nothing useful here. It was not taken.

**What does cover it**: a Suunto 7 uploads its own workouts to the Suunto
cloud over WiFi, so this project's cloud sync should already read them
without any watch-side work. Untested, and worth one look at the workout
list before claiming it.


## Does a Race have the push path too? (open, but cheap to settle)

Jarno's question: with the Race's WiFi switched off, would the app fall
back to pushing the ephemeris over BLE the way it does for a 9 Baro? If it
did, one mechanism would cover both watches and the unknown 16-character
token would stop mattering.

Two things in the 2026-09-25 Race capture argue against it, and one makes
the experiment a waste of an evening *today*:

- **The app never reads the watch's WiFi state.** It touches exactly two
  WiFi paths, `/Settings/Wifi/Cloud/OfflineMaps/Url` and
  `.../STTAuthorization`, both writes. `/Settings/Wifi/Enabled` exists as a
  resource and is not asked for. Together with
  `OBI2::**LegacyDevice**GNSS::putEphemerisData` that points at a branch on
  device class, not on WiFi being available.
- **The Race's ephemeris is current.** Its `Date` answered
  `2026-09-25T00:00:00Z` - the watch had already fetched the day's file
  itself. Switching WiFi off does not make that stale; the data is good for
  days. A capture taken now would show the same nothing the last one did.

The cheap way to settle it is to ask the watch instead of the app. A
resource a model does not have answers with the six-byte `f5` rejection
rather than timing out, so one GET to
`/Device/GNSS/ExtendedEphemerisData/Upload/0` on a Race is the whole
experiment: an ack means the push path exists there and a capture is worth
taking, an `f5` means it is legacy-only and the idea is dead.

`AppController::probePath()` exists for exactly this, and for the next
question of the same shape - of which there have been several, each
previously costing a capture.

### The probe's answer: a Race has the push resource too

```
/Settings/Wifi/Enabled                        exists (f0 a4 9f 01 80 00 c8 00)
/Device/GNSS/ExtendedEphemerisData/Upload/0   exists (01 40 07 01 80 00 c8 00)
```

Both acked with status 200 rather than the `f5` rejection, so **the push
path is not legacy-only**. The handle bytes for `Upload/0` are
`01 40 07 01 80 00` on the Race - character for character what a 9 Baro
answered for the same path, which is worth noting on its own: a handle is
per resource, not per session, at least for this one.

So the hypothesis survives its first test. What it does *not* yet show is
whether the official app ever chooses that path for a Race - the resource
existing and the app using it are different claims, and only a capture of
a sync where the Race genuinely needs new ephemeris can settle the second.

**That capture has to wait for the data to go stale.** The Race's `Date`
said `2026-09-25T00:00:00Z`, the day's own file, because the watch had
already fetched it over WiFi. Switching WiFi off does not make what it
already holds stale. The file regenerates daily at 00:00 GMT, so the
earliest the watch can be behind is the following day.

**And it matters less than it did.** If our own code can push - and the
resource is there, and the framing is known byte for byte - then the
official app's choice is a curiosity rather than a blocker. What is still
needed either way is the source of the 61440 bytes.


## The two watches report different formats, and the app fetches only one

Jarno's question was whether a Race with WiFi switched off would have the
ephemeris pushed to it instead. Reading `Format` on both watches answers it
without a capture.

```
/Device/GNSS/ExtendedEphemerisData/Format   Suunto 9 Baro -> 2
                                            Suunto Race   -> 3
```

The 9 Baro reads 2 both in the capture (before anything was pushed) and
afterwards, so the value is stable and is a property of the watch rather
than of what it currently holds - which is worth stating, because it was
briefly doubted on a mistyped byte and the doubt is what the value being
read twice, hours apart, disposes of.

The enum the watches spell out is `SGEE, EPO, CEP, LLE`. **Which name each
number is remains an assumption**: the schema walk returns the four names
each with an identical `08 00 00 00` marker and no visible index, so
"numbered in the order enumerated" - making these CEP and LLE - is a
reading, not a reading-off. What does not depend on the assumption is that
the two values differ.

And the app can only ever obtain one format. Its string table has exactly
one field of that family - `cepUrl`, alongside `cepBinary`,
`cepBinaryPath` and `cep_lastModified`. There is no `lleUrl`, no
`sgeeUrl`, no `epoUrl`; `LLE` appears as a standalone word only inside the
enum the watch itself sends. The file on disk is called `cep_binary`.

**So the app has nothing to push to a Race, whatever its WiFi is doing.**
The fallback does not exist, and the reason is not a branch on WiFi
availability - it is that only one format is ever fetched and a Race does
not ask for it. A Race with WiFi off simply goes without fresh ephemeris.

That the Race's firmware *accepts* `.../Upload/0` - it acks with 200, with
the same handle bytes the Baro gives - stays true and stays separate. The
firmware can be pushed to; the app has nothing to push it.

### The push itself is confirmed, end to end

Independently of all of the above: the 9 Baro's `Date` read **`N/A`**
before the sync and **`2026-09-25T00:00:00Z`** after it. 61440 bytes went
across in 453-byte chunks, the watch took them, and its ephemeris is
current. The mechanism is not merely decoded, it is known to work.

### What this means for implementing it here

- **A 9 Baro can be served.** Format 2, and this project holds a real
  61440-byte file byte-identical to the one the official app pushed, with
  the framing known exactly. The only missing piece is `cepUrl`, so the
  file can be refreshed rather than replayed.
- **A Race cannot**, not by pushing - it asks for a different format and
  nothing here has a source for one. Its route stays the WiFi handshake,
  blocked on the 16-character token instead.

Two watches, two blockers, and both are "where do the bytes come from"
rather than anything about the protocol.

## Item 5 closed: weather is never pushed to either watch

On the Race, `/Weather/Sync` acks and produces no payload, twice captured.
Its own event log explains that: `Weather sync ok d:1500 f:0`,
`synced city:Tampere` - the watch fetches its own forecast over WiFi.

On the 9 Baro, `/Weather/Sync` acks and the app moves straight on to the
next resource. No stream start, no PUT, nothing. And the Baro's analytics
carry no weather entries at all - no `Weather sync ok`, no city.

So a 9 Baro has no weather, and no mechanism exists to give it one over
BLE. There is nothing here left to implement: for a Race it is the same
WiFi handshake the ephemeris uses, and for a Baro the feature is simply
absent.


## The blob's source, and both watches unblocked (2026-09-26)

One sync of the 9 Baro with mitmproxy up, the day after its ephemeris was
last written, and the download appeared:

```
GET https://devices.suunto-operations.com/devices/gpsorbit/sony?appkey=<64 chars>
    200, 61440 bytes, application/octet-stream
    Last-Modified: Sat, 26 Sep 2026 00:00:19 GMT
```

**The bytes are pushed verbatim.** The same capture carries the BLE side,
and the 61440 bytes that went over in 453-byte chunks are byte-identical
to what came down over HTTPS. Nothing is transformed, unwrapped or
re-framed in between - the app downloads a file and hands it to the watch.

The file changes daily: today's differs from yesterday's in 45050 of its
61440 bytes, while the first 88 are identical and the 1605-byte zero tail
is unchanged. So the padding to 60 KiB is a property of the transfer, not
a coincidence of one file.

### All five endpoints are literals in the APK

```
devices/gpsorbit/binary?appkey=<64 chars>
devices/gpsorbit/mtk3day?index=1&appkey=<64 chars>
devices/gpsorbit/mtk3day?index=2&appkey=<64 chars>
devices/gpsorbit/sony?appkey=<64 chars>
devices/gpsorbit/sonylle?appkey=<64 chars>
```

The `appkey` is one static value shared by all five, sitting in the dex
string table. **The server does not check it.** Tested four ways against
`/gpsorbit/sony`:

| request | result |
|---|---|
| no `appkey` parameter at all | 200, 61440 bytes |
| `appkey=` (empty) | 200, 61440 bytes |
| `appkey=xxxxxxxxxxxxxxxx` | 200, 61440 bytes |
| the real key (control) | 200, 61440 bytes |

And not merely the same length - the file fetched with no key at all is
SHA-256-identical to the one fetched with it, for both `/sony` and
`/sonylle`. A plain GET with no `User-Agent` and no headers works too.

So there is no credential to carry, and the key is not reproduced here
because there is no reason to.

**This has a consequence worth stating plainly: keeping a watch's GPS
current needs no Suunto account.** Every other watch-side feature in this
project either reads the cloud or writes to it; this one touches an open
static-file endpoint and the watch. Someone who never signs in still gets
working assisted GPS.

**And that settles the enum.** `Format` offers `SGEE, EPO, CEP, LLE`, the
endpoints are `binary`, `mtk3day`, `sony`, `sonylle`, and the 9 Baro -
which reports Format **2** - is the watch whose sync fetched `/sony`. Two
lists of four, in the same order, with the third member observed rather
than assumed:

| Format | name | endpoint | bytes |
|---|---|---|---|
| 0 | SGEE | `binary` | 71603 |
| 1 | EPO | `mtk3day` (two parts) | - |
| 2 | CEP | `sony` | 61440 |
| 3 | LLE | `sonylle` | 22836 |

The sizes come from fetching each once. So the "numbered in the order
enumerated" assumption this document has carried since yesterday, hedged
every time it was used, turns out to have been right - and it is no longer
an assumption.

### What this unblocks

**Both watches, and neither needs the WiFi handshake.**

- A 9 Baro reports Format 2, so `/gpsorbit/sony`, 61440 bytes, pushed in
  453-byte chunks - the exact path already confirmed working end to end.
- A Race reports Format 3, so `/gpsorbit/sonylle`, 22836 bytes. Its
  firmware acks `.../Upload/0` with the same handle bytes the Baro gives.

So the 16-character `WatchUserKey` token, and the whole
`/Settings/Wifi/Cloud/*` route, stop being on the critical path. They are
how the *official app* keeps a WiFi watch fed; they are not the only way
to feed one.

Reading `Format` first and choosing the endpoint from it is what makes
this general rather than a pair of special cases - and it is one request,
which `readValue()` already makes.

**Still untested**: that a Race actually accepts a pushed blob. Its
firmware answers the GET, which is not the same as taking the data. That
test needs no capture and no cloud - fetch `sonylle`, push it, read `Date`
back, exactly as the Baro has now done twice.


## A Race takes a pushed ephemeris (2026-09-26)

The question a capture could not answer, answered by writing the code and
running it: **a Suunto Race accepts GPS assist data pushed over BLE by a
third-party client.**

```
Race, before:  /Device/GNSS/ExtendedEphemerisData/Date -> 2026-09-25T00:00:00Z
                                                Format -> 3
             this project pushes /gpsorbit/sonylle, 22836 bytes
Race, after:   Date -> 2026-09-26T00:00:00Z
```

So the mapping Format 3 -> `sonylle` is right, confirmed by the watch
taking the file rather than by reading a list. And the WiFi handshake -
`/Settings/Wifi/Cloud/OfflineMaps/Url`, `STTAuthorization`, and the
16-character token whose origin was never found - **is not needed at
all**. It is how the official app keeps a WiFi watch fed. It is not the
only way, and this project does not have to use it.

That removes the last blocker on the Race side, and it removes it
permanently: nothing in the working path touches an account, a session or
a credential.

### And a 9 Baro does too - it just takes its time

The same code against a 9 Baro sends all 61440 bytes with every chunk
acked, the commit accepted, and then `Date` still reads `N/A` three
seconds later. No chunk was refused - a refusal now fails loudly with its
status and byte offset, and none happened.

What differs between the two runs is the file: 22836 bytes in 51 chunks
for the Race, 61440 in 136 for the Baro. The official app pushes the
larger one through the same single `/Upload/0` and the watch takes it, so
the index is not it and the size is not inherently it.

It was the wait. Reading `Date` again a few minutes later with the path
probe gave `2026-09-26T00:00:00Z` - the watch had taken the data all
along and was still working through it when the code gave up on it.

So **both watches accept a pushed ephemeris**, and the only difference is
how long the older one needs: a Race is done in about three seconds, a 9
Baro takes minutes for nearly three times as much data on older hardware.

A fixed wait is the wrong shape for that - whatever value is picked is too
long for one watch and too short for the other. `Date` is polled instead,
every three seconds for up to three minutes, and answers as soon as the
watch does. `N/A` means "holds no ephemeris", which is also what a watch
says while it is busy replacing one, so it is a reason to keep waiting
rather than a verdict.

That the first version reported failure on a watch that was merely still
working is worth keeping in mind for the next thing that answers 202.
