# SuuntoPlus apps, watch faces and guides

What the official app does to get a SuuntoPlus sports app, a watch face or a
guide onto the watch. The cloud half is **measured** from the mitmproxy
captures; the BLE half is **named but not yet captured** — the resource paths
and the app's own class and method names are read out of the APK, and no HCI
log of a real install exists yet. Keep that line in mind: everything in
"Over BLE" below is a map, not a transcript.

## They are all the same kind of thing

Three content types, one delivery format. Each is a plain zip with a
`manifest.jsn`, and the manifest's `type` says which it is:

| `type` | `usage` | id prefix | extra files | what it is |
|---|---|---|---|---|
| `feature` | `workout` | `zz…` | `main.js`, `t.xml` | SuuntoPlus sports app |
| `watchface` | `watchface` | `zw…` | `main.js`, many `*.pxb` | watch face |
| `sequence` | `workout` | `fp…` | `main.js`, `step0..N.js`, `icon.pxb` | guide / workout plan |

Measured from the 16 zips in the captures. A feature's manifest also carries
its data contract — `in` entries naming watch resources it subscribes to or
gets (`Activity/Move/-1/HeartRate/Current`, `Settings/User/MaxHR`,
`Fusion/Altitude`, …) and `out` entries naming the channels it writes. A
watch face zip is mostly art: `clface-q.pxb`, hour/minute/second hands, an
`-aod-` variant of each for always-on display, and a font atlas.

`.pxb` is Suunto's own bitmap container and is not decoded here.

## From the cloud

All on `api.sports-tracker.com/apiserver`, same `STTAuthorization` session key
as everything else, same ASKO `{error, payload, metadata}` envelope.

| request | gives |
|---|---|
| `GET v1/suuntoplus/features/items?onlyMyLibrary=true&variant=<V>&capabilities=<CSV>` | the account's library: id, name, description, `richText`, icon/banner URLs, `version`, `pluginId`, rating |
| `GET v1/suuntoplus/features/plugins/<id>` | the zip. **Watch faces come down this same endpoint**, not a separate one |
| `GET v1/suuntoplus/guides/items` | guides, with `activities`, `usage`, `pinned` |
| `GET v1/suuntoplus/guides/priority` | just the ids, in order |
| `GET v1/suuntoplus/guides/plugins/<id>` | a guide zip |
| `POST v1/suuntoplus/watchface/push/library?capabilities=…&variant=…&softwareVersion=…&serialNumber=…` | with an **empty body**: asks the cloud to seed the library with the factory watch faces for this watch |

The dex names the method behind that POST
`addPrefabricatedWatchfaceToLibrary`, and on Jarno's Race it answered

```json
"pushStatus": "CURRENT_VERSION_HIGHER_THAN_MAXIMUM_PUSH_VERSION"
```

i.e. the firmware is past the version this seeding applies to, so nothing was
pushed. Worth remembering before reading that status as an error.

The store endpoints (`store/sportsapps`, `store/category/watchface`,
`store/userWatchface/plugins/{id}`, `store/guides/files/{id}`, …) are the
browse-and-add side and are listed in `docs/app-api-surface.txt`. The
`suuntoplus/*` endpoints above are the sync side, and they are the ones that
matter for putting content on a watch.

### The capabilities string is the watch, described

Both the feature listing and the watch-face push carry a sorted
comma-separated capability list. It decides what the cloud offers, so it has
to come from the watch before either call can be made. Jarno's Race
(`variant=Tianjin`, firmware 2.53.42) reports among others:

```
feat_zappcount_100  feat_zappwf_v5  feat_zappwfcount_9  plugin_maxzappsize_2
plugin_settings_v1  feat_wbstorage_v1  feat_plugins_1  ui_type_ui2
ui_version_2  hw_display_amoledr466  hw_displaytype_amoled  hw_wifi_1
```

So: 100 zapps, 9 watch faces, watch-face format v5, and `feat_wbstorage_v1`
for Whiteboard storage. A second capability list in the captures reports
`feat_simvariant_Ibiza` with a much shorter feature set and no `ui_type_ui2`
— a different, older watch.

`SuuntoWatchCapabilityStore` and `WatchCapabilityUpdater` in the APK are
where this list is kept and refreshed, which is the hint for where it is read
from over BLE.

## Over BLE — named, not yet captured

The APK's string pool carries 115 `suunto://` resource paths, now in
`docs/mds-resource-paths.txt`. The ones for this job:

```
suunto://%s/WatchFace/Capabilities      suunto://%s/WatchFace/Info
suunto://%s/WatchFace/Lists             suunto://%s/WatchFace/Id
suunto://%s/WatchFace/Current           suunto://%s/WatchFace/Install
suunto://%s/WatchFace/InstallPath       suunto://%s/WatchFace/Delete/Id
suunto://%s/WatchFace/Delete/MultiId
suunto://%s/Settings/Zapp/Watchface/Id  suunto://%s/Settings/Zapp/Watchface/Default
suunto://%s/Zapp/%s/Lock                suunto://%s/Plugin/InstallAid
suunto://MDS/Plugin/
suunto://MDS/FileSystem/%s/File         suunto://MDS/FileSystem/%s/List
```

`WatchFaceMdsApi` is the class that calls them. Its data classes name the
shapes: `GetWatchFaceCapabilitiesResponse(capabilities=…)`,
`GetWatchFaceInstalledListContract(startAfterId=…)` — the same
start-after-id paging the logbook uses — `GetWatchFaceInstalledListResponse(lists=…)`,
`GetWatchFaceInfoResponse(watchFace=…)` over `MdsWatchFace(id=…)`,
`GetWatchFaceInstalledPathResponse(installPath=…)`,
`InstallWatchFaceContract(id=…)`, `WatchFaceDeleteContract(id=…)` and
`(ids=…)`, `SetAsCurrentWatchFaceContract(id=…)`,
`WatchFaceSimpleResponse(isSuccess=…)`.

`WatchFaceDeviceApi`'s suspend functions, recovered from their Kotlin
continuation classes, give the order the app does things in:
`getWatchFaceCapabilities`, `getInstalledWatchFaceList`,
`getWatchFaceInstallPath`, `startWatchFaceInstall`,
`startInternalWatchFaceInstall`, `setAsCurrentWatchFace`,
`subscribeCurrentWatchFaceId`, `uninstallWatchFace`.

For sports apps and guides the equivalent is
`com/suunto/connectivity/suuntoplusguide/MdsPluginInstallAid(location=…)`
with `MdsPluginInstallAidResponse(content=…)`, plus
`GetZappPluginDirectoryQuery`/`Response` and `ZappPluginDirectoryProvider`
— the watch is asked where plugins live, and answers with a location.

**The inference, stated as an inference:** ask the watch for an install path
or plugin directory, write the zip's bytes there through
`MDS/FileSystem/<serial>/File`, then PUT the id to `WatchFace/Install` (or
the plugin equivalent) to make the watch unpack and register it, and
`Settings/Zapp/Watchface/Id` to select it. Nothing here says the bytes go
through `FileSystem/File` rather than a chunked transfer of the kind
`Firmware/Transfer` uses, and nothing says what frame types carry it. That is
the one missing measurement.

## What it would take to finish this

1. **An HCI capture of a real install.** Add a watch face in the Android app
   with snooping on, the way `docs/logbook-data-format.md`'s findings were
   got. That single capture should settle the write mechanism, the framing
   and the order, because every name involved is already known — this is now
   a confirmation, not a search.
2. **`zappctl`** (`wylandplex/zappctl`) already installs SuuntoPlus apps over
   this protocol and documents the frame types this project independently
   confirmed (`0x0A` register, `0x0D` handle read, `0x0E` PUT). Read its
   install path before writing our own; it is the one external source that
   has done exactly this.
3. `.pxb` stays undecoded, which only matters for generating a watch face
   rather than installing one.

Nothing here is implemented in this app. It is a map of where to dig.

## The transfer, measured (2026-10-12)

The BLE half above was a map with one missing measurement: how the zip's
bytes travel. Two HCI captures from the rooted phone, taken while the
official app installed a watch face (`zwc05201`) and a sports app
(`zzhrzo01`), answer it. **Both use the same sequence**, which settles the
"one mechanism" reading:

```
0x0a  /Plugin/InstallAid                  ask where plugins live
0x0a  /Dev/FileSystem/DirectoryCreate     create /zapp
0x0e  → /zapp                             (the directory name, as a PUT parameter)
0x0a  /Dev/FileSystem/Stream              open a stream for /zapp/<file>.zip
0x0e  × N, 471-byte bodies                the zip, with a byte offset in each
0x0e  /Settings/Watchface/Current         watch faces only: make it current
0x0a  /Plugin/Install                     install
0x0a  /Dev/FileSystem/FileDelete          clean up
```

So it is `/Dev/FileSystem/Stream` with `0x0e` PUTs carrying an offset - the
same stream machinery the sleep timeline is *read* through, in the write
direction. **The inference recorded above, that the bytes go through
`MDS/FileSystem/<serial>/File`, is wrong.** That resource exists in the
app's string pool but is not what an install uses.

The zip travels uncompressed and in the clear: `PK\x03\x04` headers,
`clface-q.pxb`, `font-aod-q.pxb`, `main.js` and the app's own XML and
JavaScript are all readable in the capture.

### The filename is the pluginId, not the id

A watch face is written as its catalogue id - `/zapp/zwc05201.zip` - and a
sports app as its **`pluginId`**: store item `zzaero01` arrives as
`/zapp/zzaeroen.zip`, and `zzhrzo01` as `/zapp/zzhrzoen.zip`. That field is
right there in the `features/items` listing (`"pluginId": "zzbrnren"` for
`zzbrnr01`) and its purpose was not guessable from the APK.

### Adding to the library is a separate call

Picking something in the store sends `PUT /apiserver/v1/store/library` ->
`201`, and only then does the app fetch `features/plugins/<id>` and push it.
The first capture attempt caught the PUT but not the push, and the watch
face genuinely did not arrive - a useful accident, because it shows the two
halves are independent.

### Resource paths the APK's string pool does not have

Live traffic carries more than the 115 paths in
`docs/mds-resource-paths.txt`:

```
/Plugin/List          /Plugin/Install        /Meta/Hash
/Dev/Capabilities     /Dev/FileSystem/Stream /Dev/FileSystem/FileDelete
/Dev/FileSystem/DirectoryCreate             /Sync/BusyState
/Logbook/UnsynchronisedLogs                 /Settings/General/TimeZoneInfo
/Settings/Unit/UnitsMode                    /Settings/Navigation/RouteList
/Navigation/Poi/List  /Navigation/QuickNaviState
/Voice/AutoLap        /Voice/HeartRateZone   /Voice/Status
/Device/Power/Batterylevel                  /Update/Suggest
/Device/GNSS/ExtendedEphemerisData/Upload/0..2
/Weather/Sync         /System/Mode           /Info
```

Two of those answer standing questions: the ephemeris upload is
`/Device/GNSS/ExtendedEphemerisData/Upload/<n>`, and `/Update/Suggest`
reads back the firmware string `Tianjin-fw_2.53.42.31929-Q-c7ce43eb`.
`/Settings/Zapp/Watchface/Default` answers with `zwwtch01`.

### A frame pair this project had not named

Alongside `0x0d`/`0x05` (handle read and response) the capture is full of
`0x10`/`0x08`. This project knew `0x10` only as the bulk-stream start
trigger; it is the general "read this handle" request as well, and `0x08`
is its response. `/Weather/Sync` and most of the small settings reads above
use it.

### Weather is still not done

The app said it was syncing weather, and what the capture actually contains
is `0x0a /Weather/Sync` twice, each answered with a single byte `01`. That
is a flag, not a forecast. No weather payload is pushed anywhere in this
capture, so `docs/watch-push-resources.md`'s open item stands.

Worth recording as a method note: an ASCII grep across the traffic first
appeared to show a `/Weather/Current/ValidWeatherData` PUT. It was a
`$.subscribe('/Weather/Current/ValidWeatherData')` inside the watch face's
own JavaScript, travelling as file content. **Searching for resource paths
in a capture that carries a file transfer will find the file's strings** -
decode to frames and look at the request type before believing a hit.

## The stream write, byte for byte (2026-10-12)

Decoded from the watch-face install capture and verified by arithmetic
rather than by eye. A `/Dev/FileSystem/Stream` write is a `TYPE=0x0E` PUT
whose body is:

```
[handle 6 bytes][01 0d 00 02][total u24][end u24][len u16][len bytes of file]
```

| field | what it is | how it was verified |
|---|---|---|
| `total` | the file's whole length | constant across all 98 frames of one stream, and equal to 44294 - the exact byte count `GET features/plugins/zw31ju01` returned over HTTPS |
| `end` | bytes written *including* this chunk | 453, 906, 1359 … 43941, 44294; the last equals `total` |
| `len` | this chunk's payload | matched `body.size() - 18` in 131 of 131 frames, 453 for every chunk but the last |

So `end` is the position after the chunk, not before it: the first frame
carries 453 with `end == 453`. Concatenating the payloads in order gives a
file that starts `PK\x03\x04` and continues `font-aod-q.pxb`, which is the
zip the cloud served.

`total` and `end` are read as three bytes each because that is what the
observed frames show; a zapp is small enough (`plugin_maxzappsize_2`) that
nothing here would distinguish a 24-bit field from a 32-bit one with the
neighbouring `02` in it. The `01 0d 00 02` prefix is constant and is not
split with any confidence: by analogy with the parameterised handle fetch it
reads as one parameter of type `0x000d`, with the `02` belonging to the
value.

### The sequence, with both earlier doubts resolved

Tracing the same capture answered the two questions this section first
raised, and one of them was my own mistake.

**The PUTs do use the register's handle.** `0x0a /Dev/FileSystem/Stream` is
acked with `f0 23 0a 03 80 01` and that is exactly what the data PUTs
carry. The `f0 23 05 03 80 01` I reported was the ack of a different
request in the same neighbourhood. There is no unexplained walk in between.

**The id sequence restarts because the session is a different one.** The
install's requests are `03 80 01` where the rest of the traffic is
`01 80 00` - the GET verb byte is `0x03`, not `0x01`, and the third byte is
`01` rather than `00`. A separate session carries its own counter, which is
why ids run from 1 while the main session is at 2378.

So, per file, with ids exactly as captured every time:

```
0x0e  reqId=11   "b:/zapp"                     DirectoryCreate's parameter
0x0a  reqId=1    /Dev/FileSystem/Stream        -> ack carries the handle
0x0d  reqId=2..18                              schema introspection
0x0e  reqId=19   "b:/zapp/<name>.zip"          names the file being written
0x0e  reqId=20.. [total][end][len][data]       the file itself
```

Two details that could not be guessed: the watch's filesystem is addressed
with a **drive prefix**, `b:/zapp/...`, and a filename is passed as a
parameter of type `0x000c`, the NUL-terminated string type this project
already knew from the sleep timeline's reads.

The `0x0d` walk in the middle is introspection - `/Meta/Hash` answers
`3829AF73` and the reads that follow return field names, `FileName` and
`Data` among them. It is the same `protocol_v9` structure walk `/Entries`
does, and as there, it is probably skippable. Worth trying without it.

## zappctl, read properly (2026-10-12)

[wylandplex/zappctl](https://github.com/wylandplex/zappctl) installs
SuuntoPlus apps over BLE from Linux without Suunto's app, and it ports
SyncFix's decompiled `MainActivity.java`. It is 0BSD, so the licence is no
obstacle, but as with libdivecomputer nothing is copied here: it was read,
and every claim checked against this project's own capture.

**It confirms the stream write byte for byte.** `build_stream_frames` emits

```python
bytes([0xf0,0x23,0x0a,0x03,0x80,0x01, 0x01,0x0d,0x00,0x02])
  + le24(len(pkg)) + le24(end) + le16(length) + pkg[off:end]
```

- the same ten-byte prefix derived above from the capture,
- `le24`/`le24`/`le16`, which settles the "u24 or u32 with the 0x02 in it"
  question in favour of 24-bit fields,
- `CHUNK = 453`, "from SyncFix",
- request ids from 20, and the filename frame at 19 as
  `[handle][01][0c 00][path NUL]`.

Two independent derivations agreeing on every field is as close to certain
as this protocol gets.

### What it added

**The install commit, and a trap in it.** `post_frames.txt` is eight frames:
register `/Plugin/Install`, six `0x0d` walk steps, then a `0x0e` whose whole
body is `f0 <ref:2> 01 80 00`. zappctl's comment records that a recorded
commit refused with 400 on a 9 Peak Pro while *the upload had already
succeeded*, because the reference is device-assigned - `0x53,0x0e` on a
Vertical 2, `0x53,0x0d` on a 9 Peak Pro.

Read against the frame format, that "reference" is simply the middle two
bytes of the handle the registration hands back, and this project's capture
agrees: `/Plugin/Install` on the Race is acked with handle
`f0 53 0f 01 80 00`, so the Race's reference is `53 0f`. A client that
registers the path itself and uses the handle it is given needs neither
zappctl's 139 replayed setup frames nor its re-pinning pass. **The replay
and the re-pinning are the same problem**: captured frames carry another
watch's handles. Our own capture bears this out from the other side - its
install session is `03 80 01` where SyncFix's recording is `01 80 00`.

**A silent failure worth more than the rest.** The drive prefix is per
generation: NG3 wants `b:/zapp`, NG1 wants `/zapp`, and zappctl's comment
says the wrong one "writes the upload to a path the watch ignores - and the
install still answers 200". So a successful-looking install can leave
nothing on the watch. The Race took `b:/zapp` in the capture, which settles
it for this watch and for nothing else.

**MTU.** zappctl forces an MTU exchange because "BlueZ reports the default
ATT MTU (23) until an exchange happens", then writes `mtu - 3` at a time.
This project's client writes 20 bytes unconditionally, which for a 100 kB
watch face is some five thousand writes. Notifications from this watch
already arrive 60 bytes at a time (see docs/notifications.md), so the link
clearly carries more than 23 - worth doing before any upload is attempted
in earnest, not after.

### What remains unknown

The `0x0d` walks - 139 frames before the write, six before the commit - are
replayed by both SyncFix and zappctl, so neither explains them. They are the
`protocol_v9` structure introspection this project has seen at `/Entries`
and `/Summary`, and whether a client that already knows the handle can skip
them is untested. That is the one thing to try first, because skipping them
is the difference between a few frames and a hundred and forty.

## The install, and the walk that turns out not to be optional (2026-10-12)

The write sequence is implemented and every frame of it is accepted. It
still installs nothing, and the answer took four attempts, three of which
found real defects that were not the cause. Recording the order because the
order is what the next person needs.

### What the watch says, and what it does

An HCI capture of our own attempt, decoded beside the official app's
capture of the same install:

```
0x0a  /Dev/Capabilities        -> 200
0x0a  /Plugin/InstallAid       -> 200, answers "b:/zapp"
0x0a  /Dev/FileSystem/DirectoryCreate (03 80 01) -> 200
0x0e  "b:/zapp"                -> 200
0x0a  /Dev/FileSystem/Stream   (03 80 01) -> 200
0x0e  "b:/zapp/zzaeroen.zip"   -> 100
0x0e  × 39, 471 bytes each     -> 100 each, last one 200
0x0a  /Plugin/Install          -> 200
0x0e  commit, empty            -> 200
```

Nothing refuses anything. `/Plugin/List` is byte-identical before and
after. This is the failure zappctl warns about - "the install still answers
200" - reached without getting the path wrong.

### The difference, counted

Between the Stream registration and the first data frame the official app
sends **18 frames** that this sequence did not: a `/Meta/Hash` registration
and a sixteen-step `0x0d` walk. Its filename PUT is request id 19; ours was
the next frame after the register.

Everything else matches. Same register bodies, same 30-byte filename PUT,
same 471-byte data frames with the same header fields, same empty commit.
So by elimination, with every other variable checked against the capture:
**the introspection walk is required, and skipping it is accepted and
inert.** Both SyncFix and zappctl replay it, and neither explains it; this
is why.

### The three fixes that were not the cause

Each was a real defect, found by looking for this one, and each stands:

1. **`/Plugin/InstallAid` was not being called.** The captured install asks
   it before touching the filesystem, and its reply carries the directory,
   so asking removes the NG1/NG3 drive-prefix trap rather than hardcoding
   one side of it. The watch knows where plugins go; nothing else has to.
2. **A registration carries a verb, and the filesystem wants the write
   one.** Counted across all nine registrations in the capture:
   `/Dev/FileSystem/*` registers as `03 80 01`, everything else - including
   `/Plugin/InstallAid` and `/Plugin/Install` - as `01 80 00`. This client
   had read the middle bytes as a constant `0x80 0x00` since it was written,
   which was true of every registration it had ever made.
3. **The ATT MTU is 127, not 23.** Writes had been 20 bytes each on the
   assumption that BlueZ would not negotiate more. A 100 kB watch face is
   eight hundred writes instead of five thousand, and the watch's own
   60-byte notifications had been saying so for weeks.

### What to do next

Replay those 18 frames, from this project's own capture rather than
zappctl's recording - same watch, same firmware, and the handles will be the
ones this watch issues. They are `f0 00 0e`, `f0 00 03`, `f0 00 04`,
`f0 00 07`, `f0 00 05`, `f0 00 0c`, `f0 00 0b`, all in the `03 80 01`
session, and `/Meta/Hash` is what allocates the first. Request ids are ours
to choose; the official app numbers them 1-18 because the session is new.

There is precedent for replaying bytes nobody understands with the reason
written down: `Mds::literalSessionHandshakeRequest()`.

## The install works, and the oracle was wrong all along (2026-10-13)

Proven by deletion and reinstallation, which is the only test that could
separate our install from the official app's earlier push of the same
package:

```
07:32:54  list b:/zapp   zzaeroen.fea present
07:32:58  delete         zzaeroen.fea gone
07:33:10  install
07:33:32  list b:/zapp   zzaeroen.fea present again
```

So the zip is written, `/Plugin/Install` unpacks it and the watch registers
the result. A SuuntoPlus sports app can be installed on a Suunto Race from
Sailfish OS, with no Suunto app involved.

### What the directory says

```
b:/zapp: {"dirs":["storage"],
  "files":["storage.sbm","index.db","descript.sbm",
           "zwwtch01.wfa", ... 8 × .wfa ...,
           "zzbrnren.fea", ... 8 × .fea ...,
           "fp03pfbd.gui"],
  "more":0}
```

A watch face is installed as `<id>.wfa`, a sports app as
`<pluginId>.fea`, a guide as `<id>.gui`. **No zip survives**: the staged
file is consumed, which is also why the official sequence ends with a
`FileDelete`. `index.db` and `descript.sbm` beside them are the watch's own
registry.

### /Plugin/List is not the installed list

It named fifteen things while the directory held seventeen plugin files,
with `"more":0` saying the listing was complete, and it omitted exactly
`zzaeroen` and the guide `fp03pfbd` - through a reboot. The capture settles
that this is not our doing: the official app pushed `/zapp/zzaeroen.zip`
during the watch-face session and that name never appeared in its own
`/Plugin/List` read either.

So the plugin list is narrower than "what is installed" - the registry in
`index.db` can disagree with the filesystem - and **a catalogue page has to
read the directory, not the plugin list**, to say what is on the watch.

### The cost of the wrong oracle, written down plainly

Five rounds went into chasing an install that was working, because
`/Plugin/List` was chosen as the measure of success without checking what it
enumerates. Each round found a real defect and fixed it, and none of them
was the failure:

| fix | why it stands |
|---|---|
| `/Plugin/InstallAid` is called, and first | the watch names the directory, so the NG1/NG3 prefix never has to be guessed |
| the write verb | counted across all nine registrations: `/Dev/FileSystem/*` is `03 80 01`, everything else `01 80 00` |
| the MTU is 127 | measured; 124-byte writes instead of 20 |
| the setup walks | verified byte-for-byte, and `FileDelete`'s is a *different* walk from the stream's - eight steps against sixteen |

Two of my own conclusions had to be withdrawn on the way: that the install
was working and the measurement lied (said on one observation, and the
observation did not support it - a feature in the watch's menu can be
firmware), and before that, that the plugin list was merely a stale cache.

The lesson worth keeping is cheaper than any of it: **when the instrument
and the system disagree, the instrument is a suspect too** - and here it was
the only suspect never examined. The watch's own menu was free to look at
from the first round, and the directory listing that finally settled it took
an hour to write.

## Three kinds, and the listing that was missing (2026-10-13)

The page shows the library grouped under three headings - watch faces,
sports apps, guides - and the third one needed a second request. There is
no `sequence` in `features/items`, ever: a guide sitting on this watch as
`fp03pfbd.gui` was invisible on a page built from that listing alone.

| kind | listing | zip | installed as |
|---|---|---|---|
| watch face | `features/items` (`type: watchface`) | `features/plugins/<id>?capabilities=…` | `<id>.wfa` |
| sports app | `features/items` (`type: feature`) | `features/plugins/<id>?capabilities=…` | `<pluginId>.fea` |
| guide | `guides/items` | `guides/plugins/<id>?capabilities=…` | `<id>.gui` |

Two asymmetries in that table are measured, not assumed, and both are easy
to get backwards:

- **The guide *listing* takes no parameters at all.** The captured app
  requests `v1/suuntoplus/guides/items` bare, where every features request
  carries `variant` and `capabilities`. The guide *download* does carry
  `capabilities` - so the listing is the odd one out, not the download.
- **Only a sports app is named by `pluginId`.** A watch face is named by
  catalogue id, and a guide has no `pluginId` field in its listing at all,
  so a filename rule that fell through to `.fea` would report every guide
  as not installed - silently, and with the correct-looking explanation
  that the user simply has not installed it.

The last column is measured for all three kinds now. A guide staged as
`<id>.zip` really is renamed to `<id>.gui` by the watch, the way a watch
face's becomes `.wfa` and an app's `.fea`: this was an inference from the
mechanism when it was written - the watch picks the extension itself and no
install frame names it - and it was settled the same day by deleting the
guide from the watch and installing it again, which the page then reported
as on the watch. The library also went from fifteen items to sixteen, which
is the guide listing joining the features one correctly.

The grouping is a `SectionHeader` drawn by the delegate wherever the kind
changes from one row to the next, with the list sorted by kind and then by
name in `AppController`. `ListView.section` is no use here: the model is a
plain JavaScript array, which exposes no roles for `section.property` to
read. Nesting the `ListItem` inside a `Column` under the header costs
nothing - `ContextMenu` finds its flickable by walking the parent chain
(`Util.findFlickable`), and the `ListView.view` attached property that
`ViewItem` looks for is only used to set `__silica_contextmenu_instance`,
which `SilicaListView` does not even declare.

## aabbeell/suuntoplus-agentic-dev-env, read for protocol detail (2026-10-13)

A third independent project in this area, and worth being clear about what
it is: tooling for *writing* SuuntoPlus apps on a Mac, which drives the
official **SuuntoPlus Editor** VS Code extension. Its install path is
`vscode.commands.executeCommand('suuntoplus.deployApp', …)`, and the BLE
happens inside Suunto's own closed `SDSApplicationServer` (an Intel binary
on port 9801). So it adds **no wire-level detail** over zappctl - it never
speaks the Whiteboard protocol itself. Its tools talk to that server in
JSON over a WebSocket, in the same resource vocabulary our APK string dump
already had: `suunto://SDS/ConnectedDevices`,
`suunto://<serial>/Plugin/List`, `suunto://SDS/SystemEvents/<serial>`.

Four things in it are still useful to us:

- **Where a `pluginId` comes from.** The Editor's `suunto-plus.js getAppId`
  makes it from the first six letters of the manifest name - spaces, `-`,
  `.` and non-ASCII removed, lower-cased - plus a two-digit counter. That
  is exactly the shape of the ids we join the library on (`zzbrnr01`), and
  it means the id is a *name*, not a hash: two differently-named apps can
  collide on one id and replace each other on the watch.
- **"The next phone sync deletes every sideloaded app."** The official app
  reconciles the watch against the account library. Nothing we install is
  at risk, because we only ever install library items - but anything
  sideloaded is, and it explains why the watch's directory is not simply
  an append-only pile.
- **The platform's own limits**, from the watch's system log on a Race S:
  two SuuntoPlus apps per sport mode, and a shared pool of about sixteen
  simultaneous resource subscriptions per mode. Installing more than that
  is allowed; using them in one mode is not. Worth knowing before we offer
  a Store that makes installing easy.
- **`/Plugin/List` is what the official Editor reads too** - its
  `watch-apps.js` prints exactly that body. It is the same resource we
  found to under-report, so the directory stays our oracle; what this adds
  is that Suunto's own tooling has the same blind spot.

`suunto://MDS/Logs/<serial>/Syslogs` and `/Analytics/<serial>/Systemevents`
are already in `docs/mds-resource-paths.txt`, unexplored. The system log
those tools read is the same thing seen from the desktop side, so a watch
log is reachable over our BLE link too, whenever it is worth the trouble.

## The store (2026-10-13)

Measured from `suunto-watchface-1613.flows`, where the official app's own
browse-and-add is captured end to end.

| call | what it gives |
|---|---|
| `GET v1/store/home?variant=…&capabilities=…` | the whole catalogue: `banners`, `categories`, `items` |
| `GET v1/store/sportsapps/<id>?watchCapabilities=…` | one item's long `richText` and `rating` |
| `GET v1/store/sportsapps/user/feedback/<id>` | that item's rating as the user left it |
| `PUT v1/store/library` | adds to the library, 201 |

**One request is the whole store, pictures and all.** `store/home` answers fourteen
categories *and* `items`: 230 complete records, each with `title`,
`subTitle`, `description`, `iconUrl`, `type`, `libraryStatus`,
`activities`, `readMoreUrl` and its own `categories`. A category carries
only references (`types[].itemRefs[].id`), in the store's order, so
browsing costs nothing after the first fetch. **The per-item endpoint turns
out to be needed for nothing at all** and is not implemented: `items`
already carries `richText`, `rating`, `owner`, `version` and both banner
urls.

The images are the nicest part of this reply:

| field | sports app | watch face | guide |
|---|---|---|---|
| `iconUrl` | `feature-icons/<id>.png` | `watchface-widgets/<id>/preview/<variant>.png` | `guide-icons/<hash>.png` |
| `tileBannerUrl` | `feature-banners/<id>.png` | `watchface-widgets/<id>/banner/<variant>.png` | - |
| `tileBannerV2Url` | - | `…/banner/<variant>_v2.png` | - |

A watch face's icon is therefore **a render of that face on this watch** -
the last path segment is the `variant` the request sent, so the preview
already matches the model and nothing has to be built on our side. Both
hosts serve these with no authentication at all (checked: 200, `image/png`,
`cache-control: public, max-age=2592000`), so a QML `Image` loads the url as
it stands. Azure's blob host answers `application/octet-stream` for a PNG,
which does not matter - Qt sniffs the bytes.

`richText` is plain text with newlines, not markup, measured on both an app
and a watch face. 157 of 207 sports apps and 12 of 23 guides have one, so a
page has to read without it; `description` is the short line every item
carries. `tileBannerV2Url` appears on exactly 53 items, which is exactly the
watch-face count.

Confirmed on the phone: the watch-face thumbnails are the faces themselves,
the long descriptions render and scroll, and the half of the guides that
carry neither a banner nor a `richText` - *7 Minute Workout v2* and *7 Yoga
poses* in Training tools - show their icon and their short `description`
with no empty owner-and-rating line. A guide never has a banner, so the
item page falls back to the icon; the image block only disappears when
neither loads.

Because those CDNs ask to be cached for thirty days, the QML engine gets a
`QNetworkDiskCache` under `QStandardPaths::CacheLocation` - without one, the
engine's network stack has no cache and every visit to the store would
re-download a 74 KB render per watch face. That path is inside what Sailjail
grants: the app has been writing `map-tiles/` and other files there all
along.

Three things in it are traps:

- **A watch face is a `sportsapp` in the store.** The `watchface`
  *category* holds 53 refs and every one of them is typed `sportsapp`;
  `items` contains only `sportsapp` (207) and `guide` (23), never a
  `watchface`. The same item comes back from `suuntoplus/features/items` as
  `type: "watchface"`. So the store's `type` is a transport word, not a
  description, and the add must send it back verbatim: the captured app
  added the watch face *Race Flow* with
  `{"type":"sportsapp","id":"zwc05201","addToWatch":true}` and got a 201
  whose payload says `"type":"sportsapp"` and `pluginId: zwc05201`. Mapping
  it to `watchface` first is how an add would get refused. What the item
  really is only becomes knowable from the library listing afterwards -
  which is where our page gets its heading from, so nothing has to guess.
- **The parameter is renamed between the two reads**: `capabilities` on
  `store/home`, `watchCapabilities` on `store/sportsapps/<id>`.
- **`store/home` answered 403 three times and then 200** to a byte-identical
  request, within half a minute, during the capture. So a refusal from this
  endpoint is worth retrying before it is worth believing - and it is not
  evidence of a missing header. Nothing in the app's request changed in
  between: same session key, same user-agent, no `x-totp`.

### Confirmed on hardware the same day

A watch face added from the store's watch-face category went into the
library and onto the watch, in that order, with no manual step: the store
accepted it as a `sportsapp`, the library listing answered `watchface`, the
filename rule therefore chose `<id>.wfa`, and the watch shows it. So the
"carry the type verbatim" rule is measured, not reasoned - the one thing in
this chain that a plausible-looking mapping would have broken.

`DELETE store/library/sportsapps/<id>` works too, which settles the one
call here that was never captured - the path read out of the APK's endpoint
table was right.

Removing from the library is offered on both pages, and the two listings
disagree about what a guide is called (`guide` in the store, `sequence` in
the library), so `removeFromStoreLibrary()` accepts either word and picks
the path segment itself. Putting that mapping in the caller would mean two
callers, one of which would eventually delete nothing at all and report
success.

A library removal deliberately leaves the watch alone, so the library page
offers it only for an item the watch does not have. The three actions are
therefore never ambiguous: an installed item can only be removed from the
watch, and one that is not installed can be installed or dropped from the
library. Offering both at once would let a file stay on the watch while its
row vanished from a page that lists the library - the same blind spot the
watch's own extra files already have.

`addToWatch: true` is sent but is not what installs anything - the app
pushes the file over BLE itself afterwards, which is exactly the division
this app makes: the Store page adds to the library, and the SuuntoPlus page
installs from it.

### Not captured, taken from the APK's endpoint table

`DELETE store/library/sportsapps/<id>` and `.../guides/<id>`
(`SuuntoPlusStoreRestApi.removeFeatureFromLibrary` /
`removeGuideFromLibrary`) are implemented from
`docs/app-api-surface.txt` rather than from the wire. If the path is wrong
the item stays in the library and the next listing says so.

Also in that table and deliberately left alone for now: `store/search`,
`store/categories/<id>`, `store/guides`, `store/trainingplans/…`, and the
whole `store/userWatchface*` family - a *separate* API
(`OnlineWatchFaceRestAPI`, with `POST store/userWatchface/online`,
`PATCH …/online/<id>` for add-to-watch and `GET
store/userWatchface/plugins/<id>` for the file). That one is for the user's
own custom and album watch faces, not for the Suunto-published `zwc…` ones
the store's watch-face category lists.

## The directory holds more than the library does (2026-10-18)

The library page matched the account's library against the directory and
drew a row per library item. Anything in the directory with no library
entry was therefore invisible — and could not be removed from this app at
all, while still occupying one of the watch's plugin slots.

That is not a corner case. A development build is exactly this: the
suuntoplus-mahle project's `build-app.js` names a `"type":"device"` app
`<id>-q.dev`, zappctl pushes it as `b:/zapp/<id>.dev`, and no catalogue
anywhere has heard of it.

So the extras are rows now, and the extension is what says which kind:

| extension | kind | where it comes from |
|---|---|---|
| `.wfa` | watch face | the library, named by catalogue id |
| `.fea` | sports app | the library, named by **pluginId** |
| `.gui` | guide | the library, named by catalogue id |
| `.dev` | sports app | pushed by hand; a `"type":"device"` build |

Anything else in there — `storage.sbm`, `index.db`, `descript.sbm` — is the
watch's own registry and gets no row: a row offers deletion, and this app
should not offer to delete what it cannot name.

Such a row carries no catalogue id, so removal goes by file name
(`AppController::removeSuuntoPlusFile()`), guarded on the extension and on
the name still being in the last directory listing. Installing it again is
not offered, because there is nothing to install it *from*.

**A note in suuntoplus-mahle's `docs/watch-notes.md` says the opposite of
what this app does**: "BLE has no delete in the watch protocol: test
variants stay on the watch until removed over USB." That is true of
zappctl, not of the protocol. `/FileSystem/Delete` is measured working in
this very document — see the delete-and-reinstall trace above — so a test
build can be removed over BLE after all.

### Deleting the file is half an uninstall (2026-10-18)

Three hand-pushed `.dev` builds were removed from this page; their files
went, and the watch then showed **three rows in its own SuuntoPlus
selection with a button and no name**. The name lived in the file; the entry
lived in the registry, and deleting the file does not touch it.

`/Plugin/Uninstall` is what does, and it was in the APK's resource table all
along beside `/Plugin/Install`, `/Plugin/InstallAid` and `/Plugin/List`. Its
parameter is a single string: `MdsUninstallPluginRequest(id=…)`, which makes
it the same two-step string write as `/Dev/FileSystem/FileDelete` — register,
then PUT the id. Taken from the APK, not from the wire: no capture here
contains an uninstall, so the first real one is the measurement.

Removal therefore does both now, uninstall first because it may take the
file with it, and only a *pair* of failures is reported. And because the
registry can outlive the file, the page reads all three answers:

| source | what it answers | what the page does with it |
|---|---|---|
| the account's library | what you own | rows, with install / remove |
| the plugin directory | what files exist | says "on the watch"; extras get their own rows |
| `/Plugin/List` | what the watch's menu draws | an entry with no file gets a row whose only action is the uninstall |

The third was previously read once, during the install work, and then
dropped as "the wrong oracle" — which it is, for *is this installed*. It is
the right one for *what will the watch show*, and nothing else can see a
leftover.

### And the registry listing is incomplete too (2026-10-18)

The leftover rows the section above added did not appear: after three
`.dev` files were removed, the watch drew three nameless rows in its own
SuuntoPlus menu and `/Plugin/List` named **none** of them.

So the third oracle has the same hole as the first. `/Plugin/List` already
named fifteen things where the directory held seventeen files; now it also
fails to name an entry whose file is gone. It is not "the watch's menu" —
it is its own partial view, and all three readable answers can miss what
the watch is actually drawing.

What is left to try, in order of cost:

1. **Read `b:/zapp/index.db` as a file.** It sits in the same directory as
   the plugins, reading a file off the watch is already implemented
   (`/Dev/FileSystem/Stream`, see `watch-push-resources.md`), and it is
   the registry itself rather than a view of it. If the three leftovers are
   in there, the names to uninstall are in there too.
2. `/Plugin/Uninstall` with the id the *file* had, blind. The ids are known
   — they were the file stems — so this costs one write each and either
   clears the rows or does not. It is cheap enough to be worth trying
   before any decoding, but it answers nothing if it fails: no reply
   distinguishes "wrong id" from "wrong resource".
3. A capture of the official app removing a sideloaded plugin, if the
   official app can even see one.

### The registry is a file, and it is trivial (2026-10-19)

`b:/zapp/index.db`, read off the watch with the ordinary file read. 408
bytes, twenty entries, every byte accounted for:

```
entry := id(8 bytes, ASCII) value(LE32)* 0xFFFFFFFF
```

The first value is the kind, and it agrees with the extensions in the
directory listing: **1** sports app (`.fea`), **2** guide (`.gui`), **4**
hand-pushed development build (`.dev`), **8** watch face (`.wfa`). The
second was 0 for all twenty. Two entries carried a third — 54 for a sports
app, 3 for the guide — and what it means is not known; the parser keeps it
rather than discarding it.

The file named all four `mahle*` builds, including the three whose files
were deleted. So this is the answer to the nameless rows: the registry is
what the watch's menu is drawn from, the file is readable, and
`/Plugin/List` was simply the wrong question twice over.

The page reads this instead of `/Plugin/List` now, and a registry entry
with no file of its own gets a row whose only action is
`/Plugin/Uninstall`. `src/ble/pluginregistry.h` carries the format;
`tests/test_pluginregistry.cpp` covers it with vectors built from the real
file's shape, including a truncated tail and a padded id.

## Making a guide, not just installing one (2026-10-19, not implemented)

Everything above is about guides somebody else wrote. **Creating one is four
endpoints and one JSON document**, and this section is the schema, written
down so the work can start without the research.

Source: [`Marius-Ar/suunto-api-wrapper`](https://github.com/Marius-Ar/suunto-api-wrapper)
(ISC), a TypeScript client for this same cloud API, read from its type
declarations - not measured here. Two things in it this project *can*
vouch for, which is why the rest is worth trusting: its `guides/items`
listing is the endpoint this app already uses, and its `generateXtotp()`
derives the `x-totp` TOTP exactly as `SuuntoAuth::generateTotp()` does -
same obfuscated master secret, PBKDF2 with 100 iterations, 32-byte key,
30-second period, six digits, keyed by the account email. An independent
implementation agreeing on that is the best confirmation available without
another capture.

### The endpoints

```
GET    v1/suuntoplus/guides/items          the account's guides, metadata only
GET    v1/suuntoplus/guides/files/<id>     a zip: guide.json + icon.png
POST   v1/suuntoplus/guides/files          create; Content-Type: application/zip
PUT    v1/suuntoplus/guides/files/<id>     replace
DELETE v1/suuntoplus/guides/files/<id>     delete (the path already in
                                            docs/app-api-surface.txt)
```

The zip holds exactly two entries, `guide.json` and `icon.png`, and the
icon must be a PNG of exactly **300x300**. The wrapper ships a transparent
default for callers with no artwork, which says the field is required
rather than optional.

The answer is the usual envelope, with the created guide's own record in
`payload`: `id`, `username`, `type`, `name`, `description`,
`shortDescription`, `owner`, `url`, `iconUrl`, `richText`, `activities`,
`localDate` (YYYY-MM-DD), `usage`, `pinned`, `externalId`, `ownerId`,
`modificationTime`, `fileModificationTime`.

### `guide.json`

```
{ type: "sequence", name, description, shortDescription, owner,
  activities: [<cloud activity id>, ...], usage: "workout",
  url?, localDate?, externalId?,
  steps: [ ... ],
  extensions?: { "com.suunto": { workoutPlanType?: "interval" | "race",
                                   raceParameters?: { totalDistanceMeters,
                                                       totalDurationSeconds?,
                                                       unitSystem } } } }
```

Three kinds of step, keyed by `type`:

| step | fields |
|---|---|
| `fields` | the thing the athlete performs: `trigger`, `fields`, `title`, `phase`, `lap`, `alerts`, `notification`, `transitions`, `options`, `createManualLap`, `id`, `extensions` |
| `notification` | a passive banner: `title`, `fields`, no trigger |
| `repeat` | `times` and a nested `steps` array - so "5 x (1 km hard + 60 s easy)" nests naturally |

`phase` is one of `warmUp`, `interval`, `rest`, `coolDown`, `split`,
`finished`, and it is repeated inside
`extensions["com.suunto"] = { phase, isCustomNameSet, uuid }` - an
upper-case UUID per step.

**Triggers and conditions are one union**, nestable:

```
and{triggers[]}   or{triggers[]}
distance{value}        duration{value}        (since the workout started)
stepDistance{value}    stepDuration{value}    (since this step started)
location{latitude, longitude, distance?}      manualLap{}
```

Metres and seconds throughout - no scaling, unlike the watch's own binary
formats.

**Fields** are what the watch draws, and there are five families:

- a live readout, `type` naming the metric: `altitude`, `ascent`,
  `ascentTime`, `cadence`, `descent`, `descentTime`, `distance`,
  `duration`, `energy`, `heartRate`, `pace`, `power`, `speed`,
  `strokeRate`, `strokes`, `swolf`, `temperature`, `verticalSpeed`, with
  optional `window` (`workout`/`step`/`manualLap`), `aggregate`
  (`average`/`max`/`min`), `windowIndex`, `comparison`;
- a target range: `targetHeartRate`, `targetPace`, `targetPower`,
  `targetSpeed`, `targetCadence`, each with `min`, `max`, `value`;
- a target taken from the user's own zones:
  `targetFromHeartRateZone`, `targetFromPaceZone`, `targetFromPowerZone`,
  with `zone`;
- a zone selector: `targetHeartRateZone`, `targetPaceZone`,
  `targetPowerZone`;
- a countdown, `stepDistanceCountdown` or `stepDurationCountdown`, and
  plain `text`.

Plus the small supporting shapes: `alerts: [{ type: "default", condition,
countdown: { type: "standard" } }]`, `lap: { type: "manual", hidden }`,
`notification: { type: "default", title, text }`,
`transitions: [{ condition, stepId }]`,
`options: { repeatNumberPreferred }`.

### The rules that are not in the schema

Worth more than the type names, because they are what a first attempt gets
wrong:

- **An alert's condition may only be `stepDuration` or `stepDistance`.**
  The wrapper mirrors a step's trigger into an alert by scanning it
  recursively for the first such condition; a trigger made only of
  `location` produces no alert at all.
- **A manual lap is the escape hatch**, and it is added by default:
  `trigger: or(stepDuration|stepDistance, manualLap())`. Without it a step
  whose condition never fires traps the athlete in it.
- A countdown field is appended to match whichever of `.duration` or
  `.distance` the step ends on.
- The backend rejects a document missing the required properties, which is
  why the wrapper's builder throws before sending rather than after.

### What this would be worth here

The app can already install a guide onto the watch. With the above it could
**compose** one - an interval session built on the phone, uploaded, and
installed over BLE in the same breath - which is the one thing in the
SuuntoPlus area the official app does and this does not. The pieces that
exist: the guides listing, the zip install path, `/Plugin/Install`, and the
Secrets-backed session. The pieces missing: this JSON writer, a 300x300
PNG to put in the zip, and a UI to describe a workout.
