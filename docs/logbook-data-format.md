# `/Logbook/byId/<id>/Data` - the real workout data pipeline, fully decoded

## What this is

The single biggest open question left in the whole project: how does a
new workout's actual sample data (GPS, HR, pace, ...) come off the watch
over BLE? This is now **fully answered and reproduced against three real
workouts** captured live from Jarno's Suunto Race. It's a four-stage
pipeline:

```
BLE notify PDUs
  -> SLIP/CRC32 Whiteboard frames        (already known - mdswirecodec.cpp)
  -> MDS bulk-transfer chunks            (type=0x01, reqid=0x0000)
  -> Heatshrink (LZSS) compressed stream
  -> "SBEM0103" TLV container            (the real per-workout data)
```

## Stage 1: bulk-transfer chunking (type=0x01, reqid=0x0000)

`/Logbook/byId/<id>/Data` doesn't return its payload directly from a GET.
Like `/Entries`, the initial GET only gets a short ack; the real payload
then streams in as a flood of unsolicited `TYPE=0x01`, `reqid=0x0000`
notifications (not tied to any request - same "reqid=0" pattern as the
session handshake). Each one, after the outer SLIP/CRC32 envelope
(`mdswirecodec.h`'s `Mds::Decoder`) is stripped, has this structure
(verified byte-for-byte against >900 real captured frames, 0 CRC
failures once SLIP unescaping was applied correctly - see "A
methodology bug" below):

```
offset  size  field
0       10    constant prefix: f0 24 0e 01 80 00 02 24 22 25
10      4     running byte-offset into the logical stream, LE32
              (0, then +chunk_size each notification - confirms these
              chunks reassemble into ONE continuous byte stream)
14      2     chunk_size, LE16 (payload length that follows - observed
              constant at 278 for a full-size chunk, shorter for the
              last chunk of a transfer)
16      6     more constant/near-constant bytes (not decoded further -
              not needed to extract the payload)
22      N     the actual payload (chunk_size bytes)
```

This exactly matches `MDS_HEADER_SIZE=28` / `MDS_CHUNK_SIZE_OFFSET=20` in
[libdivecomputer](https://github.com/libdivecomputer/libdivecomputer)'s
`suunto_nautic.c` (see "Provenance" below) - those offsets are relative
to the *raw* wire packet (`0xA5` sync + opcode + the 6-byte outer
envelope this project already strips), i.e. `packet[28]` = `body[22]`
here. Concatenating `payload` across consecutive chunks in
byte-offset order reassembles the full compressed stream - this project's
own capture reassembled 3 separate transfers of 57174, 52517 and 27719
bytes respectively (three different logbook entries fetched back-to-back
in the same capture).

**A methodology bug worth remembering**: the first pass at this analysis
used a quick Python reassembly script that concatenated raw notification
bytes and looked for a trailing `0x7E` to mark message end, *without*
implementing SLIP unescaping (unlike this project's real `Mds::Decoder`,
which does). That's fine for the mostly-ASCII schema/descriptor traffic
elsewhere in the capture, where literal `0x7E`/`0x7D` bytes are rare, but
this bulk binary stream hits them constantly - so a chunk of real data
would occasionally get mis-split mid-message, its CRC would fail, and it
looked (misleadingly) like some frames were corrupt or used a different
checksum scheme. Redoing the reassembly with a proper unescaping decoder
(the same logic as `Mds::Decoder::feed()`) made every single frame's CRC
validate. Lesson: **any analysis script touching this protocol needs the
real unescaping decoder, not a quick approximation** - the moment the
payload isn't mostly-ASCII, the shortcut silently produces wrong bytes.

## Stage 2: Heatshrink decompression

The concatenated payload stream is compressed with
[Heatshrink](https://github.com/atomicobject/heatshrink) (a small LZSS
variant designed for embedded use, ISC licensed) - confirmed by:
- High byte-entropy (~7.7-7.8 bits/byte out of 8) that isn't explained by
  bit-packed-but-uncompressed structured data alone, and isn't zlib/gzip
  (no magic bytes, `zlib.decompress` fails at every `wbits`/offset tried).
- `suunto_nautic.c` uses `heatshrink_decoder_alloc(256, 7, 5)` -
  `window_sz2=7`, `lookahead_sz2=5`. Decompressing this project's own
  captured streams with those exact parameters (via the `heatshrink2`
  PyPI package, which wraps the real C library) makes the output start
  with the literal bytes `SBEM0103` - unambiguous confirmation, not a
  coincidence.

`src/ble/heatshrink/` vendors the real upstream `heatshrink_decoder.c/.h`
(+ `heatshrink_common.h`/`heatshrink_config.h`, default dynamic-alloc
config, ISC `LICENSE` file included) rather than reimplementing the
algorithm - same reasoning as reusing `suuntool`'s documented cloud
signing scheme instead of guessing: a real, tested implementation beats a
hand-rolled one for something this fiddly. `src/ble/sbemcontainer.h/.cpp`
wraps it in a small Qt-free `Sbem::heatshrinkDecompress()`.

## Stage 3: the `SBEM0103` TLV container

After decompression, the bytes are Suunto's own SBEM binary format
(`SBEM0103` magic, then a flat sequence of type-length-value records):

```
[chunk id: 1 byte][length: 1 byte][value: length bytes]
```

...except `length == 0xFF` means "the real length is an extended 4-byte
little-endian value that follows immediately", for chunks too big for a
single byte.

`Sbem::parseContainer()` walks this and was validated against one full
real capture (the smallest of the three, 27719 compressed / 83045
decompressed bytes): **7527 chunks, parsed to completion with zero
malformed chunks, consuming every single decompressed byte exactly** -
see `tests/test_sbemcontainer.cpp` (`tests/fixtures/
logbook_data_heatshrink.bin` is the real captured golden vector,
compressed-but-MDS-header-stripped). Chunk-id histogram from that same
real workout: `0x0c`×732, `0x0f`×1492, `0x12`×1553, `0x16`×1483,
`0x17`×7, `0x18`×697, plus one-shot `0x01/0x02/0x03/0x04/0x08` chunks at
the very start (very likely a one-time header/summary section, not yet
decoded further).

## Provenance

The exact `MDS_HEADER_SIZE`/`MDS_CHUNK_SIZE_OFFSET` constants and the
Heatshrink parameters came from
[libdivecomputer](https://github.com/libdivecomputer/libdivecomputer)
PR #73 (`suunto_nautic.c`/`suunto_nautic_parser.c`, LGPL-2.1 per that
project's `COPYING` file - **not vendored into this project**, only read
for reference, same as `zappctl`'s docs were used earlier). **Correction
from an earlier draft of this doc**: this PR's title is "Add Suunto
Nautic / Ocean (Vaasa) BLE support", and while the Nautic/Nautic S are
pure dive computers, the **Suunto Ocean is explicitly a hybrid** -
Suunto's own marketing: "a dive computer and GPS sports watch in one",
with 95+ sport modes, GPS, barometer, offline maps and on-wrist HR -
i.e. built on the same sports-watch platform as the Race/Vertical this
project targets, not a separate lineage. That makes the Ocean-specific
parts of this driver (wrist HR, GPS, activity/sport id) far more likely
to carry over to the Race than the pure-diving parts (depth, gas
mixes, deco/NDL) - and that's exactly the split the empirical checks
below found.

## Chunk semantics: what carries over from Ocean/Nautic to the Race, and what doesn't

Tested every chunk-decode hypothesis from `suunto_nautic_parser.c`
against all three of this project's real captured streams (not just the
format mechanics - the actual claimed byte layouts). Every chunk except
the timeline base (`0x01`) is documented there as starting with a
signed `int16` LE millisecond delta from the previous chunk's time -
that convention held up correctly (see below), but which chunk ids
carry which fields is clearly **firmware/device-specific**, exactly as
that source's own comments warn (e.g. its `CHUNK_IMU` id is `0x23` on
one hardware variant and `0x22` on another) - so each hypothesis had to
be checked, not assumed:

**Confirmed, carries over directly:**
- **`CHUNK_ACTIVITY` (`0x08`)**: one-shot, `[timeDelta:2][sportId:1
  ][asciiTail]`. All three streams: the ASCII tail is *literally the
  decimal string of sportId itself* (`sportId=4` → tail `"4\0"`,
  `sportId=12` → tail `"12\0"`) - clean, self-consistent confirmation
  across independent real workouts. This is the field `WorkoutStore`
  needs for `activityId` on a BLE-synced workout.
- **The universal leading-delta convention**: walking the *entire*
  interleaved chunk stream in original order and accumulating every
  non-`0x01` chunk's leading `int16` delta into one shared clock
  produces a plausible total elapsed time for all three streams (78.97
  min / 61.44 min / 25.88 min) - clearly real workout durations, not
  noise. (An earlier pass summed each chunk id's deltas *separately*,
  which is not how the format works - chunks of different ids share one
  interleaved clock - and produced nonsense including negative totals
  for some ids; fixed before trusting these numbers.)
- **`CHUNK_PROFILE_1HZ` (`0x12`)**: real "1Hz-ish" pacing role confirmed
  (thousands of occurrences, ~500-900ms apart) - this is the timeline's
  main heartbeat chunk on the Race too, and it turns out to be a literal
  heartbeat: **byte 2 (the single payload byte after the leading 2-byte
  delta) is heart rate, `uint8` bpm.** Confirmed against Jarno's real
  app-reported avg/max HR for all three workouts by scanning every
  `(chunk id, byte offset)` pair for one whose byte-max across the whole
  stream matched the real max: `0x12` offset 2 hit an **exact** max-bpm
  match on all three streams (97/133/99), with the byte-average also
  landing close to the real reported average in each case. So on the
  Race, HR lives at `0x12` (not `0x0f`, Ocean/Nautic's id for it) - a
  clean, concrete example of the "dynamic schema" issue #70 described:
  the container/framing carries over, the *id assignment* doesn't.

**Refuted / doesn't carry over as documented:**
- **`CHUNK_HEARTRATE` (`0x0f`)**: on the Race, byte 2 (the claimed `hr:
  uint8 bpm`) takes wildly implausible values (0, 3, 4, 9, 255, ...) -
  this id does **not** mean heart rate here (real HR is at `0x12`, see
  above). Also structurally different: Ocean's HR chunk is 3 bytes, the
  Race's `0x0f` chunk is always 6 bytes.
- **`CHUNK_SURFACE_PRESSURE` (`0x17`)**: tried the documented `float32`
  at offset 2 (barometric pressure, Pa) - a very plausible candidate
  since the Race does have a barometer - but it decoded to a flat `0.0`
  on every sample across all three streams, so this offset/field
  doesn't hold either, at least not as a raw Pa float.

**Not yet tested**: `0x16` (17 bytes, vs. Ocean's 141/195-byte
`CHUNK_EXTENDED_STATUS` - clearly a different, much smaller record on
Race), `0x18` (5 bytes), and `0x1f` (7 bytes, only seen in one of the
three streams so far, same frequency as `0x12` in that stream - possibly
a paired/companion record). The one-shot `0x01`/`0x02`/`0x03`/`0x04`
header chunks at the very start of the container are also still
undecoded; `0x01` (`CHUNK_TIMELINE_BASE`, 8 bytes) has a suspicious
*constant* 3-byte tail (`01 00 0c`) across all three streams with only
the preceding byte varying, hinting at a version/type marker rather
than workout-specific data, but this isn't confirmed either. `0x0c` (20
bytes, very frequent) is partially decoded - see "Ground-truth
calibration" below.

## What [libdivecomputer issue #70](https://github.com/libdivecomputer/libdivecomputer/issues/70) adds

This is the original reverse-engineering report PR #73 was built from (by
`urbamax`, MIT-style "here's my research" writeup, not code - fetched
verbatim via the GitHub REST API, not through an AI summary, after the
MDS-header-offset mistake below). Two things materially change this
project's plan:

**1. Confirms our own framing/activity findings independently.** The
report's own worked example CRC/header explanation matches this
project's derivation exactly (their `0x01 0x2D` = `0x012D` = 301 decimal
is this project's own observed 301-byte notification body length,
derived completely independently). Their `CHUNK_ACTIVITY` (`0x08`)
description - `Offset 2: Activity Type, Offset 3-5: CustomModeId as an
ASCII string (e.g. "51\0")` - is the exact same "ASCII tail mirrors the
numeric id as a decimal string" behavior this project found empirically
on all three real Race captures. Two independent devices, two
independent reverse-engineering efforts, same behavior - this is now
about as confirmed as it can be without Suunto's own source.

**2. The big one - chunk ids are a *dynamic*, not fixed, schema.** A
follow-up comment (`urbamax`, from ARM64-disassembling the official
app's `libmds.so`) states plainly: *"You won't find a static Chunk ID
... hardcoded for the dive summary. The SBEM0103 format uses a Dynamic
Schema. The app uses `SDS::LogbookDecoder::setDescriptors` to map a
dictionary of names to dynamically generated Chunk IDs for that specific
dive or device model."* This is the real explanation for why some
Ocean/Nautic hardcoded offsets happened to carry over to the Race
(`CHUNK_ACTIVITY`) while others clearly didn't (`CHUNK_HEARTRATE`,
`CHUNK_SURFACE_PRESSURE`) - they're not universal constants, they're
assigned per device/session, and matching by accident is exactly as
likely as it sounds.

This points straight back at this project's own `docs/
sml-schema-descriptors.md` - the 246-entry `<PTH>`/`<FRM>` catalog this
project already pulled from the Race over BLE separately is almost
certainly *the same descriptor mechanism* (`setDescriptors`) the app
comment describes. **Correction to that doc's own speculation**,
checked properly this time (re-extracted all 31 `<GRP>` entries from the
original capture with correct SLIP unescaping, not just the couple
spot-checked earlier): the `<GRP>` lists are comma-separated integers up
to **361**, not in the 1-31 range this project's actual SBEM chunk ids
occupy - so `<GRP>` numbers are *not* directly the SBEM TLV chunk ids.
They're a separate, larger index space (most plausibly indices into the
246-entry `<PTH>` catalog itself, grouping related fields together) -
related to the dynamic-schema mechanism, but not a direct lookup table
for it. The actual descriptor-to-chunk-id assignment (or how to trigger
and read it, if it's exposed over BLE at all rather than being
compile-time-fixed per firmware) is still unfound.

**3. A robustness tip worth keeping for a live parser.** A separate
comment warns Heatshrink output can contain small localized corruption
artifacts (e.g. runs of `1E 1E 1E 1E`) that a naive linear TLV walk will
misinterpret as a bogus chunk id+length and use to desync the rest of
the stream - their fix is to validate known chunk ids against an
*expected* fixed length and fall back to byte-scanning resync on a
mismatch. This project's own `Sbem::parseContainer()` doesn't do this
yet - not urgent (it walked one full real capture with zero malformed
chunks), but worth adding once real chunk lengths are pinned down, since
a live BLE download is more likely to hit transmission hiccups than a
clean historical capture replay.

Also noted but **not applicable here**: that comment additionally
describes ~84 bytes of supposedly-uncompressed plaintext at the start of
"the file" before the real Heatshrink stream begins, from their own
debugging process. This doesn't match this project's own pipeline, which
is already validated end-to-end (decompression starts at byte 0 of the
MDS-header-stripped stream and immediately produces the literal
`SBEM0103` magic, byte-exact across three real captures) - very likely
an artifact of whatever raw/less-processed capture format they were
working from at that point in their own investigation, not a correction
to what's already confirmed working here.

## Still open (where Phase 6/`LogbookSync` picks up next)

1. **Find the actual SBEM chunk-id assignment mechanism** - now
   understood to be a "dynamic schema" rather than fixed constants (see
   above), which reframes the whole remaining-chunk question: the next
   useful step probably isn't more guess-and-check against Ocean/Nautic
   offsets, it's figuring out how/where the Race exposes its *own*
   chunk-id assignment (if over BLE at all) or accepting per-firmware
   calibration against known real values as the practical path.
2. **Decode `0x16`/`0x18`/`0x1f`'s internal value structure** - `0x0c`
   is now solved (UTC timestamp + GPS fix, see "GPS found" below, which
   also gives distance and speed for free); altitude and cadence/steps
   are the remaining unknowns, most likely in one of these three.
3. Add resync-on-malformed-chunk robustness to `Sbem::parseContainer()`
   (see point 3 under issue #70 above) before relying on it against a
   live BLE download rather than a replayed historical capture.
4. Confirm this same pipeline holds for a workout **as it's actively
   streamed live** (this capture was of the official Android app doing a
   historical sync, presumably after the workout already ended) -
   probably fine, no reason to expect otherwise, but not yet exercised.
5. Resolve the countdown-counter field noted in "Ground-truth
   calibration" below (the offset-13 puzzle itself is resolved - see
   "GPS found" further down).

## Ground-truth calibration (ID = actual UTC timestamp; `0x0c` decoded)

Jarno supplied the real recorded stats for all three captured workouts
from the app itself, keyed by their logbook entry id - which the
libdivecomputer issue #70 report already claimed *is itself* the
workout's Unix start timestamp in seconds. Confirmed exactly right: all
three logbook ids convert cleanly to the real workout start times.

| id | start (Helsinki) | sport | duration | distance | avg/max speed | energy | ascent/descent |
|---|---|---|---|---|---|---|---|
| 1785740504 (stream0) | 2026-08-03 10:01 | Cycling | 38:19 | 5.42 km | 8.5 / 25.7 km/h | 156 kcal | 27.8 / 19.5 m |
| 1785760357 (stream1) | 2026-08-03 15:32 | Cycling | 37:17 | 5.56 km | 8.9 / 26.5 km/h | 210 kcal | 58.6 / 52.1 m |
| 1788194033 (stream2) | 2026-08-31 19:33 | Walking | 24:53 | 2.09 km | 5.0 / 5.4 km/h | 132 kcal | 15.4 / 18.7 m, cadence 55/72 rpm, 2672 steps |

With real numbers to search for, brute-force-scanned every byte offset
of the decompressed stream for a `float32`/`uint32` matching each known
value, then cross-referenced hits back to their containing chunk. This
cracked open `0x0c` (the 20-byte, very frequent chunk flagged "not yet
tested" above):

**Confirmed**: `0x0c`'s bytes `2..8` (6 bytes, not the full 8 a `uint64`
read would suggest - see below) are an **absolute Unix timestamp in
milliseconds**. Every sample across all three streams decodes to a time
within seconds of the real workout's start, climbing steadily through
the recording - e.g. stream1's first `0x0c` sample reads `1785760359000`
against a real start of `1785760357000` (2s in), and later samples climb
from there. This is a much better anchor than the per-chunk leading
`int16` delta this doc previously assumed applied uniformly (`0x0c`'s
own leading 2 bytes are usually `0000` - it doesn't consistently carry a
meaningful delta the way `0x12` does).

**Confirmed**: bytes `8..12` (`float32`) hold a live-updating stat.
Caught an *exact* hit - `210.0` kcal, bit-for-bit, twice, in
stream1's `energy_kcal`. Given it's a running total, later samples
naturally read closer to the true final value than earlier ones (this
is why the very first broad scan needed generous tolerances rather than
exact matches to catch it happening mid-workout, not just at the end).

**Update - both of the next two items turned out to be resolved once
real per-second ground truth was available (see "GPS found" further
below); kept here for the record rather than deleted, since the dead
end itself is a useful lesson:**
- The offset-13 `float32` "matching" various speed/ascent/descent
  targets at different sample indices, mentioned as two live hypotheses
  here originally, turned out to be neither - it was simply *misaligned*
  reads straddling two real, adjacent fields (latitude at bytes `10-14`,
  longitude at `14-18`, both confirmed below). A byte offset one or two
  positions off from a real field's boundary will produce numbers that
  *look* like noisy, semi-plausible values without being anything real -
  worth remembering as a general trap, not just specific to this field.
- The "linearly decreasing 16-bit value" at bytes `12..14` is the same
  artifact: those two bytes are the high half of latitude (bytes `12-13`)
  overlapping the low byte of a still-unidentified field at byte 14's
  neighbourhood. A cyclist moving in a roughly consistent direction
  changes their raw latitude integer roughly monotonically over a short
  stretch, which is exactly the "smooth countdown" pattern observed -
  not a counter, just latitude drifting.
- The energy_kcal exact-`210.0` hit at offset 8 (two instances, out of
  1520 samples) is now understood too, and was very likely pure chance:
  `210.0`'s IEEE-754 bytes are `00 00 52 43` - bytes 8-9 being `00 00`
  (a field that legitimately reads zero most of the time, see below) and
  latitude's own low two bytes (bytes 10-11) happening to equal `52 43`
  at exactly those two samples is plausible, not evidence of a real
  "energy" field at that position. A useful reminder that an *exact*
  match is strong evidence but not proof by itself when the search space
  is wide enough and enough samples are checked.
- The three streams' *raw recorded time span* (last sample's timestamp
  minus first) is roughly **2x** the real reported duration for both
  cycling workouts, but close to 1:1 for the walking one. Working
  theory, not confirmed: **auto-pause** - the watch likely keeps logging
  wall-clock time through stationary/paused segments (common at traffic
  lights while cycling), while the app's reported "Kesto" only counts
  active time. Walking rarely triggers auto-pause the same way, which
  would explain why only that stream's total lines up closely.
- `distance_m` is now resolved - see "GPS found" below, it's derived
  from GPS rather than stored. `steps` and `cadence` (workout 3's
  walking-specific fields) are still open - worth specifically checking
  `0x1f` and `0x18` (both walking-heavy chunk ids) against them now that
  real per-second target numbers exist.

## Cross-referencing Jarno's own FIT exports (per-second ground truth)

Jarno exported all three workouts as `.fit` files from the app (Garmin's
open, well-documented binary format - parsed here with the `fitparse`
PyPI package, not hand-rolled, same "use a real implementation" approach
as Heatshrink). **Not committed to this repo or kept anywhere outside
this session's scratchpad** - they contain real GPS coordinates and
other personal data, unlike everything else in this doc (which is wire
protocol structure, not workout content). If per-second ground truth is
needed again later, re-export and re-run the analysis scripts described
here rather than expecting the `.fit` files themselves to be saved
anywhere in the project.

Each file's `record` messages are a per-second time series
(`timestamp`, `heart_rate`, `distance`, `speed`, `altitude`, GPS
`position_lat`/`position_long`, `cadence`, `temperature`,
`vertical_speed`) - exactly the kind of ground truth needed to move
past aggregate-stat matching to matching *specific moments*.

**Confirms the auto-pause theory from the previous section, decisively.**
FIT's own `session.total_elapsed_time` (4736.15s / 3683.66s / 1492.73s)
matches this project's earlier "sum every chunk's leading delta across
the whole interleaved stream" derivation (78.97 / 61.44 / 25.88 min)
almost exactly - confirming that derivation was measuring real elapsed
wall-clock time correctly all along, not an artifact. Meanwhile FIT's
`record` message *count* (2301 / 2238 / 1493) lines up almost exactly
with the real reported **active** duration in seconds (2299 / 2237 /
1493) - i.e. FIT's own per-second stream already excludes auto-paused
time, which is exactly the mechanism that explains the earlier 2x gap
for the two cycling workouts (frequent stops) and its near-absence for
the walking one (few stops).

**Confirms `0x0c`'s UTC timestamp field completely.** Joining each
`0x0c` sample's decoded UTC-ms (rounded to the second) against the FIT
per-second series matched **100% of samples** across all three streams
(1820/1820, 1520/1520, 732/732) - the timestamp field is fully solved,
not just plausible.

**A methodology trap worth recording**: an initial raw-value correlation
pass found offset 3/4 "matching" `distance` with `r` near ±1.0 - this
was spurious. Distance (and the UTC timestamp) both increase
monotonically through a workout, so *any* two monotonically-trending
series correlate strongly regardless of whether they're related -
including, it turned out, reading raw bytes from *inside* the UTC
timestamp field itself as if they were an unrelated float. Re-ran using
first-differences (`value[i] - value[i-1]`) instead of raw values, which
cancels out any shared trend and only responds to genuinely correlated
*changes* - the offset 3/4 "hit" disappeared under this stricter test,
confirming it was the trend artifact, not a real field. Any future
correlation-based field hunting on this data should default to
differenced series, not raw ones, to avoid this trap.

## GPS found - and it resolves distance and speed too

The previously-unresolved offset-13 field turned out to be a mis-read
straddling two *other*, real fields right next to it. Brute-force
scanning every `(chunk id, byte offset)` in `0x0c` for an `int32` inside
the plausible geographic bounding box of each real GPS track (using
Jarno's real FIT coordinates converted from semicircles) found:

- **Bytes `10..14`**: latitude, `int32` LE, degrees × 10⁷.
- **Bytes `14..18`**: longitude, `int32` LE, degrees × 10⁷.

Both hit **100% of samples** in all three streams (1820/1820, 1520/1520,
732/732), and - unlike every other field so far - the match isn't just
"close", it's **essentially exact**: mean distance between the decoded
coordinate and the real FIT coordinate at the same timestamp was
**0.00-0.01 m** across all three streams. (The old "offset 13"
correlation hits from the previous section were reading 1 byte into
latitude's low end plus 3 bytes into longitude's high end - a
meaningless straddle that happened to wobble in a vaguely plausible
range, now fully explained away.) So `0x0c` is really a combined
**timestamp + GPS fix** record - on Ocean/Nautic this was its own
separate chunk (`CHUNK_GPS`, `0x0B`, per issue #70), but the Race folds
it into the same 20-byte record as the clock. This also explains why
`0x0B` never appeared anywhere in this project's captures: on the Race,
GPS isn't a separate chunk id at all.

**This unlocks distance and speed for free - they don't need their own
field, because they're derived, not stored.** Accumulating great-circle
(haversine) distance between consecutive decoded GPS fixes matched
Jarno's real reported total distance within 0.04-0.4% on all three
workouts (5437.1 m vs. 5417.0 m real; 5561.7 m vs. 5557.0 m; 2094.8 m
vs. 2094.0 m - the small residual is expected haversine-vs-the-app's-own-
smoothing noise, not a sign anything's wrong). Point-to-point instantaneous
speed (distance between consecutive fixes ÷ time between them) correlated
strongly with real per-second speed for both cycling workouts (`r=0.94`
and `r=0.97`) - walking showed no reliable correlation (`r=-0.04`), which
makes sense rather than being a failure: at walking pace (~1.3 m/s) a
single GPS fix's few-metre noise floor is a much larger fraction of the
actual per-second movement than it is at cycling speed, so raw
point-to-point differencing is too noisy there without the smoothing a
real implementation would apply (e.g. averaging over a few seconds).

Altitude didn't come from `0x0c` (its two still-unaccounted-for byte
pairs, offsets `8-9` and `18-19`, were ruled out as `float32`, raw
`int16`/`uint16` metres, or decimetres, by direct comparison against
real per-second altitude - absence of a match, not just absence of a
coincidental one).

## Cadence found in `0x16` - and a proper cross-chunk timeline to go with it

Picking this up again to hunt for altitude required first solving a
prerequisite: `0x16`/`0x17`/`0x18` (unlike `0x0c`) carry no absolute UTC
timestamp of their own, only the universal leading `int16` delta. Built
a proper single-pass timeline reconstruction instead of the cruder
whole-stream summation from earlier: walk every chunk in original
stream order, and whenever a `0x0c` chunk appears, treat *its* absolute
UTC as authoritative and reset the running clock to it; for every other
non-`0x01` chunk, advance the running clock by that chunk's own leading
delta since the last authoritative reset. This lets any chunk type be
joined against the FIT per-second series, not just `0x0c`.

With that in place, a differenced-correlation sweep (see the methodology
trap noted earlier - differenced, not raw, to dodge shared-trend
artifacts) across `0x16`, `0x17`, `0x18` against every real per-second
field turned up one unambiguous hit on the walking stream: **`0x16`
byte 10 correlates with Δcadence at `r=+1.000`.** Checked directly
against absolute values, not just the correlation: it's an exact,
byte-for-byte match - `uint8`, raw rpm, every single one of the first 30
(and by extension all 1483) samples checked. Cycling showed no signal
there at all, consistent with Jarno's cycling sessions having no cadence
sensor paired (FIT's own `cadence` field was empty for every cycling
record). Same offset also showed up as `off=8/9` in some rescans due to
this byte's neighbours often being `0x00` - `10` is the confirmed exact
position.

The same sweep found `0x16` offset 1 correlating with Δdistance at
`r=+0.976` on the walking stream - not yet chased further (distance is
already solved via GPS so this isn't urgent), but worth a note for
whoever looks at `0x16` next; it may be a redundant/derived speed-ish
field the watch itself computes, or something else entirely.

## Altitude: extensively tested, still not found

Several encodings were tried against `0x16`/`0x17`/`0x18` with real
per-second altitude as ground truth, and none produced anything close
to the near-zero error seen for GPS, energy, or cadence:

- Raw `int16`/`uint16` in metres, decimetres, and centimetres, at every
  byte offset in all three chunks: best correlations topped out around
  `r=0.1-0.64` with mean absolute errors of several metres to several
  hundred metres depending on scale - no clear winner, and the
  strongest-*looking* candidate by raw error alone (`0x18` offset 3,
  centimetre scale) turned out to be a **near-constant** value that
  barely moved while real altitude changed by over a metre - a
  reminder that low mean-error alone isn't enough evidence; the
  candidate also has to *vary* the right amount (this project's `x_std`
  vs. `real_std` check in the search script exists specifically to catch
  this).
- Raw atmospheric pressure (`uint32`/`int32`/`float32`, plausible Pa
  range 50000-200000) converted through the standard barometric formula
  (`44330 * (1 - (P/101325)^(1/5.255))`): no candidate came within
  thousands of metres of the real value - not a near-miss, a clear
  rejection of this encoding in the offsets tried.
- Altitude expressed *relative to the workout's own minimum altitude*
  (motivated by `Header.Altitude.Min` existing as a named field in the
  watch's own SML schema catalog, suggesting Suunto favours
  delta-from-reference encoding elsewhere) - closest so far, but still
  only within 7-20 m mean error depending on offset/scale, nowhere near
  the exactness seen for other confirmed fields.

**Round two, after cadence/steps: extended the search to every remaining
chunk type, still nothing.** Built the proper cross-chunk timeline (see
the cadence section above) and re-ran the same rigorous methodology
against every chunk not yet ruled out:

- **`0x0f`** (6 bytes, fires almost as often as `0x12`/HR - a natural
  altitude-at-~1Hz candidate) - fully decoded for the first time this
  round. No altitude correlation anywhere above the noise floor on any
  stream; the strongest signals found were weak-to-moderate temperature
  correlations (`r≈0.49-0.57` on the two cycling streams, offset-
  dependent) - plausible as a real but different field (on-wrist or
  ambient temperature), not investigated further since altitude was the
  goal.
- **`0x15`** (35 bytes, rare - 2-4 occurrences per stream, evidently some
  kind of periodic/lap-style summary record) - looked promising at
  first: two of stream0's four instances gave `19.75` at offset 8,
  suspiciously close to that workout's real descent (`19.5`). Fully
  refuted on cross-checking the other two streams: the same offset gave
  `15.0/5.9/8.0` for stream1 (real ascent/descent `58.6`/`52.1` - not
  close at all) and `23424.0/0.04` for stream2 (real `15.4`/`18.7` -
  wildly off-scale). The stream0 near-match was coincidence, the same
  trap noted for the `0x15`-offset-8-as-"energy" red herring earlier in
  this document - a value landing in a plausible numeric neighbourhood
  once or twice, out of many blind offset/chunk combinations tried, is
  expected by chance and isn't evidence on its own.
- **The one-shot header chunks** (`0x01`-`0x04`, plus `0x08` and `0x15`
  already covered) - dumped and brute-force-scanned every byte for
  ascent/descent totals directly (the same technique that found GPS/
  energy): no hits beyond the already-refuted `0x15` one above.
- **Vertical speed** (`vertical_speed` from FIT, m/s - motivated by the
  established pattern that this protocol seems to store *rates*
  cadence, HR rather than *integrated totals* distance, steps, so a
  climb-rate field seemed at least as likely as an absolute-altitude
  one) - tested directly (not differenced, since it's already a rate)
  against every offset in `0x0c`/`0x16`/`0x17`/`0x18`/`0x0f`: no
  meaningful correlation anywhere.

**Open hypothesis, not confirmed, now the leading explanation given how
exhaustive this search has been**: the app's displayed/exported altitude
may not be the watch's raw barometer reading at all. Many fitness
platforms apply "elevation correction" - replacing or blending noisy
on-device barometric altitude with a digital-elevation-model lookup
keyed on GPS position, server-side or in the app, specifically *because*
raw barometric altitude drifts with weather and temperature over a
session. If that's what's happening here, there may be no byte in the
raw `/Data` stream that matches the FIT-exported altitude at all - it
would need to be compared against a barometer-plausible but not
DEM-corrected reference to confirm, which isn't available from this
data alone. Two concrete ways to make progress if this is picked up
again: (1) a workout with a much larger, sharper altitude swing (a real
hill climb rather than ~5-30 m of gentle terrain) would make a genuine
raw-barometer field's signal much easier to distinguish from noise; (2)
`/Logbook/byId/<id>/Summary` (confirmed to exist as a separate BLE
resource, not yet captured/analysed at all in this project) is exactly
the kind of endpoint that might carry workout-level ascent/descent
totals the way `/Data` carries the sample stream - worth a dedicated
capture rather than continuing to mine `/Data` for something that may
genuinely not be there.

## Steps: no separate counter found - very likely derived from cadence, like distance/speed

Went looking for step count the same way GPS/energy/cadence were found:
a monotonically-increasing counter that should land near the real total
(2672 steps, or 1336 "strides" per FIT's `session.total_strides` -
strides being gait cycles, one per two steps, which is itself a clean
2672÷1336=2.0 cross-check that these are the same real quantity in two
conventions).

- **`0x1f` ruled out first** - it looked like the natural candidate
  (walking-only, same sample count as `0x12`), but its actual structure
  is `[delta:2][tag:1, constant `0x02`][value:4, constant `float32`
  `50.0`]` - a fixed marker/threshold of some kind fired periodically,
  not a per-step or per-stride event. No variation at all in the tag or
  value across all 1553 samples in the walking stream rules it out
  completely.
- **No monotonic counter found in `0x16` or `0x18` either** - scanned
  every remaining byte offset in both (beyond `0x16`'s now-confirmed
  cadence at offset 10) for a `u16`/`i16`/`u32` trajectory that starts
  low and climbs toward ~2672 (or ~1336): nothing did. `0x16`'s bytes
  11-15 are hard zero for every sample checked; the other unexplained
  bytes in both chunks show noisy, non-monotonic patterns consistent
  with delta/status fields, not a running total.
- **Integrating cadence over time gets close, supporting a "derived, not
  stored" conclusion**: summing `cadence × 2 ÷ 60 × dt` (the ×2 because
  cadence is steps-per-foot, so ×2 for total steps) across consecutive
  `0x16` samples, skipping gaps over 3s the same way the auto-pause
  handling elsewhere in this doc does, gives **2731 steps** against a
  real **2672** (2.2% over) - and without the ×2 factor, **1365
  strides** against a real **1336** (also 2.2% over, the same
  proportional gap, which is exactly what you'd expect if this is one
  systematic rounding/pause-boundary effect rather than two unrelated
  near-misses). That's a real, structural match, just not the
  bit-exact kind GPS/cadence/energy gave - most plausibly because this
  project's simple ">3s gap = paused" heuristic doesn't line up exactly
  with whatever boundary the watch/app itself uses, not because the
  underlying idea is wrong.

Taken together with distance/speed in the GPS section above, a pattern
is emerging: **the raw BLE stream seems to carry primitive sensor
readings (GPS fixes, cadence rate, heart rate) and leaves time-
integrated quantities (distance, speed, steps) for the app/cloud to
compute** - consistent with what a resource-constrained watch would
actually want to transmit. If that holds, a client implementation
(this project's own `LogbookSync`, eventually) should plan to compute
distance/speed/steps the same way rather than expecting to find them
as raw fields.

## Practical upshot

GPS route, distance (derived), speed (derived), heart rate, activity
type, cadence, and now a strong approximate reconstruction of steps
(also derived) are all confirmed and usable for a real BLE-synced
workout view. Altitude/ascent/descent remains the one open item, for
the DEM-correction reasons discussed above. The `.fit` cross-referencing
methodology (per-second ground truth, brute-force exact-value search for
distinctive fields, differenced correlation with an explicit variance
check for noisier ones, and - new this round - checking whether a
"missing" field is actually a derived quantity rather than a stored one)
is now a proven template, with six real fields confirmed directly (UTC
time, GPS lat/lon, heart rate, activity/sport id, energy, cadence) and
two more (distance, steps) understood as derived rather than missing.

## `Logbook::decode()`: SBEM chunks to workout summary fields

`src/ble/logbookdecoder.h/.cpp` (Qt-free, `tests/test_logbookdecoder.cpp`)
turns a decoded `/Data` payload into the fields confirmed above - GPS-
derived distance/speed, heart rate, activity id, cadence-derived step
count - using a single pass over the chunk stream that also reconstructs
absolute time for every chunk (not just `0x0c`): whenever a `0x0c` chunk
appears its own UTC timestamp resets a running clock; every other
non-`0x01` chunk advances that same clock by its own leading delta. No
`totalAscent`/`totalDescent`/`energyConsumption` fields - deliberately
left out rather than populated with a guess, per the "still not found"
conclusion above. Validated against the same real captured golden vector
`sbemcontainer`'s test uses, both against an independent Python
re-implementation of the identical algorithm (exact match) and against
Jarno's real reported stats for that workout (within the tolerances
already documented above - exact for heart rate, ~2-5% for the derived
fields).

## The BLE trigger sequence: more than a two-message handshake

Implementing `Logbook::decode()`'s BLE-facing counterpart required
finally pinning down how the real app actually starts the bulk-transfer
stream - `suunto_nautic.c`'s documented model (GET's ack carries a
"Watch Magic" session id at body offset 5; send it back +1 then +2 as
two `FETCH1`/`FETCH2` triggers) turned out not to match this project's
own capture at all.

**What the real capture (frames 7826-7880) actually shows**: `GET
/Logbook/byId/<id>/Data` → a `TYPE=0x02` ack → then a long chain of
`TYPE=0x0b`/`0x0d`/`0x03`/`0x05` exchanges - the same general handle-
based resource-walking mechanism `/Entries` and the `Descriptors` schema
fetch already use, just applied to `/Data`'s own structure - eight
request/response round trips deep, where each response can reveal one or
two further child handles to visit (confirmed by tracing exactly which
byte offset of which *earlier* response each subsequent request's handle
bytes come from - not always the immediately preceding one, e.g. one
request's handle came from a response two exchanges earlier, and a
single response yielded two handles visited one after the other) -
before a final `TYPE=0x10` message triggers the actual `TYPE=0x01`
stream. This is a genuine binary object-graph walk, not a fixed two-step
sequence, and reconstructing its general grammar from one example capture
carries real risk of being subtly wrong for a different logbook entry.

**A much simpler hypothesis, confirmed byte-exact against the same
capture**: the final `TYPE=0x10` trigger's body is *mechanically* just
the initial GET's ack body's first 6 bytes with the ack's own trailing 2
bytes dropped and a single `0x00` appended - no handle-walk needed to
construct it. Verified by rebuilding the exact captured trigger frame
(frame 7868, requestId `0x0535`) from the exact captured ack (frame
7828) with this rule and getting a **byte-for-byte match, CRC32
included** - see `tests/test_mdswirecodec.cpp`'s
`testEncodeStreamStartTrigger()`.

**What's genuinely unconfirmed**: whether the elaborate middle walk is
required to "warm up" the resource before the watch will honour this
trigger, or whether it's just the official app fetching UI-only metadata
(a resource size, a display name literally named `"LogDataNotification"`,
the ASCII string `"bytes"` as a unit label - all seen in that walk's own
responses) that has nothing to do with making the data fetch itself
work. `Mds::encodeStreamStartTrigger()` and
`MdsWhiteboardClient::fetchLogbookData()` implement the **simplified**
hypothesis - skip straight from the ack to the trigger - specifically
*because* it's cheap to falsify: if the watch actually needs the walk
first, this will simply time out (the same safe, informative failure
mode every other wrong guess in this project has hit), telling us
definitively that the walk is required without having risked anything
by trying the shortcut first. `AppController::testLogbookFetch()` (new
"Test fetch" field on `PairingPage.qml`, taking a logbook id) is the
validation probe for this - real hardware, not this document, has the
final answer.

**If the shortcut doesn't work**: the walk's general grammar isn't
understood well enough yet to implement generically. The concrete next
step would be a second real capture of the *same* sequence against a
*different* logbook entry, to see which parts of the walk vary with the
entry (the handle values, presumably) versus which are fixed (the walk's
shape/offsets) - a single capture can't distinguish those, which is
exactly why the general case wasn't attempted this round.

## The shortcut works - first real end-to-end confirmation, and two real bugs it found

**The simplified trigger works.** Jarno ran `testLogbookFetch()` against
a real logbook id on his Suunto Race (2026-09-21) and got a real
`/Logbook/byId/<id>/Data` payload back with no handle-walk at all - just
GET → ack → the simplified trigger → the bulk stream. This is the first
live confirmation of the *entire* pipeline this document describes, not
just the historical-capture replay everything above was validated
against: `OK - 57174 bytes compressed, activity=4, duration=2140s,
distance=5394m maxSpeed=9,6m/s avgHR=76 maxHR=97 steps=19448`. (57174
bytes is, reassuringly, the exact compressed size of one of the three
original historical-capture streams this whole investigation was built
on - this was the same cycling workout, now fetched live instead of
replayed.)

Most of that matched the real app-reported stats for this workout
closely (duration 2140s vs. real 2299s/38:19; distance 5394m vs. real
5420m; avg/max HR 76/97 vs. real 77/97) - but two fields were clearly
wrong, and both turned out to be real, fixable bugs rather than
limitations of the approach:

- **`maxSpeed` = 9.6 m/s (34.6 km/h) against a real max of 25.7 km/h.**
  Traced to a single ~3-second gap between GPS fixes covering an
  implausible distance - a position glitch, not 34.6 km/h of real
  cycling - dominating the naive point-to-point max-speed calculation.
  Every *other* candidate speed in this workout had a ~1-second gap
  (GPS fixes land close to 1s apart throughout every capture this
  project has seen); excluding pairs more than 1.5s apart from the
  max-speed calculation specifically (distance/duration keep the wider,
  already-validated 3s window - they sum many gaps rather than taking a
  single max, so they're far less sensitive to one outlier) drops the
  glitch and lands on 7.127 m/s = 25.7 km/h, matching the real value
  almost exactly.
- **`steps` = 19448, nonsensical for a cycling workout.** Traced to
  chunk `0x16` byte 10 (cadence) reading the literal value `255` for
  *every single sample* in this workout (confirmed by direct
  inspection) - Jarno's Race had no cadence/foot-pod sensor paired for
  this ride, and `255` is the watch's sentinel for "no reading", the
  same role `0`'s already-handled sentinel plays for heart rate. The
  step-integration code was treating that sentinel as a literal (very
  high) cadence and integrating it into a physically-impossible step
  count. Fixed by excluding `255` samples from the integration, the
  same way `0` heart-rate samples are already excluded.

Both fixes are in `src/ble/logbookdecoder.cpp` (`kMaxSpeedGapMaxMs`,
`kNoCadenceSentinel`) and covered by a second golden-vector fixture in
`tests/test_logbookdecoder.cpp` built from this exact real device
result (`tests/fixtures/logbook_data_heatshrink_cycling.bin`) - so this
specific regression can't silently come back. Confirmed both fixes are
safe against the original walking fixture too: it has no cadence `255`
samples and no GPS gaps over 1.5s in the relevant window, so neither
fix changes that fixture's already-passing expected values at all.

**What this leaves open**: whether the skipped handle-walk is ever
*required* for some other logbook entry or watch state that happens to
differ from the one tested here is still unconfirmed - one successful
fetch is strong evidence, not proof for every case.

## Wired into `WorkoutStore` and the UI

`AppController::testLogbookFetch()` now saves a successful fetch, not
just reports it: `workoutFromDecoded()` (`appcontroller.cpp`) maps
`Logbook::DecodedWorkout` onto the project's own `Workout` struct - key
`"ble_<logbookId>"` (its own namespace, deliberately not merged with a
cloud-synced record of the same real workout - no cross-reference
solid enough to upsert-collide on automatically beyond timestamp
proximity), source `"ble"`, `totalAscent`/`totalDescent`/
`energyConsumption` left at `0` (absent) since none of those were found
in this resource - and upserts it, then refreshes `workoutModel` so it
shows up on `MainPage` immediately. Still `Q_INVOKABLE testLogbookFetch`
rather than a dedicated "sync" action, since manual logbook-id entry
(no on-device `/Entries` listing UI yet - that resource needs the same
handle-walk-or-shortcut question this whole document has been chasing
for `/Data`, not yet attempted for it) makes this a developer probe more
than an end-user feature for now, even though it persists for real.

**A real display bug this caught**: `MainPage.qml`'s `activityName()`
table (`1=Running, 2=Cycling, 11=Hiking, 22=Trail running`) is the
*cloud* API's activity-id vocabulary (from `suuntool`'s example output) -
a BLE-decoded workout's `activityId` comes from the watch's own SML
`ActivityType` instead, a **different numbering**: confirmed `4` for
cycling and `12` for walking against Jarno's real workouts, not `2`.
Using the cloud table for a BLE workout would have shown "Activity 4"
instead of "Cycling". Fixed by giving `activityName()` a `source`
parameter that picks between the existing cloud table and a new,
separately-maintained `bleActivityName()` (currently just the two ids
confirmed above, same graceful numeric fallback for anything else).

**A related display fix**: `WorkoutDetailPage.qml` used to show
Ascent/Descent unconditionally, which is fine for cloud workouts (the
API always reports them, `0` included for a genuinely flat session) but
would show a false "0 m" for every BLE workout, since `Logbook::decode()`
leaves those fields at `0` to mean *absent*, not *zero* - the same
convention `Workout`'s own header comment already documents for
`maxSpeed`/`energyConsumption`/`stepCount`. Made Ascent/Descent
conditional (`> 0`) the same way those already are, rather than passing
`source` through to gate them more precisely - accepting the same
small, already-accepted tradeoff (a genuinely-zero cloud value would
also be hidden) the existing fields already live with.

## What `libmds.so` says about `/Entries` (and altitude, as a bonus)

Picking the `/Entries`-listing question back up, Jarno asked whether the
official Android app's APK could help. Disassembling its native
`libmds.so` (dynamic C++ symbols intact - class/method names, not just
addresses) turned up real, current answers rather than more guessing:

- **`getEntries`/`getData`/`getDescriptors`/`getSummary` are all thin
  wrappers around one generic mechanism.** `SDS::Logbook::getEntries()`
  doesn't hand-roll any binary protocol itself - it calls
  `SDS::WB::Sync<SDS::JsonBody>::op(...)`, the exact same generic
  "Whiteboard sync" call every other Logbook resource method uses (all
  found as real exported symbols: `SDS::Logbook::getData`,
  `getDescriptors`, `getSummary`, `getDataBinaryFromDevice`,
  `readResourceById`, `parsePath`). **This confirms the handle-walk this
  document spent so much effort on for `/Data` is not `/Data`-specific at
  all** - it's `/Entries`'s mechanism too, implemented once, generically,
  underneath every Logbook call. Whatever is eventually learned about it
  from one resource should generalize to the others.
- **The actual wire-level serializer has a name**: `whiteboard::
  protocol_v9::Serializer`/`Deserializer`/`ChunkSerializer`/
  `UnknownStructureDeserializer` - a real, versioned, general-purpose
  binary RPC layer, not ad-hoc per-resource code. `UnknownStructureDeserializer`
  in particular strongly suggests it deserializes structures generically
  *using the same kind of self-describing schema* this project already
  found and reverse-engineered by hand (`docs/sml-schema-descriptors.md`'s
  `<PTH>`/`<FRM>`/`<GRP>` catalog) - i.e. the Descriptors mechanism isn't
  a side detail, it's *how the generic deserializer knows what shape to
  expect*.
- **This generic layer's actual implementation is not in any public
  repo.** Suunto's own open-source firmware SDK,
  [`movesense-device-lib`](https://bitbucket.org/movesense/movesense-device-lib),
  publishes `MovesenseCoreLib/include/whiteboard/` - real headers,
  confirmed to be the same Whiteboard framework - but only the
  *device/firmware*-side interfaces (`ResourceTree`, `ResourceClient`,
  `ResourceProvider`), not the phone-side path-to-descriptor-id
  resolution logic `protocol_v9` implements. That part stays proprietary,
  compiled only into `libmds.so` - see `docs/sml-schema-descriptors.md`'s
  "Confirmed against the real source" section for what *was* found there
  (the SBEM format itself, fully confirmed against Suunto's own public
  header, including a complete explanation of the `<GRP>` mechanism this
  project had only partially worked out empirically).
- **Bonus, unprompted confirmation of the altitude finding**:
  `SDS::LogbookDecoder::applyAltitudeOffsets(SDS::SampleData&)` and
  `applyTrackRunningCorrection(SDS::SampleData&)` exist as real functions
  in the app's own decode pipeline - i.e. the app *does* apply a
  client-side correction to altitude (and to GPS/pace, "track running
  correction") after decoding the raw watch data, before displaying it.
  This is exactly the "DEM/map-correction" hypothesis this document's
  altitude section proposed as the leading explanation for why no raw
  byte in `/Data` matched the app-displayed altitude - now corroborated
  by a real function name in the app doing exactly that, not just a
  plausible guess.

**Practical next step, cheap because it reuses existing infrastructure
entirely**: since `/Entries` is now confirmed to go through the identical
generic mechanism `/Data`'s bulk-fetch shortcut already works against,
the natural, low-cost experiment is trying
`MdsWhiteboardClient::fetchLogbookData()` - already fully generic, not
`/Data`-specific in its own implementation - directly against
`"/Logbook/Entries"` and seeing what comes back, before investing more
effort in disassembling `protocol_v9` itself.

**Result: the shortcut doesn't work for `/Entries`** - Jarno tried
`testEntriesFetch()` on real hardware (2026-09-22) and got a clean
timeout, "No bulk data arrived before the silence timeout" - the same
safe, informative failure mode this project's earlier wrong guesses have
all hit. Makes sense in hindsight rather than being a surprise: `/Data`
is a genuinely large payload (tens of KB compressed) that plausibly
*needs* the dedicated streaming mechanism (`TYPE=0x10` trigger →
`TYPE=0x01` flood) `Mds::encodeStreamStartTrigger()` targets; `/Entries`
is small (a list of maybe dozens of workout ids), so it was never
actually a good candidate for *that specific* mechanism - it almost
certainly still resolves through the ordinary, smaller handle-walk
(`TYPE=0x0d`/`0x05` request/response pairs, single small messages, no
bulk stream at all) this document's earlier sections already traced part
of for `/Data`'s own preliminary walk, and that mechanism's general
grammar is still not understood well enough to implement.

**Where this leaves things**: getting a real on-device workout listing
still needs either (a) generically parsing that ordinary handle-walk
response format (the actual hard problem, `protocol_v9`'s implementation
not being public - see above), or (b) a different, narrower path that
sidesteps `/Entries` for the common case - e.g. a workout already visible
via cloud sync already carries its own start time, which *is* the BLE
logbook id (confirmed earlier in this document), so "fetch richer BLE
detail for an already-cloud-known workout" doesn't need `/Entries` at
all, only a *watch-only, never-cloud-synced* workout would. Jarno chose
(a) - see below for how far that got.

## Decompiling `protocol_v9` itself with Ghidra

Jarno asked whether a Java runtime was needed to decompile the APK
properly. Not for the APK/DEX side (`androguard`, already Python-based,
no JVM needed) - but for *this* problem specifically, disassembly alone
(the `objdump` work earlier in this document) wasn't going to be enough:
reading raw AArch64 assembly by hand doesn't scale to understanding a
whole recursive parser's control flow. A real decompiler does, and
Ghidra (NSA, free) is one - and yes, it's Java-based.

**Getting a JRE and Ghidra running without root, for the record** (this
environment has no passwordless sudo): `apt-get download` fetches a
`.deb` without installing it, and `dpkg -x <deb> <dir>` unpacks it into
an arbitrary directory - no privilege needed for either. Used this for
`openjdk-21-jdk-headless` (+ `ca-certificates-java`/`java-common`) and
separately for `binutils-aarch64-linux-gnu` (a working `objdump`/
`readelf`/`nm` for AArch64 binaries, needed earlier in this document
too - the system's native `objdump` only understands x86-64). Two real
snags, both fixable: the extracted JDK's `conf/security/*` files are
symlinks to absolute `/etc/...` paths that don't exist outside a real
install - copied the real files (also present in the extracted tree,
just under `etc/`) over the dangling symlinks; and Ghidra's headless
launcher insists on a full JDK (`javac` etc.), not just a JRE, for
reasons unrelated to analysis itself. `ghidra_<version>_PUBLIC.zip` from
the project's GitHub releases needs no further installation - unzip and
run `support/analyzeHeadless`.

**What Ghidra's decompiler actually showed**, from the same real
`libmds.so`, now as readable pseudo-C instead of raw disassembly:

- **`SDS::Logbook::parsePath()`** - purely local, no wire I/O: splits the
  input path string and matches segments against literal resource names
  (`"byId"`, `"Data"`, `"Entries"`, `"Summary"`, `"Synced"`/`"synced"`-
  looking segments, etc. - compared as packed 4-8 byte integer
  comparisons rather than string calls, the compiler's own
  optimization) to populate a `LogbookResource` enum. Confirms the
  resource names this document already knew about; doesn't touch the
  wire protocol.
- **`whiteboard::protocol_v9::Deserializer::deserializeValue()`** - the
  actual per-value wire format, now concrete rather than inferred: every
  value starts with a **2-byte type tag + 1 flag byte**, then its data.
  Length is determined per type-tag: most scalar types use a fixed
  per-type byte count (via a lookup table the decompiler couldn't fully
  resolve the contents of - see below); type tag `0xc` is a
  **NUL-terminated string** (walks byte-by-byte for the terminator, no
  separate length prefix); type tag `0xd` needs a fixed 8+ bytes (very
  likely a 64-bit scalar - `int64`/`double`/a timestamp); any tag above
  `0xe` is treated as `0xf` and decoded as a **structure/array with a
  9-bit length prefix** (`& 0x1ff`, i.e. up to 511) packed into the next
  2 bytes alongside more flag bits.
- **`whiteboard::protocol_v9::UnknownStructureDeserializerImplementation::
  deserialize()`/`deserializeProperty()`** - the actual handle-walk
  engine: a **recursive, metadata-driven structure walker**. Each
  property carries a **nullable/optional bit** - a presence byte is read
  first (0 = absent, calls the visitor with a null marker and moves on
  without recursing; non-zero = present, recurse via `deserialize()`
  for that property's own sub-structure/value) - and dispatches to a
  **virtual "visitor" callback** (an interface pointer stored in the
  deserializer object, called through its vtable) for every
  value/structure encountered, rather than building a fixed in-memory
  tree directly. This is precisely the general shape this document's
  earlier "handle-walk" tracing (the `0x0b`/`0x0d`/`0x03`/`0x05`
  exchanges) was circling without being able to name: a real,
  general-purpose, **self-describing recursive binary structure
  format**, metadata-typed the same way this project's own
  `docs/sml-schema-descriptors.md` `<PTH>`/`<FRM>`/`<GRP>` catalog is
  metadata for the *SBEM* side of things - same idea, different (and
  separate) type system for the *Whiteboard RPC* side.

**What this doesn't yet give**: a working generic decoder. Three real
gaps stood between "the algorithm's shape is understood" and "this can
parse a real `/Entries` response" - see the follow-up section right
below for how far these got resolved in a second decompilation pass:
1. ~~The scalar-size lookup table's actual per-type byte counts weren't
   recovered~~ **Resolved, see below.**
2. The exact C++ layout of `whiteboard::metadata::DataType`/`Property`
   (the structures the walker reads its own instructions from) wasn't
   extracted - needed to know, for a given resource, exactly what
   sequence of typed properties to expect. **Still open.**
3. Most importantly: **where do those `DataType`/`Property` metadata
   tables themselves come from for a specific resource like
   `/Entries`**? Baked into the app at compile time (in which case
   they're extractable from `libmds.so`'s own data sections, a further
   Ghidra pass), fetched from the watch dynamically (in which case
   they'd be the `Descriptors` mechanism this project already partly
   understands from `docs/sml-schema-descriptors.md`), or some
   combination - not established either way. **Partial new evidence
   below suggests `/Entries` may not even go through this mechanism at
   all.**

**Honest scope assessment**: this is real, substantial, concrete
progress - going from "an opaque byte sequence" to "a specific, named,
partially-decompiled recursive binary RPC protocol with a documented
value-encoding scheme" is not nothing. But turning it into working code
that can parse a live `/Entries` (or generic handle-walk) response is
its own multi-step engineering task from here, not a quick follow-on.

## Second decompilation pass: the scalar-size table, and `/Entries` may use a different mechanism than `/Data`

Continuing directly from the three gaps above, using the same Ghidra
project (no new import needed - `analyzeHeadless ... -process "libmds.so"
-noanalysis` reuses the existing analysis in seconds).

**Gap 1, resolved.** The earlier attempt to dump
`whiteboard::SCALAR_VALUE_DATA_SIZE`'s bytes went through manual ELF
virtual-address arithmetic and got the address wrong (this `.so` is a
PIE and Ghidra loads it at image base `0x00100000`, not `0`, so a raw
ELF-header vaddr and a Ghidra address are offset from each other) - the
bytes it dumped were actually a `whiteboard::BufferPool::vtable`
pointer from a nearby, unrelated location, not the table. Re-run
properly, resolving the symbol's own address directly through Ghidra's
API instead of hand-computed offsets, gives the real table cleanly:

```
whiteboard::SCALAR_VALUE_DATA_SIZE = { 0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 2, 0, 0, 3 }
                              index:   0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
```

16 entries - consistent with a 4-bit type-tag nibble indexing straight
into it. `deserializeValue`'s already-decompiled code special-cases
`0xc` (NUL-terminated string), `0xd` (fixed ≥8 bytes, handled directly)
and capped-`0xf` (9-bit length-prefixed structure/array) rather than
consulting this table for those three tags - the values at those
indices here (2, 0, 3) are consistent with being unused/overridden by
that special-casing rather than real byte-counts. Indices 0-11 read as
a plausible small-integer/float type enum (`0`=none, `1`=bool/int8 x3,
`2`=int16 x2, `4`=int32/float x2, `8`=int64/double x2) - a reasonable,
if not yet independently confirmed, guess at the concrete type-tag
assignments.

**Gap 3, partial but important new evidence.** Read the not-yet-examined
decompiled bodies of `SDS::Logbook::getDataBinaryFromDevice` (the
function behind `/Data`) and `SDS::Logbook::getEntries` (behind
`/Entries`) side by side. They are **not** the same shape:

- `getDataBinaryFromDevice` is short and calls two named,
  already-understood methods directly: it first tries
  `subscribe(this, "Data", logbookId, ...)`; if that doesn't return
  `200`, it optionally calls `getDescriptors(...)` (gated by a bool
  parameter) and then falls back to `getStream(this, "Data", logbookId,
  ...)` - and a `getStream` success is treated the same as a direct
  `subscribe` success. **This is exactly the mechanism this project's
  own BLE shortcut (`Mds::encodeStreamStartTrigger`, the proven-working
  `/Data` fetch) already implements** - independent confirmation, from
  the real app's own code, that going straight to the stream-start
  trigger without the full handle-walk is not a lucky guess, it is
  what the official client itself falls back to.
- `getEntries`, by contrast, is a large (~760-line decompiled),
  JSON-heavy function (`wbjson::Json` in/out) that builds up a small
  *list* of candidate path strings (at least two seen in the
  disassembly: one ending in `...Entries`, a second, longer ~40-byte
  one also ending in `...ries`, likely a differently-prefixed variant
  such as a `Mem/`-qualified path) and works with `SDS::Status`/
  `Header` objects - but **contains zero references to `subscribe`,
  `getStream`, `getDescriptors`, or any `protocol_v9`/handle symbol by
  name** anywhere in its body (confirmed by grepping the full
  decompiled function text). Whatever it calls to actually perform the
  fetch is reached through an unresolved vtable dispatch, not a direct,
  named call the way `getDataBinaryFromDevice`'s two attempts are.

**Correction, same pass, a few minutes later**: the "maybe `/Entries`
needs a `subscribe`/notify verb instead of `getStream`" idea above was
wrong - it came from grepping `getEntries`'s decompiled body for the
literal names `subscribe`/`getStream`/`getDescriptors` and finding
none, without also checking for `Sync`/`op`. It does call those -
`WB::Sync<SDS::JsonBody>::op(&local_200, &local_110, 1, pbVar12,
&local_250, 10000, 1, 1)`, twice (once per candidate path string,
retrying the second, longer path if the first fails). Reading `op`
itself (`SDS::WB::Sync<SDS::JsonBody>::op`, a separate ~90-line
decompiled function, address `0x009f84f0`) confirms its real signature:
`op(wbclient::Operation verb, string uri, string contract, unsigned
long timeoutMs, bool, bool)` - `getEntries` passes verb `1` for both
attempts, timeout `10000`. **Verb `1` is the same numeric value this
project's own hand-rolled `encodeGetRequest()` already uses for its
`[0x01 verb]` body byte** - i.e. `/Entries` is fetched with an ordinary
GET, exactly like `/Data`, not some other verb. `op()`'s own body is
generic plumbing (builds a callback closure, calls `SyncBase<JsonBody>
::waitFor(...)` with a 5000ms timeout, invokes it) - it doesn't reveal
resource-specific behaviour, it's the same call machinery presumably
used for every Whiteboard resource fetched as JSON.

**What this actually establishes**: the fork between `/Data` and
`/Entries` isn't in which verb is sent - both start with the same GET.
It's in what happens with the *response*. `/Data`'s payload is an
opaque binary blob, so once the watch acks the GET, a direct stream-
start trigger derived from that ack is sufficient to make it start flowing
(this project's proven shortcut). `/Entries`'s payload is a *structured*
list of `LogEntry` records, which - per everything this document and
`docs/sml-schema-descriptors.md` have found about `protocol_v9` -
gets pulled apart field-by-field over the wire via the handle-based
walk (`0x0b`/`0x0d`/`0x03`/`0x05`), because that walk *is* the
mechanism by which a self-describing structure gets transmitted at all,
not an optional priming step. There is no equivalent one-shot shortcut
to skip it for a structured resource - that was a real, freestanding
hope, and it doesn't survive this closer look.

**Honest conclusion**: solving `/Entries` for real requires either (a)
fully implementing the generic handle-walk (tracking a handle from each
response, issuing the next `0x0b`/`0x0d` request referencing it - gaps
2/3 above, the `DataType`/`Property` struct layout and where the
per-resource metadata comes from), or (b) a narrower, hand-crafted
walker built by re-tracing the original capture's exact `/Entries`
handle sequence step-by-step (skipping full `protocol_v9` generality,
the same way `Logbook::decode()` hand-crafts known SBEM chunk semantics
instead of implementing a generic SBEM/SML interpreter) - a real,
multi-step task either way, not a small follow-on.

## Cleanly re-decoded `/Entries` handle-walk trace, and a first real wire-level pattern

Picked option (b) up directly: re-ran the original capture's `wb_traffic.tsv`
through a small Python re-implementation of `Mds::Decoder` (same
SLIP-unescape-with-escape-state-tracking logic, not naive splitting -
several of these frames span 2-3 BLE PDUs, e.g. the initial `/Entries`
ack's 52-byte response) and reassembled every WRITE and NOTIFY frame
for the whole `/Logbook/Entries` exchange (frames 6644-6700,
reqid `0x04af`-`0x04bd`, 15 request/response pairs) cleanly, for the
first time - this is genuinely new derived data, not previously
captured this legibly anywhere in this project's docs or scratch files.

**A first real structural pattern, confirmed against 2+ examples, not
guessed:**
- Every `0x0d` request body and its matching `0x05` response body
  share the same first 3 bytes: `f0` (the fixed sub-request marker,
  matching zappctl's documented framing) followed by a 2-byte value -
  e.g. request/response pair for reqid `0x04b0` both start
  `f0 00 03 ...`, pair `0x04b7` both start `f0 00 0c ...`. This 2-byte
  value functions like a structure/property id the request is asking
  about and the response is confirming it answered.
- **That 2-byte id is not invented by the client - it's read out of a
  specific byte offset in an *earlier* response**, the same kind of
  handle-propagation this document already found for `/Data`'s
  simplified trigger. Confirmed concretely: request `0x04b2`'s id bytes
  (`84 01`) appear at byte offset 14-15 of the *previous* response
  (`0x04b1`, right after that response's own `34 8e 3f` sub-field) -
  not at the tail, which is why a naive "does the request's suffix
  appear anywhere in the previous response" scan missed it at first
  pass. A working generic (or hand-crafted) walker needs to know which
  offset in a given response shape yields the next id - this differs
  by what kind of structure is being walked (a fixed few bytes into a
  property-list response vs. a fixed few bytes into a value response),
  and figuring out that offset rule for every response shape in this
  trace is the remaining work, not yet done.
- Real field/type names recovered directly from the response bodies'
  embedded ASCII, in walk order: `StartAfterId` (0x04b7), then
  `IncludeSummaryOnly` (0x04b8) - these two are the `/Entries` request's
  own *parameters* (a `LogEntries` GET can apparently be filtered
  incrementally by log id and take a summary-only flag) - followed by
  `LogEntries` (0x04ba, the response structure's own type name) and
  `elements` (0x04bc, its array field). This matches the parameter
  names already known from the original capture's earliest inspection
  (see this document's much earlier "Post-probe finding" section) but
  now with the exact surrounding bytes attached, not just the strings
  in isolation.

**Not yet done, and why this is a real stopping point to check in on**:
turning "the pattern is visible and one offset-propagation instance is
confirmed" into "a working decoder" needs the *rule* for every step's
offset, derived from a single historical capture with no way to
cross-check a wrong guess except a real-hardware timeout (same
low-risk-to-try, high-cost-to-fully-verify shape as everything else in
this project) - genuinely more hours of the same careful byte-tracing
that solved `/Data`'s trigger, not a quick finish from here.

## Third decompilation pass: the real wire structure header and the real recursive walk algorithm

Checked [`suunto-git`](https://github.com/orgs/suunto-git/repositories)
(Suunto's own GitHub org, Jarno's suggestion) first - all 14 public
repos are iOS app dependency mirrors for the Chinese market (WeChat,
Douyin, Xiaohongshu, Alipay, Amap SDKs) plus an unrelated headset
product line and a watch-payment feature; nothing BLE/Whiteboard/MDS-
related. Not useful here, ruled out quickly.

Back on the handle-walk: `nm -C -D` on `libmds.so` turned out to
export **far more of `protocol_v9` than the original 32-function
target list captured** - the whole class hierarchy is there with real
names: `StructureDeserializer`, `StructureVisitorBase`,
`UnknownStructureDeserializer`, `StructureSerializationLengthCalculator`,
etc. Decompiled the most promising few
(`StructureDeserializer::deserializeHeader`/`deserialize`,
`StructureVisitorBase::process`) in a third pass (same Ghidra project,
still no new import needed) and this is a real breakthrough - the
actual wire-level structure header format and the actual recursive
walk algorithm, from Suunto's own compiled code, not inferred from
capture bytes alone:

**The structure header (`StructureDeserializer::deserializeHeader`,
2 bytes, confirmed against the decompiled logic, not the capture)**:
- Byte 0: a value used later as a 9-bit validation/consistency check
  against what the walk actually consumes (`deserialize`'s
  `bVar2 = *(byte*)param_2` folded together with bit 0 of byte 1) -
  not yet tied to a concrete field meaning, but clearly a checksum-like
  guard, not real data.
- Byte 1, bits 1-3 (3-bit field, value 1-7, 0 = "no alignment"):
  selects an **alignment mask** from a small table
  (`DAT_004c2610`, dumped directly from `libmds.so`'s own `.rodata`):
  `{1, 3, 7, 15, 31, 63, 127}` for selector 1-7 - i.e. round the
  payload start up to a 2/4/8/16/32/64/128-byte boundary. Byte 1 bits
  4 and 5 are separately read out by `deserializeHeader` itself into
  two output bools (purpose not yet determined - possibly
  "hasOptionalFields"/"isPartial"-type flags, consistent with similar
  bit-flag roles seen elsewhere in this protocol).
- The actual structure **payload starts at `buffer + 2 + padding`**,
  where `padding = alignmentMask & -(buffer + 2)` (the standard
  round-up-to-alignment bit trick).

**The recursive walk (`StructureVisitorBase::process`)**: given a
`whiteboard::metadata::DataType` (fetched separately, see below - not
part of the wire bytes) and a payload pointer, dispatches on the
`DataType`'s own first byte (its "kind", a *different*, smaller enum
than `deserializeValue`'s 0-15 wire value-type tag - only 0, 2, 3 seen
here):
- **kind 0 (leaf)**: a scalar or, if the `DataType`'s sub-field at
  offset 2 equals `0xc`, a string - calls the visitor's `visitString`.
- **kind 3 (structure)**: calls `visitSubStructure`, then asks the
  metadata provider for the structure's **property-id list**
  (`-1`-terminated), and for each property id looks up that property's
  own metadata (a flags field controlling whether it's a pointer to
  indirect through, whether it's optional/nullable, and its own
  alignment/offset contribution) and recurses `process()` into it at
  the computed offset - a genuine metadata-driven field-by-field walk.
- **kind 2 (array)**: calls `visitArray` to get an element count and a
  base offset, then recurses `process()` once per element at
  `baseOffset + i * elementStride`.
- Anything else: not handled (returns "no data").

**This directly answers the most important part of gap 3**: the
`DataType`/`Property` metadata is **not carried in the wire bytes at
all** - `StructureDeserializer::deserialize`'s signature is literally
`(unsigned short typeId, void const* buffer, bool, MetadataMap const&
metadata)`, and it resolves the `DataType` via
`IDataTypeMetadata::getBaseDataType(metadataMap, &typeId)` *before*
touching the buffer. The wire-level header above only carries
alignment/validation bits, never structure shape. Where the
`MetadataMap` itself gets populated (baked into `libmds.so`'s own data
for known resources vs. built from the watch's own `Descriptors`
response) is still open, but the lookup *mechanism* - a small integer
type-id resolved through a metadata map, independent of the connection
- is now concretely known, which is real progress on gap 2 as well
(the property list's per-property flags word, read as
`*(ushort*)(propertyMeta+4)`, is exactly the `Property` struct's own
layout gap 2 asked about - now narrowed to one specific field within
one specific struct, not a complete unknown).

**Where this leaves `/Entries` in practice**: enough of the real
algorithm is now understood to write a decoder, but it needs a
hand-written `DataType`/`Property` tree for `LogEntries`/`LogEntry`
(matching the field names already recovered: `StartAfterId`,
`IncludeSummaryOnly` as the request's own parameter structure;
`LogEntries` → `elements` → presumably `Id`/`ModificationTimestamp` per
entry) rather than a live `MetadataMap`, since building the general
"fetch descriptors, populate a MetadataMap generically" machinery is
substantially more work than this one resource needs. That hand-written
tree, plus a small implementation of `deserializeHeader`/`process`'s
logic above, is the concrete remaining task - scoped much more tightly
now than "solve `protocol_v9` in general," but still a real
implementation effort, not a one-line fix, and - like everything else
in this project - only checkable against a real device, not from this
sandbox.

## `/Entries` shortcut #2: skip the whole descriptor walk, not just understand it

Kept re-tracing the capture past the 15-step descriptor walk (reqid
`0x04af`-`0x04bd`, all schema/field-name traffic - `StartAfterId`,
`IncludeSummaryOnly`, `LogEntries`, `elements`, `LogEntry`, `Id`,
`ModificationTimestamp`, `Size`) and found it continues: at reqid
`0x04c5`, the app sends one more `TYPE=0x0D` request whose body is just
`[0xF0][the ORIGINAL ack's own 2-byte handle][0x01 0x80 0x00][0x00]` -
**the same handle named by the very first GET's ack (reqid `0x04af`,
frame 6646), not anything produced by the 14 intervening steps.** Its
response is the real data: three `LogEntry` records, each 24 bytes,
decoding (after the shared 8-byte response prefix and a 2-byte
`protocol_v9` structure header - see above) to `[id: uint32 LE]
[modificationTimestamp: uint32 LE][16 more bytes, not yet decoded]`
repeated.

**Confirmed correct, not just plausible**: the three decoded `id`
values - 1785740504, 1785760357, 1788194033 - are not novel numbers
invented for this analysis. They are the *exact* logbook ids
independently captured elsewhere in this same HCI log as literal ASCII
path segments in separate, unrelated requests:
`/Logbook/byId/1785740504/Data` (frame 7826), `/Logbook/byId/
1785760357/Summary`, `/Logbook/byId/1788194033/Summary`. Three
independent byte-for-byte matches against real, separately-captured
resource paths is about as strong a confirmation as this project can
get without new hardware access - this is genuinely decoded, not
guessed.

**This means the whole 15-step schema walk (`0x04b0`-`0x04c4`) is
skippable** for actually fetching the entries list, exactly the same
shape of shortcut this document already found for `/Data`'s stream
trigger: go straight from the initial GET's ack to the one request that
actually answers with data, instead of replaying (or trying to
generically reimplement) the descriptor introspection the official app
does in between. Whether that walk serves some other real purpose (a
one-time, cacheable schema fetch the app does before ever needing to
re-fetch it) or is pure overhead for this specific request shape is
still open, but irrelevant to getting the entries list itself.

**Implemented and unit-tested (not yet run on real hardware)**:
- `Mds::encodeEntriesFetchTrigger(requestId, ackBody)`
  (`src/ble/mdswirecodec.h`/`.cpp`) - builds the 7-byte-body `TYPE=0x0D`
  request from a GET's ack body. Golden-vector-tested byte-for-byte
  (CRC32 included) against the real captured frame (reqid `0x04c5`) in
  `tests/test_mdswirecodec.cpp`.
- `LogEntries::decode()` (`src/ble/logentriesdecoder.h`/`.cpp`, Qt-free)
  - decodes a response body into `std::vector<LogEntries::Entry>{id,
  modificationTimestamp}`. Tested in
  `tests/test_logentriesdecoder.cpp` against the real captured response,
  with the three-way real-id cross-check above spelled out in the test
  file's own comment. Only `id`/`modificationTimestamp` are decoded -
  the remaining 16 bytes per record (likely including the `Size` field
  this project separately recovered the name of) are left undecoded
  rather than guessed at.
- `MdsWhiteboardClient::fetchLogEntries()` - GET, then the new trigger,
  then `LogEntries::decode()`, following the same queued/serialized
  request pattern as `get()`/`fetchLogbookData()`.
- `AppController::testEntriesFetch()` - now calls `fetchLogEntries()`
  instead of the old (confirmed-failing) `fetchLogbookData()` reuse
  attempt, reporting the decoded entry ids via `logbookTestResult()`.

**What's NOT confirmed**: whether the fixed byte offsets this decoder
assumes (entry count position, 24-byte record stride, records starting
16 bytes into the array payload) hold for a different number of
entries, a list large enough to need pagination (the `StartAfterId`
parameter strongly suggests pagination exists - this capture's list of
3 may just be short enough to fit in one response), or a different
alignment selector than this one response happened to use. Like every
other shortcut in this project, a wrong guess should fail safely (an
empty result or a decode that doesn't match, not a crash) - the real
test is Jarno running `testEntriesFetch()` against his own watch.

**Gate: ✅ PASSED 2026-09-22.** Jarno ran `testEntriesFetch()` against
the real Race: `OK - 3 entries: 1785740504, 1785760357, 1788194033` -
an exact match, in order, to the three ids this document predicted from
offline analysis of the historical capture alone, before ever touching
real hardware. This is the strongest possible confirmation available
without a differently-shaped watch state to test against: the whole
chain (GET, `Mds::encodeEntriesFetchTrigger()`, the real watch's live
response, `LogEntries::decode()`) works end to end on real hardware,
not just against a replayed capture. `/Logbook/Entries` no longer
needs a hand-typed logbook id - this closes the "still open" item that
has stood since breakthrough #4.

**Still genuinely open**: this watch's real entries list happened to
be exactly 3 long, same as the capture this was derived from, so
pagination (the `StartAfterId` parameter) and a different alignment
selector remain untested - not a reason to doubt this result, just an
honest note that "works for a 3-entry list" isn't yet "works for any
list size."
