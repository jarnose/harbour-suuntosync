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

## What the watch actually wants (2026-09-26, from libmds.so)

The sandboxing question was settled long ago; what a notification *is* on
the wire never was. `libmds.so` has a whole class for it,
`OBI2::ANCSNotification`, and its method names give the shape:

```
put  del  doOp  getConnectionType  parseSerial
wbNotif  legacyNotif
truncateContentIfNeeded  truncateUtf8String
```

**Two branches again**, chosen by `getConnectionType` - the same split the
GPS ephemeris turned out to have, and presumably the same two generations.

- **`wbNotif`** builds `/net/<serial>/…` and performs `add` or `delete`,
  matching the resources `/Device/Connectivity/Ble/Ancs/Notification/Add`
  and `/Del`. ANCS is Apple's Notification Center Service; Suunto's
  version rides over Whiteboard rather than over the standard GATT
  service, but the vocabulary is borrowed.
- **`legacyNotif`** builds an SML tree under the prefix
  `sml.UserNotification.` and hands it to `convertNotificationToSml`,
  which fails with "Failed to generate a string from the notification
  tree". So for the older generation a notification is an SML structure,
  not an ANCS-style record.

Field names visible in the string table: `notificationId`,
`notificationType`, `categoryId`, `requestData`, `Subheader`, `Body`,
`Timestamp`. Not a complete list and not typed - the useful ones are
obvious but their order and encoding are not.

And `truncateContentIfNeeded` with `truncateUtf8String` beside it says
there is a length limit, enforced on a UTF-8 boundary rather than a byte
one. Whatever it is, it is worth respecting rather than discovering.

**What is still missing** is the same thing that was missing for
`/Logbook/Entries` before it was cracked: the structure of the request. It
is not in the watch's SBEM descriptor tables - those describe workout
data, and neither watch's table mentions notifications at all.

Two ways to get it, in increasing order of cost:

1. **Ask the watch.** `AppController::probePath()` on
   `/Device/Connectivity/Ble/Ancs/Notification/Add` says immediately
   whether a given watch has that resource.
2. **Capture one.** An Android sync with a real notification arriving,
   the same btsnoop route everything else here came from.

### What the probe answered

Both watches have it, and both refuse to be read without arguments:

```
9 Baro  f0 12 03 01 80 00  90 01  00 00
Race    f0 12 04 01 80 00  90 01  00 00
```

`0x0190` is **400**. The GET resolved a handle - that is why the resource
counts as present - and the zero-parameter fetch was rejected, which is
what a resource that exists to be written to should do.

So **one code path covers both watches**, and the generation split that
`wbNotif`/`legacyNotif` implies is not a split between these two.
`getConnectionType` is more likely choosing by transport than by model;
whatever `legacyNotif` is for, it is not the 9 Baro.

That leaves only the field list, and 400 does not name it. The schema walk
the official app runs before its PUTs would - but that walk is
`protocol_v9`'s structure traversal, which took a Ghidra pass to
understand and has been shortcut past everywhere else in this project
precisely because the watch does not require it. Implementing it to read a
schema, when a capture shows the finished bytes directly, is the more
expensive of the two routes.

**So: capture one.** The rig is already there - btsnoop runs permanently
on the S7, the Race is paired to it, and no mitmproxy is needed because
none of this touches HTTPS. Turn notifications on in the official app,
make the phone produce one, and the bytes are in the log.

## The daemon, if it happens: who owns the link

The constraint that shapes everything: **Whiteboard is strictly one
request, one response, with matched request ids.** Two processes writing
to the same watch corrupt each other. So exactly one of them may hold a
Whiteboard session at a time - and note that this is about the session,
not the Bluetooth connection. BlueZ refcounts the connection; handing over
means `MdsWhiteboardClient::detach()` and `attachToDevice()`, which exist.

Two shapes were considered.

**The daemon owns the link whenever it is running**, and the app talks to
the watch through it. This is what Amazfish and Vapaamin do. It means the
app needs a second, complete implementation of every watch operation as a
D-Bus client, *and* keeps the direct one for when no daemon is installed -
because a Store-eligible app that stops working without a Chum package is
not what was wanted. More code, two paths to keep in step, and the daemon
becomes a dependency of the thing it was supposed to stay out of.

**The app wins ties; the daemon holds the link the rest of the time.**
The app is unchanged except for claiming the link when it needs one and
releasing it after. The daemon holds it otherwise and sends notifications.
A D-Bus name with `AllowReplacement` and `ReplaceExisting` is the whole
mechanism: the app takes the name, the daemon gets `NameLost` and
detaches; the app finishes, the daemon gets the name back and reattaches.

The second is recommended. Its cost is real and worth stating: **a
notification arriving while a sync is running is delayed** until the sync
finishes, or dropped if the daemon does not queue. A sync is minutes. The
consolation is that the app holding the link means somebody is looking at
the phone.

Nothing here is worth building before the payload is known, because a
daemon that can arbitrate perfectly and send nothing is not a daemon.
