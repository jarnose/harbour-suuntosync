# SML field schema, captured live from a Suunto Race over BLE

## What this is

`sml-schema-descriptors.txt` (246 lines) is the raw `<PTH>...<FRM>...` field
catalog extracted from a real Whiteboard `Descriptors` response (captured
2026-09-21 - see the original Android HCI capture, response starting at
frame 7750, t≈389.5s, triggered by a request in the
`/Logbook/byId/<id>/...` exchange - the exact triggering request path
wasn't pinned down precisely before this session paused, see "Open
questions" below).

Each line is one field definition:

```
<PTH>Samples.TimelineSample.Attributes.suunto/sml.Header.Duration.<FRM>uint32
```

`<PTH>` gives the dotted field path (matches Suunto's public SML XML schema
exactly - cross-checked against `stacksjs/ts-watches`' `SuuntoDriver` SML
parser, which reads the *same* field names, e.g. `Header.Duration`,
`Header.Distance`, `Sample.HR`, `Sample.Longitude`, `Sample.UTC` - from
Suunto's cloud-exported `.sml` XML files). `<FRM>` gives the primitive wire
type: `uint8/16/32`, `int16/32`, `dint16`, `float32`, `bool`, `utf8`,
`local64` (a timestamp), or `enum` (sometimes followed by `:0=Foo,1=Bar,...`
value names, captured verbatim where seen).

This is a huge deal for `LogbookSync`/`WorkoutStore`'s BLE side (see
`~/.claude/plans/agile-hopping-harp.md`, Phase 6): it means the actual
per-workout `Data` binary blob is a stream of `(fieldId, rawValue)` pairs
against *this exact, already-known* schema, not an opaque format that needs
separate reverse engineering. `Header.*` fields are one-shot summary values
(duration, distance, ascent, energy, HR zones, ...); `Sample.*` fields
repeat once per timeline sample (HR, GPS lat/lon, altitude, speed, cadence,
power, UTC timestamp, ...); `Windows.Window.*` are per-interval/lap
aggregates; `Zapps.Zapp.*` describes SuuntoPlus app-defined custom channels
(same `Channels` concept zappctl's `docs/PROTOCOL.md` mentions from the
app-install side).

## What's confirmed vs. still open

**Confirmed** (high confidence, directly read off the wire):
- The exact set of 246 field paths and their primitive types, for this
  Suunto Race's firmware version specifically. A different watch/firmware
  may expose a different field set - re-capture if this stops matching.
- This is the same schema as Suunto's public cloud SML export format, so
  units/semantics can be cross-checked against that (e.g. `stacksjs/
  ts-watches`' SML parser, or Suunto's own documentation if any exists) once
  actual values are being decoded, rather than guessed from the field name
  alone.

**Open** (next concrete work, not done yet):
1. **The exact 2-byte field-ID encoding.** Every entry is preceded by what
   looks like a little-endian uint16 (values increasing roughly
   sequentially, e.g. 0x0020, 0x0021, 0x0023, ... in the order fields were
   captured) - but the precise byte offset/boundary wasn't nailed down
   before this session paused; the raw capture (`wb_traffic.tsv` in that
   session's scratchpad, or a fresh capture) needs another careful pass to
   confirm exactly which 2 bytes are the ID for each entry and rule out an
   off-by-one.
2. **Which exact request produces this Descriptors response**, and whether
   it's per-logbook-entry or a fixed/global schema (i.e. does every watch
   firmware version return the identical descriptor list once, cacheable,
   or does it vary per logbook entry depending on which SuuntoPlus Zapps
   were active during that specific workout?).
3. **Decoding an actual `/Logbook/byId/<id>/Data` response** using this
   schema - i.e. does `Data` actually contain `(fieldId, value)` pairs
   referencing these same IDs, and in what framing (length-prefixed?
   fixed-size records per Sample?) - not yet attempted.
4. Units/precision for several fields aren't obvious from the name alone
   and need either cross-referencing against the public SML format or
   empirical calibration against a known real workout (e.g. `Header.Energy`
   - joules or kcal; `precision=` suffixes seen on some `<FRM>` tokens, e.g.
   `uint32,precision=1,nillable=4294967295` for `Sample.Distance`, hint at a
   scale factor and a sentinel "no data" value that need confirming).

## Provenance

Extracted by reassembling and CRC-verifying the raw Whiteboard frames from
the same Android HCI snoop log used throughout Phase 0a/6 (see
`~/.claude/plans/agile-hopping-harp.md`) - not from any external/public
source. Cross-referenced against `stacksjs/ts-watches`
(`packages/ts-watches/src/drivers/suunto.ts`, MIT) purely to confirm the
field *names* match Suunto's known public SML format; that project's parser
itself was not used and doesn't talk BLE.
