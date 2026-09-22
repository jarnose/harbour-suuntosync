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
1. ~~The exact 2-byte field-ID encoding~~ - **resolved, see "Confirmed
   against the real source" below**: these aren't SBEM chunk ids at all,
   they're `Descriptor::id_t` values from a completely different,
   larger-range id space.
2. **Which exact request produces this Descriptors response**, and whether
   it's per-logbook-entry or a fixed/global schema (i.e. does every watch
   firmware version return the identical descriptor list once, cacheable,
   or does it vary per logbook entry depending on which SuuntoPlus Zapps
   were active during that specific workout?). Still open.
3. ~~Decoding an actual `/Logbook/byId/<id>/Data` response~~ - **done, see
   `docs/logbook-data-format.md`** - though it turned out `/Data`'s SBEM
   chunk ids are a *separate*, much smaller id space than this document's
   `<PTH>`/`<GRP>` catalog, not a direct match (see below).
4. Units/precision - **the encoding is now known precisely** (see "The MOD
   modifier string" below), but which real fields use which modifier
   strings hasn't been re-extracted from a fresh capture yet - the
   original capture's raw modifier strings weren't saved verbatim, only
   the path/format pairs.

## Confirmed against the real source (2026-09-22)

Jarno asked whether the official Android app's APK could help with a
*different*, still-open question (the `/Logbook/Entries` handle-walk - see
`docs/logbook-data-format.md`). Disassembling `libmds.so` (the app's
native Suunto/Movesense client library, ARM64, dynamic symbols intact -
not stripped of C++ names) turned up something more valuable for *this*
document: Suunto's own open-source firmware SDK,
[`movesense-device-lib`](https://bitbucket.org/movesense/movesense-device-lib)
(`MovesenseCoreLib/include/sbem/`), which is the real, public header
declaring the exact same `SBEM0103` format this document reverse-engineered
from scratch. Genuinely independent confirmation, not a guess:

- `Sbem.hpp` declares `SBEM_VERSION_HEADER("SBEM0103")` verbatim - the
  magic this project found empirically in `docs/logbook-data-format.md`.
- **The `<GRP>` mystery is fully explained.** `sbemdescriptor.hpp`'s
  `Descriptor` class has `Type_e_TagOpen`/`Type_e_TagClose`/`Type_e_Value`
  and `isGroup()`/`groupDescriptors()`/`numOfGroupDescriptors()` - a group
  descriptor is a structural "tag" whose value is a **list of its child
  descriptors' ids**. That's exactly this document's `<GRP>32,36,134,210`
  entries - a list of *descriptor ids* (this schema catalog's own
  namespace, `Descriptor::id_t`, `uint16_t`), not SBEM chunk ids. This
  also explains why the earlier attempt to match `<GRP>` numbers against
  `/Data`'s actual SBEM chunk ids failed (`docs/logbook-data-format.md`'s
  "Major breakthrough #2"): they were never the same id space to begin
  with. `plusDescriptor()` is very likely this document's `<DELTAREF>N`
  ("reuses descriptor N's definition with a variant") from the same
  reason - a descriptor that extends another one, same idea as
  `m_plusDescriptor` here.
- **`dint16` confirmed** as "differential int16_t" (delta-encoded) -
  matches this project's own guess when the format first appeared in the
  raw dump, now confirmed rather than assumed. Full format enum:
  `bool, uint8/16/32/64, int8/16/32/64, duint8/duint16 (differential
  unsigned), dint8/dint16 (differential signed), float32/64, local64,
  enum, utf8, bin8, utc64`.
- **The MOD modifier string's exact grammar**: `sbemmod.hpp`'s
  `Mod::init()` parses it as `"x<op><val>,y<op><val>"` where `<op>` is
  one of `+-*/` - i.e. two chained linear operations (`raw x<op>val
  y<op>val` in that order) applied to get the real physical value. This
  is the precise mechanism behind the `precision=`/`nillable=`-style
  suffixes seen on some `<FRM>` tokens in the raw capture (e.g.
  `Sample.Distance`'s `uint32,precision=1,nillable=4294967295`) - not
  re-extracted from a fresh capture yet, but the parsing rule needed to
  do so is now known exactly rather than guessed at.
- Reserved descriptor ids: `0` = Descriptor (a self-referential marker)
  and `255` = Escape - a *different* reserved-255 convention than
  `/Data`'s own SBEM chunk-level "no reading" sentinel found empirically
  for cadence (`docs/logbook-data-format.md`) - similar idea, different
  layer, worth not conflating the two.

**What's still proprietary, not in this public repo**: the actual
path-to-descriptor-id *resolution* wire protocol (this document's open
item 2, and `docs/logbook-data-format.md`'s handle-walk) - `libmds.so`
disassembly named it `whiteboard::protocol_v9` (a real, versioned,
general-purpose binary RPC serializer/deserializer with its own
`Serializer`/`Deserializer`/`ChunkSerializer`/
`UnknownStructureDeserializer` classes, used generically for *every*
Whiteboard resource, not just Logbook), but its implementation isn't in
`movesense-device-lib`'s public headers (only interfaces like
`ResourceTree`/`ResourceClient` are, and only for the *device*/firmware
side, not the phone-side remote-resolution logic). Confirms the
mechanism is real and generic (so whatever's learned about it from one
resource generalizes to others) without handing over how to replicate it.

## Provenance

Extracted by reassembling and CRC-verifying the raw Whiteboard frames from
the same Android HCI snoop log used throughout Phase 0a/6 (see
`~/.claude/plans/agile-hopping-harp.md`) - not from any external/public
source originally. Cross-referenced against `stacksjs/ts-watches`
(`packages/ts-watches/src/drivers/suunto.ts`, MIT) purely to confirm the
field *names* match Suunto's known public SML format; that project's parser
itself was not used and doesn't talk BLE. Later corroborated against real
primary sources (see "Confirmed against the real source" above): Suunto's
own public `movesense-device-lib` (Bitbucket, header-only, no explicit
license file found in the repo root at time of reading - treat as
reference/read-only, not something to vendor) and disassembly of the
official Android app's own `libmds.so` (reverse engineering of a locally
possessed APK for interoperability - not decompiled source, no code
copied, symbol names and structural observations only).
