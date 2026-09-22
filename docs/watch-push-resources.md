# Pushing data *to* the watch: health, GPS and weather

Scouting notes for items 3-6 of the 2026-09-22 list. Nothing here is
implemented; this is what is known and, more usefully, what is still
missing and how to get it.

## Item 3 - reading health data from the watch over BLE

Two MDS resources exist, from the APK's own string table:

```
suunto://MDS/Sleep/%s/Entries
suunto://MDS/Activity/%s/Entries
```

`%s` is the watch serial, the same substitution `/Logbook/%s/Entries` uses.
So the addressing is familiar; what isn't known is the response structure.

This matters less than it looks. `/Logbook/Entries` took three attempts and
a decompilation pass to crack, because its response is a `protocol_v9`
structure walk rather than a payload (see docs/logbook-data-format.md).
These two are very likely the same shape - the same `SDS::WB::Sync<>::op()`
serves every Logbook verb - but "very likely" is exactly the kind of claim
this project has been wrong about before, and the only way to settle it is
to call them on real hardware and look.

**What would settle it**: `AppController::testEntriesFetch()` already does
the handle-fetch dance for `/Logbook/Entries`. Pointing the same code at
`/Sleep/<serial>/Entries` is a one-line probe, and whatever comes back -
data, an error, or a timeout - narrows it immediately. That needs the watch
in hand.

Worth noting: the cloud already has this data (item 1, now implemented), so
reading it from the watch directly is an improvement rather than the only
route - it would mean sleep without a Suunto account round-trip.

## Item 4 - how the watch's GPS data is updated

This is **extended ephemeris**, i.e. A-GPS: a few days of predicted
satellite orbits, so the watch gets a fix in seconds instead of minutes.
Three resources name it:

```
suunto://MDS/GNSS/%s/EphemerisData
suunto://%s/Device/GNSS/ExtendedEphemerisData/Date
suunto://%s/Device/GNSS/ExtendedEphemerisData/Format
```

The Date/Format pair reads what the watch currently holds; `EphemerisData`
is where the new blob goes. The app side is
`com.suunto.connectivity.location.*` (`GnssDate`, `GnssTime`,
`getLatestGnssVersion`, `getGnssVersionPath`, and the log line "No GNSS
version fetched, force an update from the cloud").

**Not in the capture.** The 2026-09-22 watch sync fetched SuuntoPlus
features, sport modes (a 2.2 MB zip from sportmode.sports-tracker.com) and
a firmware note - but no ephemeris. That is expected: ephemeris is good for
days, so it updates on its own schedule rather than every sync. The
download URL is built at runtime rather than stored as a string, so it
isn't recoverable from the APK's string table either.

## Item 5 - how the watch's weather is updated

The source is **OpenWeatherMap**, through the app's own API surface:

```
GET weather/v2/combined        OpenWeatherMapRestApiV2.getForecastWeatherV2AtLocation
GET weather/combined           OpenWeatherMapRestApiV2.getForecastWeatherAtLocation
GET find                       OpenWeatherMapRestApi.getWeatherConditionsAtLocation
GET onecall/timemachine        OpenWeatherMapRestApi.getHistoricalWeatherConditionsAtLocation
```

The push side is a proper sync step, not an ad-hoc write:
`WeatherResource.sync(WatchBt, builder)` calls
`WeatherUpdateModel.updateWeatherToDevice(WatchBt, ...)` and reports through
`SpartanSyncResult.Builder.weatherResult(...)`, with "Weather sync failed"
as its error. There is also a `WatchWeatherVersion`, so the watch tracks
which forecast it already has.

**Also not in the capture**, and for the same reason - the sync that was
recorded didn't include a weather refresh. `WeatherUpdateModel`'s own
methods carry no path strings, so the MDS resource it writes to is built at
runtime.

## Item 6 - implementing either

Blocked on the same thing both times: **a capture that contains the
exchange**. Neither is recoverable by reading the APK alone, because both
build their URLs and resource paths at runtime.

What would unblock it, in rough order of effort:

1. **A longer capture.** Leave mitmproxy running across a watch sync where
   the watch has been off the app for a day or more, so the ephemeris is
   stale enough to refresh. Opening the app's weather view during the same
   session should force the weather path too.
2. **A `btmon` capture alongside it**, since the HTTPS side only shows the
   download; the write to the watch is BLE and needs the same treatment the
   Logbook work got.
3. Failing both, decompiling `WeatherUpdateModel.updateWeatherToDevice` and
   the GNSS equivalent properly with Ghidra, the way `protocol_v9` was
   done - slower, and it still ends in a hardware test.

Until then this is scouting, not a plan. Worth saying plainly: items 4-6
were the ones I could make least progress on tonight, and it is the capture
that is missing rather than the analysis.
