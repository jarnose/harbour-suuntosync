# Notifications to the watch: what two shipping apps settle

Phase 0c has been the open question since the plan was written: *can a
sandboxed `harbour-` app observe other apps' notifications at all?*

**It can't.** Two real, shipping Sailfish companion apps both hit this and
both ended up in the same place, and one of them documents having confirmed
it on-device. No experiment of our own is needed to answer the question -
only to confirm the workaround behaves the same on Jarno's device.

Sources, read rather than copied (Amazfish is GPL-3.0; this project is not,
so its code stays read-only reference the same way libdivecomputer did):

- `piggz/harbour-amazfish` - Amazfit/Xiaomi wearables, 133 stars, actively
  maintained.
- `sznowicki-sailfish/vapaamin` (codefloe.com) - Garmin watches. Already
  cited in this project's plan for its BLE chunking strategy.

## The finding

Both apps carry this in their `.desktop` file:

```ini
[X-Sailjail]
Sandboxing=Disabled
```

Vapaamin's file explains why, and says it was verified on hardware:

> Omitting this section entirely ... turns out NOT to mean "unconfined" on
> this SFOS version - apps with no `[X-Sailjail]` group still get launched
> inside a Firejail sandbox with a filtering D-Bus proxy (confirmed
> on-device: `DBUS_SESSION_BUS_ADDRESS` pointed at
> `/run/firejail/mnt/dbus/user` even with no section present at all), which
> can only relay messages to/from the app's own connection - never the
> eavesdrop/BecomeMonitor traffic NotificationMonitor needs to see *other*
> apps' notifications, and, separately, never our own daemon's custom D-Bus
> service name either.

So there are two separate walls, not one:

1. The D-Bus proxy relays only the app's own traffic, so monitoring other
   apps is out.
2. A custom service name (e.g. `org.sailfishos.vapaamin.NotificationMonitor`)
   isn't in any `Permissions=` category either, so even a daemon/UI split
   can't hand the data across while sandboxed.

## The mechanism, if we go ahead

Both apps monitor the session bus the same way - vapaamin's
`NotificationMonitor.cpp` is effectively a rewrite of `libwatchfish`'s, with
the same rules:

1. Try `org.freedesktop.DBus.Monitoring.BecomeMonitor` (dbus 1.10+).
2. On failure, fall back to legacy `eavesdrop='true'` match rules.
3. Watch for `org.freedesktop.Notifications.Notify` **method calls** (not
   signals - the notification is intercepted in flight to the daemon),
   their `method_return`s (to learn the assigned id), and
   `NotificationClosed` signals.
4. Read Sailfish's own `x-nemo-preview-summary`, `x-nemo-preview-body` and
   `x-nemo-owner` hints, which is where the useful text actually lives.

Both split into a daemon plus a UI app, with the daemon doing the
monitoring. Both ship an identical privileges file:

```
/usr/bin/<daemon>,cehlmnpu
```

Vapaamin's daemon re-exposes what it sees as plain Qt signals on its own
D-Bus service, which the UI attaches to with `QDBusConnection::connect()`.
One trap it records, worth copying rather than rediscovering: the
`Q_CLASSINFO("D-Bus Interface", ...)` on that class is **required**, not
cosmetic - without it Qt builds an outgoing signal with an invalid
interface name and `dbus_message_new_signal()` aborts the process. They hit
it in practice: the daemon ran fine, became a monitor, then SIGABRT'd on
the first real notification.

## The decision this forces

`Sandboxing=Disabled` means **the app cannot go in the Jolla Store**.
Amazfish and vapaamin both ship through SailfishOS:Chum / OpenRepos
instead.

That is a product decision, not a technical one, and it is Jarno's to make.
Two things worth weighing:

- Everything this project does *today* - cloud sync, BLE logbook, health,
  upload - works fine sandboxed. Only notifications force this.
- The plan's Phase 9 already lists "Chum/OpenRepos packaging metadata", so
  Store distribution may never have been the goal.

A middle route exists and is worth considering: keep `harbour-suuntosync`
sandboxed and Store-eligible, and ship notifications as a **separate,
optional daemon package** that the user installs from Chum only if they
want that feature. That is more packaging work, and the two reference apps
did not do it - but neither of them had a working Store-eligible app to
protect when they made the call.
