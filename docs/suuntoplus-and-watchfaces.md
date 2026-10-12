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
