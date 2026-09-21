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
  main heartbeat chunk on the Race too, even though...

**Refuted / doesn't carry over as documented:**
- **`CHUNK_HEARTRATE` (`0x0f`)**: on the Race, byte 2 (the claimed `hr:
  uint8 bpm`) takes wildly implausible values (0, 3, 4, 9, 255, ...) -
  this id does **not** mean heart rate here. Also structurally
  different: Ocean's HR chunk is 3 bytes, the Race's `0x0f` chunk is
  always 6 bytes.
- **`CHUNK_PROFILE_1HZ` (`0x12`)'s payload**: Ocean's version needs
  `size >= 18` for its temperature decode; the Race's `0x12` chunks are
  always exactly 3 bytes (a 2-byte delta + 1 payload byte) - a
  completely different, much smaller record. Only the *pacing role* of
  this id carries over, not its field layout.
- **`CHUNK_SURFACE_PRESSURE` (`0x17`)**: tried the documented `float32`
  at offset 2 (barometric pressure, Pa) - a very plausible candidate
  since the Race does have a barometer - but it decoded to a flat `0.0`
  on every sample across all three streams, so this offset/field
  doesn't hold either, at least not as a raw Pa float.

**Not yet tested**: `0x0c` (20 bytes, very frequent - a strong GPS/pace
candidate given the size), `0x16` (17 bytes, vs. Ocean's 141/195-byte
`CHUNK_EXTENDED_STATUS` - clearly a different, much smaller record on
Race), `0x18` (5 bytes), and `0x1f` (7 bytes, only seen in one of the
three streams so far, same frequency as `0x12` in that stream - possibly
a paired/companion record). The one-shot `0x01`/`0x02`/`0x03`/`0x04`
header chunks at the very start of the container are also still
undecoded; `0x01` (`CHUNK_TIMELINE_BASE`, 8 bytes) has a suspicious
*constant* 3-byte tail (`01 00 0c`) across all three streams with only
the preceding byte varying, hinting at a version/type marker rather
than workout-specific data, but this isn't confirmed either.

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
2. **Decode `0x0c`/`0x16`/`0x18`/`0x1f`'s internal value structure** -
   GPS/pace/altitude/cadence samples almost certainly live in here, but
   no byte offset has been confirmed for any of them yet.
3. **The fastest path to calibrating the above**: Jarno supplying the
   *known real stats* (sport type, duration, distance, avg HR if
   available) for these three specific captured workouts, to search for
   matching encoded values rather than continuing to guess blind. The
   duration figures above (78.97/61.44/25.88 min) are the first
   candidate to check against reality.
4. Add resync-on-malformed-chunk robustness to `Sbem::parseContainer()`
   (see point 3 under issue #70 above) before relying on it against a
   live BLE download rather than a replayed historical capture.
5. Confirm this same pipeline holds for a workout **as it's actively
   streamed live** (this capture was of the official Android app doing a
   historical sync, presumably after the workout already ended) -
   probably fine, no reason to expect otherwise, but not yet exercised.
