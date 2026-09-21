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

## Provenance and what's still open

**Provenance**: the exact `MDS_HEADER_SIZE`/`MDS_CHUNK_SIZE_OFFSET`
constants and the Heatshrink parameters came from
[libdivecomputer](https://github.com/libdivecomputer/libdivecomputer)
PR #73 (`suunto_nautic.c`/`suunto_nautic_parser.c`, LGPL-2.1 per that
project's `COPYING` file - **not vendored into this project**, only read
for reference, same as `zappctl`'s docs were used earlier), which adds
support for the Suunto Nautic/Ocean **dive computer** - a different
device family on the same underlying Suunto/Movesense BLE firmware
stack. That driver's own comments note its chunk-id constants
(`CHUNK_GPS_ACCURACY`, `CHUNK_DIVEROUTE_FEATURES`,
`CHUNK_SURFACE_PRESSURE`, ...) are diving-specific - three of those four
literal chunk IDs (`0x0f`→HR, `0x12`→1Hz profile, `0x16`→extended
status) happened to reappear with plausible-looking frequencies in this
project's own Suunto Race capture (a sports watch, not a dive computer),
which is a good sign the *container format and transport* are fully
shared firmware infrastructure - but the exact chunk-id → field mapping
for a Race/Vertical-generation *sports* watch hasn't been independently
confirmed chunk-by-chunk, only the format mechanics (framing,
compression, TLV walk) have.

**Still open** (this is where Phase 6/`LogbookSync` picks up next):
1. **Decode chunk `0x12`'s (and others') internal value structure** -
   this is presumably where GPS/pace/etc. samples live, but the raw
   bytes inside each chunk haven't been mapped to real numbers yet.
2. **Cross-reference chunk IDs against the 246-field SML schema** this
   project already extracted from the watch itself
   (`docs/sml-schema-descriptors.md`) - the two were captured from the
   same device/firmware and almost certainly describe the same data,
   just via two different self-description mechanisms (the `<GRP>`
   comma-separated id lists seen alongside the `<PTH>`/`<FRM>` schema
   entries, also captured in this same session, are a promising lead:
   they're small integers in a similar range to these SBEM chunk ids).
3. **Decode the one-shot `0x01`-`0x08` header chunks** at the start of
   the container - likely per-workout summary values (duration,
   distance, start time) that would be the fastest way to validate any
   proposed field mapping against Jarno's own known real workout stats.
4. Confirm this same pipeline holds for a workout **as it's actively
   streamed live** (this capture was of the official Android app doing a
   historical sync, presumably after the workout already ended) -
   probably fine, no reason to expect otherwise, but not yet exercised.
