# WiFi, and how the watch is told to download offline maps (2026-10-18)

The question: a Suunto Race downloads offline maps over WiFi, not over BLE.
How is it *commanded* to?

**The short answer: the map selection is cloud state, and the watch is only
poked.** The phone writes three settings to the watch (a cloud base URL, a
token of the watch's own, a WiFi country code), puts the chosen regions in
the account's map library over HTTPS, and then sends one parameterless
write — `PUT /MapsDownload/AreaSelectionChanged` — which means "go and look
again". The watch brings up WiFi, fetches the library itself, downloads the
tile packages itself, and reports its own progress to the cloud. No map
data crosses the BLE link, which is why no capture here has ever shown any.

**Provenance.** Everything below comes from `com.stt.android.suunto` 6.7.12
(the same APK the rest of this project's reverse engineering uses), read
with `tools/dex_dump.py`: resource paths out of `SpartanBt`'s own methods, field
names out of the generated Moshi adapters' `JsonReader.Options.of(...)`,
the order of operations out of `WifiSettingsUpdater`. The two
`/Settings/Wifi/Cloud/*` writes and the `WatchUserKey` value are the
exception: those are **measured**, in the 2026-09-25 Race capture (see
`watch-push-resources.md`). Nothing else here has been on a wire yet.

## The watch's WiFi resources

All of them are `suunto://<serial>/<path>`, and all go through the same
Whiteboard transport as everything else in this project.

| resource | verb | payload | what it is |
|---|---|---|---|
| `/Device/Connectivity/Wifi/ScanAccesspoints` | GET, no parameters | → `WifiResult` | starts a scan; `resultData` is the **number found**, as a decimal string |
| `/Device/Connectivity/Wifi/Accesspoint` | GET with `{Index}` | → `{ssid, bssid, frequency, signalStrength, security}` | one scanned network, by index |
| `/Device/Connectivity/Wifi/Connect` | GET with `{Ssid, Key}` | → `WifiResult` | join a network now |
| `/Device/Connectivity/Wifi/Connection/Status` | subscribe (or GET) | → `{resultCode, resultData, resultReason}` | how the join went |
| `/Settings/Wifi/AccessPoints` | subscribe | → `{arrayData: [...]}` | the indices of the saved slots |
| `/Settings/Wifi/AccessPoints/<n>` | GET / PUT / **DELETE** | `{ssid, passphrase, security, encryption, hidden, index}` | one saved network |
| `/Settings/Wifi/Enabled` | PUT, boolean | | WiFi on or off, firmware **newer than 2.40.38** |
| `/Settings/Connectivity/Current/Wifi/Enabled` | PUT, boolean | | the same, 2.40.38 **and older** |
| `/Settings/Wifi/GeneralSettings` | PUT | `{CountryCode}` | the regulatory domain |
| `/Settings/Wifi/Cloud/OfflineMaps/Url` | PUT, string | | the cloud base URL — **measured**: `https://api.sports-tracker.com/apiserver/` |
| `/Settings/Wifi/Cloud/STTAuthorization` | PUT, string | | **measured**: `WatchUserKey <16 characters>` |
| `/MapsDownload/AreaSelectionChanged` | PUT, no parameters | | "the library changed, go and look" |
| `/MapsDownload/AreaUnderDownloadDeleted` | PUT | `{<areaId>}` | stop downloading that one |
| `/MapsDownload/NumberOfAreas` | GET | → a count | how many areas the watch holds |

Note the capitalisation, which is the watch's and not a typo of mine:
`Connect` and `Accesspoint` take **`Ssid`/`Key`/`Index`**, while a saved
access point's own fields are lowercase. And `Accesspoint` is spelled with
a small p where `AccessPoints` has a capital one.

`DELETE` is a verb this project has never sent. Everything implemented so
far is a register (`0x0a`), a handle fetch (`0x0d`) or a PUT (`0x0e`);
`MdsRx.delete()` is a fourth thing, and its frame type is unknown. Deleting
a saved network is the only thing here that needs it — a slot can also be
overwritten, which does not.

### Joining a network, as the app does it

1. `ScanAccesspoints`, then `Accesspoint` with `{Index}` for `0..n-1`.
2. Deduplicate by SSID, keeping the strongest `signalStrength`
   (`WifiProvider.filterNetworkList`).
3. `Connect` with `{Ssid, Key}`.
4. Subscribe to `Connection/Status` until `resultReason` is no longer
   `UNKNOWN`, or `resultCode` says it is up.
5. Only then persist it: read `/Settings/Wifi/AccessPoints`, pick the
   lowest index that is not in use (`WifiProvider.getStorageIndex`), and
   PUT the element into `/Settings/Wifi/AccessPoints/<index>`.

**The app does not store the network's real security type.** It writes
`security = WPA_WPA2, encryption = TKIP_AES` when there is a passphrase and
`OPEN`/`NONE` when there is not — nothing else. `hidden` is always false
and `index` is left out of the body.

```
AccessPointSecurity    OPEN 0  WEP 1  WPA 2  WPA2 3  WPA_WPA2 4
AccessPointEncryption  TKIP 0  AES 1  TKIP_AES 2  NONE 3
WifiResultReason       UNKNOWN 0  OTHER -1  TIMEOUT -2  WRONG_PWD -3
                       ACCESS_LIMIT -4  AUTH_NOT_VALID -5  DE_AUTH -6
                       INACTIVITY -7  AP_BUSY -8  POWERED_DOWN -100
```

The two security enums' numbers are the one soft spot in that list: the
constructor takes an ordinal *and* a value, and the compiler passed one
register for both, so value-equals-ordinal is a reading rather than a
reading-off. `WifiResultReason`'s values are explicit and negative, which
is what makes the pattern legible at all.

### What the app does on every connection

`WifiSettingsUpdater.onDeviceConnected`, in order, and only when the
watch's capability list contains **`hw_wifi`** (the Race answers
`hw_wifi_1`, which is in the list this project already reads — see
`suuntoplus-and-watchfaces.md`):

1. PUT the cloud base URL to `/Settings/Wifi/Cloud/OfflineMaps/Url`.
2. Derive a country code from the phone's last known place and PUT
   `{CountryCode}` to `/Settings/Wifi/GeneralSettings`.
3. Fetch a watch token and PUT `"WatchUserKey " + token` to
   `/Settings/Wifi/Cloud/STTAuthorization`.
4. Compare `/MapsDownload/NumberOfAreas` against the cloud's own count for
   this serial. If they disagree, **reset the cloud's library**
   (`POST maps/library/reset`), because a factory-reset watch has lost the
   areas the cloud still thinks it has. The watch is the truth here.

A fifth thing exists and is a special case worth knowing about rather than
copying: `enableInboxWifi` writes `/Settings/Wifi/Enabled` only for a
Suunto Vertical on firmware 2.24.40 or 2.24.42 ("inbox fw").

## The watch's own token, which was an open question

`watch-push-resources.md` has carried this since 2026-09-25: the captured
`/Settings/Wifi/Cloud/STTAuthorization` value is `WatchUserKey` plus 16
characters, the account's 32-character `userKey` is a different value, and
the HTTPS capture of the same period contains nothing that mints it.

**It is `GET userlogin`.** `WatchKeyRestApi.userLogin` →
`UserLoginResponse(watchKey=…)`, called through `WatchKeyRemoteApi`, which
takes a TOTP first (`GenerateOTPUseCase`; the log line on failure is
"Error generating totp to get watchKey") and sends it as the **`x-totp`**
header. That also explains the measurement that looked odd at the time —
the official app sending `x-totp` on 2 requests out of 122. This is one of
the two.

`SetWatchKeyForWatchUseCase` is what prefixes `"WatchUserKey "` to it
before the write. So the watch gets a credential of its own, scoped to the
watch, rather than the phone's session key — which is also why the phone's
own session key never appears in that write.

Everything needed for this is already implemented here:
`SuuntoAuth::generateTotp()` (RFC 6238, per-account, validated against
golden vectors during the login work) and the request signing. The app
reads `servertime` first to correct for clock skew; ours does not, and that
is a real difference to keep in mind if a TOTP is ever rejected.

## The cloud half

From this project's own extracted endpoint table
(`docs/app-api-surface.txt`), all on `OfflineRegionRestApi`:

| request | what it does |
|---|---|
| `GET maps/catalogue` | the available regions, near a latitude/longitude |
| `GET maps/search` | regions by name |
| `GET maps/library` | the account's chosen regions |
| `GET maps/library/{regionId}` | one of them |
| `PUT maps/library` | **place a download order** |
| `DELETE maps/library/{regionId}` | drop a region |
| `PUT maps/library/{regionId}/canceldelete` | undo that |
| `POST maps/library/reset` | `{deviceSerialNumber}` — the factory-reset case above |
| `GET maps/offlinearea` | the areas as a device sees them |
| `PUT maps/offlinearea/{regionId}/dlstatus` | progress: `{deviceSerialNumber, sourceTypeUsed, status, downloadedSize}` |

The download order's body is the interesting one:

```
PUT maps/library   {regionId, watchCapabilities, deviceSerialNumber}
```

**The watch's capability string goes to the cloud**, exactly as it does for
the SuuntoPlus store — so the cloud decides which tile packages suit this
model, and the same `Capabilities::join()` value this project already
sends serves here too.

A region, as the catalogue describes it:

```
RemoteOfflineRegion   id, name, description, area, bbox, centerPoint,
                      styles, sources, boundaryUrl, maskUrl,
                      adjacentMaskUrl, adjacentRegions, downloadOrders
RemoteOfflineAreaSource   type, baseUrl, paths, storageSize, transferSize
```

`sources[].baseUrl` + `paths` is what the watch actually fetches, and
`transferSize`/`storageSize` is what a progress bar on the phone would be
drawn from. The format of what is behind those paths is not known here, and
does not need to be: the watch downloads it.

## So what would it take to do this from Sailfish

Three of the four parts are things this project already has.

1. **Settings writes** — `putString()` on three paths, plus a boolean
   enable. All four are the existing two-step string write; nothing new.
2. **The watch token** — `GET userlogin` with an `x-totp` header, using the
   TOTP we already generate, then one `putString()`.
3. **The cloud half** — ordinary signed requests, the same shape as every
   other endpoint in `SuuntoCloudClient`.
4. **The scan-and-join flow** — this is the new work: a GET that carries
   parameters is already possible (`encodeParameterisedFetch`), but a
   *six-field structure* write like a saved access point is not something
   this project has sent. `/Dev/FileSystem/FileDelete` needed a setup walk
   describing its own structure before its one string parameter could be
   written (see `logbook-data-format.md`), and a six-field element will
   need the same kind of agreement. That walk cannot be guessed from the
   APK; it needs a capture.

A smaller, useful subset needs none of that: if the watch **already has a
WiFi network saved** (configured once from the Android app, or from the
watch's own menu where it can be typed in), then downloading maps is
nothing but the three settings writes, the cloud library calls and the
parameterless poke. That is worth building first — it is the whole feature
for a watch that is already on a network.

## The three reads, as the watch answered them (2026-10-19)

A Race with WiFi already on and a network saved from the Android app:

```
/Settings/Wifi/Enabled              f0 a4 9f 01 80 00  c8 00  01 00 01
/Settings/Wifi/AccessPoints         f0 a4 9b 01 80 00  c8 00  03 3c 12 24 6f 74
                                    02 00 00 00 08 00 00 00 00 00 00 00 01 00 00 00
/Device/Connectivity/Wifi/
        Connection/Status           f0 82 07 01 80 00  c8 00  00 82 22 24 91 3f 67 00
                                    05 10 00 73 00 10 00 00 00 00 00 00 00 00 00 00
                                    00 00 01 5f 18 fc 00 00 00 00 60 73 00 10
```

**Enabled is a scalar and reads 1**: six bytes of handle, 200, type
`0x0001`, value 1 - the same shape `/Settings/Ble/AncsEnabled` has
(`docs/notifications.md`), which is also the reason writing it as
`kParamSmallEnum` is a reasonable first attempt rather than a guess.

**The other two are structures, and they are not decoded.** Two things are
worth writing down about them anyway:

- `0x10007300` and `0x10007360` appear as 32-bit words in the status reply,
  and `0xfc185f01` beside them. Those look like the watch's own RAM
  addresses, which fits `resultData` being a `char*` in the official app's
  `WifiResult`: the structure carries the **pointer**, not the string. If
  that holds, no string inside a structure reply will ever arrive over this
  link, and reading one means asking for it some other way.
- The payload cannot be split by the structure rules alone. The header's
  alignment selector (`docs/logbook-data-format.md`) computes padding from
  the *address* the official implementation received the bytes at, not from
  an offset in the reply, so a reassembled buffer does not reproduce it.
  The metadata-driven walk is what reads these, and that is the same
  missing piece that blocks writing a six-field access point.

Neither blocks the feature: the enable write and the join take parameters,
not structures.

## Weather: the credential works, the trigger is unproven (2026-10-19)

The parts that are measured, and they are the valuable ones:

```
GET  userlogin  (x-totp)              -> watchUserKey, 16 characters
PUT  /Settings/Wifi/Cloud/OfflineMaps/Url      "https://api.sports-tracker.com/apiserver/"
PUT  /Settings/Wifi/Cloud/STTAuthorization     "WatchUserKey <16 characters>"
GET  /Weather/Sync + the 0x10 stream start
```

The watch's own weather view had been saying "update the weather from the
Suunto app" that morning; after this it showed a forecast. Nothing about
the forecast crossed the BLE link.

**That last sentence is the measurement; "after this" is not.** A later
pair of attempts moved nothing: `/Weather/Sync/LastTime` stayed at 11
October and `/Weather/Current` came back byte for byte identical. And
`/Weather/Sync` turned out to be a *subscribe* rather than a command - see
`docs/watch-push-resources.md`, which has the correction and the one test
that would settle what refreshed the forecast.

What does hold: the ephemeris, the weather and the maps all need the same
two writes, and the watch's log confirms it fetches with them
(`WifiDl eph succ`). The maps are confirmed end to end on top of that.

## The cloud's map endpoints, with their real parameter names

Retrofit keeps its paths and `@Query` names in dex annotations, which
`tools/dex_dump.py` does not read. A separate reader walked the
annotation structures directly (annotations_directory -> annotation_set ->
encoded_annotation, uleb128 indices into the string table) and got them
verbatim:

| request | parameters |
|---|---|
| `GET maps/catalogue` | `lat`, `lon`, `includeNearbyGroups`, `watchCapabilities` |
| `GET maps/search` | `query`, `scopes` |
| `GET maps/library` | `deviceSerialNumber`, `watchCapabilities` |
| `GET maps/library/{regionId}` | + the same two |
| `PUT maps/library` | JSON body, `Content-Type: application/json` |
| `DELETE maps/library/{regionId}` | `deviceSerialNumber`, `watchCapabilities` |
| `PUT maps/library/{regionId}/canceldelete` | `deviceSerialNumber`, `watchCapabilities` |
| `POST maps/library/reset` | body, `watchCapabilities` |
| `GET maps/offlinearea` | `serial`, `sourceType`, `hidePaths`, `incompleteOnly`, `includeBaseLayer` |
| `PUT maps/offlinearea/{regionId}/dlstatus` | `regionId`, body |

All of them answer the `AskoResponse` envelope this project already
unwraps. `lat`/`lon` are nullable, so leaving them out asks for the whole
catalogue rather than what is nearby - which is what a list to browse
wants, and this app has no position to offer anyway.

Implemented: catalogue, library, add, remove, and the watch nudge
(`PUT /MapsDownload/AreaSelectionChanged`, then
`GET /MapsDownload/NumberOfAreas` to see what the watch thinks it has).
Not implemented: search, cancel-delete, the reset-on-factory-reset
reconciliation, and progress reporting - none of which is needed to put a
map on a watch.

## What a capture would settle

One HTTPS capture with offline maps actually used, and one HCI capture
alongside it:

- the query parameter names on `maps/catalogue` / `maps/offlinearea`
  (retrofit annotations; the dex holds them but androguard does not hand
  them over easily, and the earlier flow file contains no `maps/` request
  at all — the feature was never opened during it);
- whether `GET userlogin` needs anything besides the signature and
  `x-totp`;
- the structure walk for a saved access point, and the `DELETE` frame type;
- what `/MapsDownload/NumberOfAreas` answers look like on the wire;
- whether the watch reports `dlstatus` itself or the phone relays it. The
  endpoint carries `deviceSerialNumber`, which suggests the watch, but the
  method is on the *phone's* API surface, so this is genuinely unresolved.

## The one thing that is not in doubt

The watch pulls its own maps. Nothing in the protocol pushes a tile to it,
there is no transfer to implement, and the phone's role is three settings,
a cloud list and a poke. For a project that has spent weeks on binary
payloads over BLE, this feature is mostly HTTPS.

### Maps work too, and the counts agree (2026-10-19)

A region added from the Sailfish app: `PUT maps/library` took it, the
credentials went to the watch, `PUT /MapsDownload/AreaSelectionChanged`
was accepted, and `GET /MapsDownload/NumberOfAreas` answered **5**.

The library listing at that moment: six regions, five `FINISHED` and the
new one `REQUESTED`. So **`NumberOfAreas` counts what the watch actually
holds, not what has been ordered** - the two numbers cross-check each
other, and neither was read from the other.

**The watch asks before it downloads.** A prompt appears on the watch's own
screen and the download starts only when it is accepted there; the progress
is shown on the watch too. So the phone cannot do this unattended, and the
page says so.

The status vocabulary, all three measured: `REQUESTED` when the region has
just been added, `IN_PROGRESS` once the watch is fetching it, `FINISHED`
when it has it. The first version guessed `DOWNLOADING` for the middle one
and the watch said otherwise - anything not in that list is now shown
verbatim rather than translated into a guess.

One thing the first version got wrong, worth keeping because the shape of
the mistake recurs: a catalogue group's `regions` are **references**
(`RemoteId`, an id and nothing else) and the full records live once in
`payload.regions`. Reading a group's entry as a region gives rows with
working ids and no names - which is exactly what it drew, while the
library view, whose objects are complete, looked fine.

And a session note: the first version held the BLE link across the two
cloud requests, measured at five seconds of blocked notifications per
refresh. The capability read is the only part that needs the watch, so the
link is let go before the HTTPS and taken again for the nudge.
