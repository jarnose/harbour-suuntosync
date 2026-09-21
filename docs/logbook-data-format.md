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

**Altitude remains open.** It can't come from 2D lat/lon the way
distance/speed can, so it must be a genuinely separate field somewhere -
but it wasn't found in `0x0c`'s two still-unaccounted-for byte pairs
(offsets `8-9` and `18-19`), whether read as `float32`, raw `int16`/
`uint16` metres, or `int16`/`uint16` decimetres (×10) - ruled out by
direct comparison against real per-second altitude, not just absence of
a coincidental match. Nor did a similar brute-force scan turn it up
elsewhere in `0x0c`. `0x16` (17 bytes, still fully unexplored) and the
`0x17` barometric-pressure candidate (previously ruled out only as a raw
`float32` Pa value at offset 2 - worth retrying with other encodings,
e.g. a fixed-point or reference-relative delta, given `Header.Altitude
.Min` exists as a named field in the watch's own SML schema catalog and
Suunto's format elsewhere favours delta-from-reference encoding) are the
next places to look.

**Practical upshot**: this is now enough to build a real GPS route, pace,
distance, heart-rate, and activity-type view for a BLE-synced workout -
everything except altitude/ascent/descent and cadence/steps. HR (`0x12`),
activity/sport id (`0x08`), the UTC timeline, and now full GPS (all in
`0x0c`) form a solid, independently-verified foundation. The `.fit`
cross-referencing methodology here (per-second ground truth, brute-force
exact-value search for distinctive fields like GPS coordinates, and
differenced correlation for noisier/less distinctive ones) is the
template for finishing altitude and cadence/steps (`0x1f`, `0x18`) next.
