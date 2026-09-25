# Suunto Sync

A native Sailfish OS app (C++/QML, Silica) that talks to a Suunto watch
over Bluetooth LE and to the Suunto cloud over HTTPS — without the official
Android app in the loop.

## What works

Confirmed on real hardware, not just in tests:

- **Workouts from the watch over BLE.** Lists the watch's logbook, fetches
  each entry, and decodes it: route, heart rate, altitude, cadence, laps,
  per-sample charts and the watch's own summary totals.
- **Workouts from the Suunto cloud**, with route, training metrics and
  sample data.
- **Uploading a watch-recorded workout to the cloud.** The watch's SBEM
  payload is converted to the JSON the cloud expects, zipped and posted. A
  workout recorded on the watch shows up in the official app afterwards.
  The workout list marks which watch-recorded workouts the cloud has
  taken, and which are still waiting.
- **Sleep, recovery and daily activity.** Read from the cloud, and read
  *directly off the watch* — which matters, because a night that the watch
  has recorded but never uploaded is invisible to every other client.
  Watch-sourced entries can be pushed up to the cloud too. Daily activity
  is ten-minute buckets of steps, energy and heart rate: a day read off the
  watch summed to the same 1553 steps and 98 kcal the watch itself showed,
  and every field was cross-checked against the cloud's own entries for the
  same buckets.

## What doesn't, yet

- **Notifications to the watch.** The mechanism is understood (see
  `docs/notifications.md`) but it requires `Sandboxing=Disabled`, which
  costs Jolla Store eligibility — a decision, not a missing feature.
- **Weather and GPS-ephemeris updates to the watch.** Both are mapped;
  neither is implemented. They need the `0x0e` PUT verb, which this
  project does not encode yet.
- **Suunto 9 Baro** — nearly. It pairs, connects, lists its logbook and
  transfers a workout with every protocol layer working unchanged, but
  every field decoded to zero: SBEM chunk ids turn out to be per watch
  model (a Race puts GPS in chunk 0x0c and heart rate in 0x12, a 9 Baro
  in 0x0d and 0x15), and the table compiled in here was a Race's. The fix
  is in — the field table is now read off whichever watch is connected,
  from `/Logbook/byId/<id>/Descriptors` — and on the device it decodes
  that watch's workouts correctly. What is not yet confirmed is the
  repair of the three workouts that were already stored, decoded wrongly
  and then dropped off the watch's own list for good; see
  `docs/sbem-chunk-map.md` and `logbook-data-format.md`.

## How it was built

Nothing here came from a published specification, because there isn't one.
The protocol was reconstructed from Bluetooth HCI snoop logs, HTTPS
captures of the official app, and decompiling `libmds.so` and the app's own
dex. `docs/` carries the results:

| document | what it covers |
|---|---|
| `logbook-data-format.md` | the BLE Whiteboard protocol, SBEM containers, the `/Data`, `/Entries` and `/Summary` transports |
| `sbem-chunk-map.md` | the watch's own descriptor table, read out of the device — and why it is per watch model |
| `workout-upload.md` | the cloud's multipart upload, and the health API |
| `watch-push-resources.md` | sleep and activity timeline files, GPS ephemeris, weather |
| `notifications.md` | why a sandboxed Sailfish app cannot observe notifications |

Two habits run through the whole thing and are worth stating, because they
caught real bugs:

**Decoders are Qt-free and tested against golden vectors.** Anything that
parses bytes lives in plain C++ with STL only, so it compiles and runs with
`g++` on a desktop. Expected values come from an independent source
wherever possible — the Suunto cloud's own figures for the same workout,
Python's `datetime` for epoch conversions, a format's published example —
rather than from this code's own output.

**Captured bytes beat inference.** Several times a plausible reading of the
protocol turned out to be wrong and only a real capture settled it: the
sleep resource path that does not exist on the wire, a parameter prefix
that looked like a length and was a type code, an upload that failed four
times for four unrelated reasons, and a daily-activity resource written off
as "a different structure" when the reply had simply been empty. The commit
messages record which guesses were wrong, deliberately.

**What is confirmed on hardware is said so, and what is not is not.** Two
watches exist here and a third does not, so "identical on both" is written
down as a measurement rather than promoted to a property of the format.

## Building

Sailfish SDK, CMake. `zlib` is the only dependency beyond Qt5 and
sailfishapp.

The golden-vector tests need fixtures that are **not** in this repository —
they are real captures containing GPS tracks and sleep data. See
`tests/README.md`.

## Licence

MIT. Vendored: [heatshrink](https://github.com/atomicobject/heatshrink)
(ISC, unmodified, in `src/ble/heatshrink/`).

Not affiliated with or endorsed by Suunto or Sports Tracker.
