# Suunto Sync

A native Sailfish OS app (C++/QML, Silica) that talks to a Suunto watch
over Bluetooth LE and to the Suunto cloud over HTTPS — without the official
Android app in the loop.

## What works

Confirmed on real hardware, not just in tests:

- **Workouts from the watch over BLE.** Lists the watch's logbook, fetches
  each entry, and decodes it: route, heart rate, altitude, cadence, laps,
  per-sample charts and the watch's own summary totals.
- **Two watch models, a Suunto Race and a Suunto 9 Baro.** SBEM chunk ids
  are per model — a Race carries GPS in chunk 0x0c and heart rate in 0x12,
  a 9 Baro in 0x0d and 0x15 — so the field table is read off whichever
  watch is connected (`/Logbook/byId/<id>/Descriptors`) rather than
  compiled in. See `docs/sbem-chunk-map.md`.
- **Two watches at once.** Both are remembered and switched from the
  pull-down menu or Settings, which matters because they decode
  differently: each one's field table is kept per address, so switching
  back does not mean fetching it again.
- **Workouts from the Suunto cloud**, with route, training metrics and
  sample data.
- **Uploading a watch-recorded workout to the cloud.** The watch's SBEM
  payload is converted to the JSON the cloud expects, zipped and posted. A
  workout recorded on the watch shows up in the official app afterwards.
  The workout list marks which watch-recorded workouts the cloud has
  taken, and which are still waiting.
- **GPS assist data to the watch**, which is what makes it find satellites
  in seconds. Settings has an "Update GPS data" button: it asks the watch
  which of four formats it wants, downloads that one and writes it across
  in 453-byte chunks. Confirmed on both watches: each one's ephemeris date
  moved to the current day. So the WiFi handshake the official app uses
  for a Race, and the credential it needs, are not required at all. The
  watch validates the data on its own time — seconds on a Race, minutes on
  a 9 Baro — so the date is polled rather than read once. **Needs no
  Suunto account**: the assist-data endpoints take no credential.
- **Phone notifications on the watch.** A text message arriving on the phone
  appears on the watch, with the app closed and nothing to press — and
  disappears from the watch when it is dismissed on the phone. Missed calls,
  voicemail, calendar alerts and messages all have their category mapped and
  tested, but a text message is the one that has been watched arriving. The
  payload is composed, not replayed: the encoder is derived
  from three captures and from the official Android app's own code, so the
  ANCS category, the notification id and the structure's lengths are all
  computed, and three captured requests are reproduced byte for byte in
  `tests/test_notificationcodec.cpp`.

  Observing other applications' notifications needs a D-Bus monitor
  connection, which the Sailjail proxy will not relay, so this half lives
  in **`suuntosync-notifyd`, a separate optional package** outside the
  sandbox — and the app itself stays sandboxed and Store-eligible. Only one
  process may hold a Whiteboard session at a time, so the two arbitrate with
  a file lock — a D-Bus name would have been tidier, and a sandboxed app may
  own only its own, with nothing under it. The daemon runs as a systemd user
  service that the package installs and starts.

  **Both watch models take one.** Their requests differ by exactly five
  constants, which are per-firmware metadata rather than protocol — the same
  kind of difference as the SBEM descriptor ids — and both sets were
  captured. A watch model nobody has captured is refused rather than sent a
  guess. See `docs/notifications.md`.

- **Sleep, recovery and daily activity.** Read from the cloud, and read
  *directly off the watch* — which matters, because a night that the watch
  has recorded but never uploaded is invisible to every other client.
  Watch-sourced entries can be pushed up to the cloud too. Daily activity
  is ten-minute buckets of steps, energy and heart rate: a day read off the
  watch summed to the same 1553 steps and 98 kcal the watch itself showed,
  and every field was cross-checked against the cloud's own entries for the
  same buckets.

## What doesn't, yet

- **The watch's media controls.** Both watches ask the *phone* for
  `/Media/Player/State`, `/Media/Player/Control` and `/Media/Track/Info`
  at the start of every connection. Nothing here answers them. It is the
  one direction of the protocol that has never been implemented, in either
  app.
- **The MediaTek EPO assist-data format**, which neither watch here asks
  for, and **laps on cloud-synced workouts**, which needs one capture of
  the response's real structure.

- **Weather to the watch** is not a missing feature: a Race fetches its
  own forecast over WiFi, and a 9 Baro never gets one at all. Nothing is
  pushed over BLE on either.

### Which watches

A Race and a 9 Baro are both supported over BLE, and they need different
field tables (see above). A **Suunto 7** is not: it is a Wear OS watch on
Google's companion channel, not the Movesense/Whiteboard family — the
official app does not even find it over that link. Its workouts reach the
Suunto cloud on their own, so the cloud sync here should cover it, which
is not the same as having been tried.

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
| `notifications.md` | the ANCS-over-Whiteboard payload, the encoder, and the daemon that feeds it |
| `sml-schema-descriptors.md` | the field schema the watch hands out about itself |
| `activity-types.md` | both activity-id vocabularies, the watch's and the cloud's |

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

Sailfish SDK, CMake. Beyond Qt5 and sailfishapp the app needs `zlib`, and
the daemon needs `dbus-1` as well — QtDBus cannot monitor another
application's traffic, so that half talks to libdbus directly.

The notification daemon is a second package out of the same tree, and its
SPEC file is in `daemon/` rather than `rpm/` because sfdk refuses to
build when it finds two of them there:

```
sfdk -c specfile=daemon/suuntosync-notifyd.spec build
```

GitHub Actions builds unsigned RPMs of **both packages** for 5.1.0.11 on
every push, aarch64 and armv7hl, and attaches all four to a release on a
`v*` tag. The daemon needs a different SPEC file than the one `mb2` would
pick, and the build action takes no option for that — so that step drives
the same container directly and swaps the SPEC inside its own throwaway
copy, which leaves the checkout alone and keeps the daemon's SPEC out of
`rpm/` where sfdk and Qt Creator need it not to be. They are CI
builds, not Store packages: `pkcon install-local` will say the package is
untrusted, and it is right.

The golden-vector tests need fixtures that are **not** in this repository —
they are real captures containing GPS tracks and sleep data. See
`tests/README.md`. CI therefore runs only the suites that need no fixture,
which is a minority of them; the rest run on the machine the captures live
on.

## Licence

MIT. Vendored: [heatshrink](https://github.com/atomicobject/heatshrink)
(ISC, unmodified, in `src/ble/heatshrink/`).

Not affiliated with or endorsed by Suunto or Sports Tracker.
