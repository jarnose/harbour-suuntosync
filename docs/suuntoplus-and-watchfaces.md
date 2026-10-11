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
