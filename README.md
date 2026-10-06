# Suunto Sync

A native Sailfish OS app (C++/QML, Silica) that talks to a Suunto watch
over Bluetooth LE and to the Suunto cloud over HTTPS — without the official
Android app in the loop.

## What works

Confirmed on real hardware, not just in tests:

- **Fitness, fatigue and form.** CTL, ATL and TSB — what Suunto's app calls
  Progress — on the main page, from the cloud's own per-workout stress
  scores through the standard 42- and 7-day exponential averages. Checked
  against the official app on the same account: 15, 5 and +9 both ways, and
  again as 17, 12 and +4 a few workouts later. The second check is the
  stronger one — the figures tracked a real change rather than matching
  once, which is what says the 42- and 7-day time constants are right and
  not just the level.
  Nothing here invents a training load from duration or heart rate; a figure
  that looked like Suunto's and was not would be worse than none.

  Computing them here rather than asking for them is not a shortcut: a
  capture of the official app with every screen opened shows it fetching no
  fitness, fatigue or form figure at all. The cloud sends the home screen's
  widget *layout* and no data for it, so the numbers are the device's own
  arithmetic over the per-workout stress scores — in both apps.
- **This month at a glance.** The main page leads with how long and how
  often, the way Suunto's own app does. Deduplicated: the same outing can be
  in the database twice, once from the watch and once from the cloud, and a
  monthly total that reads double is worse than none — so starts within
  ninety seconds of each other count as one workout, and the rule has its
  own Qt-free test.
- **Personal records**, per activity, all-time and this year, from an
  endpoint the official app uses and a capture found. Every unit on that
  page is measured rather than assumed: the durations and distances by
  arithmetic that works out only one way, and the three speed records by
  joining each one to the workout it was set in and comparing against that
  workout's own distance over its time — which is how `FastestPace`
  turned out to be a speed in metres per second despite its name. The one
  type left showing a bare number is average power, because there is no
  power meter here to check it against.
- **A base map under the GPS track**, off by default, from OpenFreeMap.
  Sailfish has no QtLocation QML module, so there is no `Map` element and no
  geoservices plugin: the tiles are fetched, decoded and painted here. The
  source matters as much as the code — OpenFreeMap needs no API key, sets
  no request limit and permits commercial use, which is what lets it be the
  default in a public repository where every raster provider's key could not
  be. Its tiles are vector, so this project has a Mapbox Vector Tile decoder:
  Qt-free, 46 assertions, golden-vector tested against a real tile whose
  expected contents were read out by an independent script first. Settings
  takes any TileJSON address or `{z}/{x}/{y}` template; the attribution its
  terms require is drawn on the map. Tapping the track hands the area to Pure
  Maps, which is the one map application here with a registered URI handler -
  it takes a single coordinate, so it shows the place and not the outing. See
  `docs/base-map.md`.
- **A cover that shows one thing, chosen.** The latest workout, lifetime
  totals, fitness/fatigue/form, last night's sleep, or nothing — because
  there is no one right answer: somebody training for a distance wants the
  totals, somebody tracking recovery wants last night, and somebody who
  only uses the app to sync wants neither.
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
  differently: each one's field table is kept per address, so switching back
  does not mean fetching it again. The daemon follows the switch, and
  Settings can forget a watch outright.
- **Workouts from the Suunto cloud**, with route, training metrics, sample
  data and laps — automatic and manual ones, which the cloud keeps as two
  parallel partitions of the same workout rather than one sequence.
  A cloud workout also gets the analysis the official app shows for it:
  VO2max, fitness age, EPOC, peak training effect, recovery time, and the
  heart-rate and power zones with time spent in each. The zone boundaries come
  from the server rather than being derived from a configured maximum —
  0/131/146/161/176 against a maximum of 192 is not an even division of
  anything, so computing them would have been wrong. It is a second
  endpoint, found by capture: the official app POSTs the list of extension
  types it wants to `/v1/workout/extensions/{key}`.
  Incremental: a routine sync asks only for what has reached the cloud
  since the last one, carrying the server's own cursor rather than this
  phone's clock. What the cloud's `since` compares against was measured
  rather than guessed, in two steps — a capture of the official app
  showed it is a server-side time and not the recording time, and then
  editing a three-month-old workout and syncing showed which one: the
  last-modified time. So an ordinary sync brings back edits as well as new
  workouts, and the full re-fetch in Settings is for a local copy that is
  damaged rather than merely stale.
- **Uploading a watch-recorded workout to the cloud.** The watch's SBEM
  payload is converted to the JSON the cloud expects, zipped and posted. A
  workout recorded on the watch shows up in the official app afterwards.
  The workout list marks which watch-recorded workouts the cloud has
  taken, and which are still waiting. One the cloud has not taken can be
  deleted from the workout's own pull-down menu — behind a remorse timer,
  because the watch prunes its own logbook and the phone's copy can be the
  only one left.
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
  voicemail, email, calendar alerts and messages all have their ANCS category
  mapped and tested; a text message is the one that has been watched
  arriving, on both watch models.

  **A ringing call, too, and it needed a second source.** An incoming call
  never reaches the notification server on Sailfish — a capture of one
  shows ten seconds of ringing with no notification in it, and the first one
  arriving after the call was already missed. So the daemon also watches
  `org.nemomobile.voicecall`, which is where the ringing actually happens, and
  clears the ring when the call is answered, rejected or missed. The missed
  call then arrives as its own notification, which is the right division: the
  ring is transient and the miss is a record.

  The payload is composed, not replayed: the encoder is derived from four
  captures and from the official Android app's own code, so the category,
  the notification id and the structure's lengths are all computed — and all
  four captured requests are reproduced byte for byte in
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

- **The watch's own settings, read and written.** Settings has a switch for
  `/Settings/Ble/AncsEnabled`, which is the watch-side gate a notification
  PUT is refused by, and the daemon binary doubles as a command-line probe:

  ```
  suuntosync-notifyd --read /Settings/Ble/AncsEnabled
  suuntosync-notifyd --write-enum /Settings/Ble/AncsEnabled 1
  ```

  It exists because questions about the watch otherwise need somebody to tap
  something, and three rounds of that was two too many.

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
  for.
- **Weather to the watch** is not a missing feature: a Race fetches its
  own forecast over WiFi, and a 9 Baro never gets one at all. Nothing is
  pushed over BLE on either.
- **Raster map tiles.** The base map draws vector tiles; an address that
  serves images is detected and says so rather than drawing a blank. It is
  the easier of the two renderers, and what it would buy is a source like
  Finland's national survey maps, which are raster and need a free key.
- **Map labels.** No place names are drawn under the track. The data is
  there — the `place` layer carries names as attributes, so no glyph
  fonts would be needed — but a name every few hundred metres may be
  clutter rather than information, and that is a judgement to make while
  looking at one.

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
| `base-map.md` | why a base map needs a vector-tile decoder on Sailfish, and the one tile source that can be a default |

Three habits run through the whole thing and are worth stating, because
they caught real bugs:

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
as "a different structure" when the reply had simply been empty.

The clearest case is the most recent. A 9 Baro refused a notification with
status 400, and three plausible explanations — Do Not Disturb, the watch's
ANCS setting, and an older "legacy" notification path that genuinely exists
in the firmware library — were all eliminated by measuring rather than
reasoning. Twenty minutes of capture then showed the real answer: the layout
is identical and five constants differ. The commit messages record which
guesses were wrong, deliberately.

**What is confirmed on hardware is said so, and what is not is not.** Two
watches exist here, so "identical on both" is written down as a measurement
of those two rather than promoted to a property of the format.

## Building

Sailfish SDK, CMake. Beyond Qt5 and sailfishapp the app needs `zlib`, and
the daemon needs `dbus-1` as well — QtDBus cannot monitor another
application's traffic, so that half talks to libdbus directly.

The notification daemon is a second package out of the same tree, and its
SPEC file is in `daemon/` rather than `rpm/` because sfdk refuses to
build when it finds two of them there:

```
sfdk -c target=SailfishOS-5.1.0.11-aarch64 build
sfdk -c target=SailfishOS-5.1.0.11-aarch64 -c specfile=daemon/suuntosync-notifyd.spec build
```

`sfdk` lives in `~/SailfishOS/bin/` and is not on `PATH`, and it refuses to
build without being told the target — the first line is the application,
the second the daemon.

GitHub Actions builds unsigned RPMs of **both packages** for 5.1.0.11 on
every push, aarch64 and armv7hl, and attaches all four to a release on a
`v*` tag. The daemon needs a different SPEC file than the one `mb2` would
pick, and the build action takes no option for that — so that step drives
the same container directly and swaps the SPEC inside its own throwaway
copy, which leaves the checkout alone and keeps the daemon's SPEC out of
`rpm/` where sfdk and Qt Creator need it not to be. They are CI
builds, not Store packages: `pkcon install-local` will say the package is
untrusted, and it is right.

The daemon keeps its own log at
`~/.cache/io.github.jarnose/suuntosync/suuntosync-notifyd.log`. That is not
a preference: journald on this device runs with `Storage=volatile` and
enough log traffic that its window is seconds wide, so there is nowhere else
for a background process to leave a trail.

Most golden-vector tests need fixtures that are **not** in this repository —
they are real captures containing GPS tracks and sleep data. See
`tests/README.md`. CI runs the ten suites that need none; the rest run on the
machine the captures live on. The newer ones are written to be in the first
group where they can be: the vector-tile suite builds a tile byte by byte and
inlines it as hex, so its exact checks run everywhere and only its feature
counts need the real tile.

## Licence

MIT. Vendored: [heatshrink](https://github.com/atomicobject/heatshrink)
(ISC, unmodified, in `src/ble/heatshrink/`).

Not affiliated with or endorsed by Suunto or Sports Tracker.
