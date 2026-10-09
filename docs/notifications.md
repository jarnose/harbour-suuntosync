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


## The captured notification (2026-09-26)

One calendar alert from the S7 to a Race - the watch showed "Testi" and
"8:49 PM" beneath it. The whole thing is a single `0x0e` PUT of 153 bytes,
preceded by the schema walk the official app always does, and that walk
names every field:

```
/Device/Connectivity/Ble/Ancs/Notification/Add
  notificationId
  requestData : AncsRequestData
      modifyExisting  categoryId  categoryCount  eventFlags  date
      appId  title  subtitle  message
      positiveLabel : LabelData { label, supportsReply }
      negativeLabel : LabelData { label, supportsReply }

AncsCategory:  IncomingCall MissedCall Voicemail Social Schedule Email
               News HealthAndFitness BusinessAndFinance Location Entertainment
```

Those are ANCS's own category names, so the vocabulary is Apple's even
though the transport is not.

### What the bytes say so far

The PUT body is `[handle 6][count=2][type 0x0007][notificationId][type][structure]`,
and the structure is a fixed part followed by a string pool starting at
byte 86.

**Strings are referenced by 32-bit offsets relative to byte 18**, which is
where the structure's own body begins. Every string in the capture checks
out:

| at | value | string |
|---|---|---|
| 30 | 68 | `org.lineageos.etar` — `appId` |
| 38 | 87 | `Testi` — `title` |
| 54 | 93 | `8:49 PM` — `subtitle` |
| 122 | 120 | `Dismiss` — `positiveLabel.label` |
| 130 | 128 | `Snooze` — `negativeLabel.label` |

And **byte 22 is a unix timestamp in seconds**: 1790444940 is 20:49 local
on the day of the capture, which is the time the watch displayed. That is
`date`, confirmed against a clock rather than assumed.

`tests/fixtures/notification_add_body.bin` holds the 153 bytes.

### What is not pinned down

Which of the remaining uint32s are `modifyExisting`, `categoryId`,
`categoryCount`, `eventFlags` and the two `supportsReply` flags, and
whether `message` was empty or is one of the strings already assigned.
Several of them read 0 or 1 in this capture, which is exactly the
situation where guessing looks easy and is not.

The cheap way through is differential rather than analytical: **capture two
or three more notifications that differ in one thing each** - a longer
title, a different app, an email rather than a calendar alert, one with no
action buttons. Bytes that move identify themselves. The rig needs nothing
new; btsnoop is always running and no HTTPS is involved.


## Three notifications, differentially (2026-09-26)

Two more, from a test app rather than the calendar:

| | app | title | subtitle | buttons | bytes |
|---|---|---|---|---|---|
| A | `org.lineageos.etar` | Testi | 8:49 PM | Dismiss, Snooze | 153 |
| B | `com.mand.notitest` | Testi ilmoitus | Alarivin teksti | Dismiss | 154 |
| C | `com.mand.notitest` | 69-character title | 54-character message | Dismiss | 246 |

**Nothing is truncated on the wire.** C's full 69- and 54-character
strings crossed intact, so `truncateContentIfNeeded` did not fire at these
lengths and what the watch showed cut short was its own screen. Where the
limit actually is remains unknown, and is now known not to be near here.

### The fixed part, field by field

Offsets are into the PUT body; string references are 32-bit and relative
to byte 18.

| at | A | B | C | reading |
|---|---|---|---|---|
| 18 | 0x02010000 | 0x02010000 | 0x02010**1** | low byte varies - the only thing that changed is that C followed B from the same app, so `modifyExisting` fits |
| 22 | 1790444940 | 1790445756 | 1790445859 | **`date`**, unix seconds, matched against the clock |
| 26 | 0x21018001 | same | same | constant across all three |
| 30 | 68 | 68 | 68 | **`appId`** offset |
| 38 | 87 | 86 | 86 | **`title`** offset |
| 54 | 93 | 101 | 156 | **`subtitle`** offset |
| 78 | **2** | **1** | **1** | **number of labels** - A had two buttons, B and C one |
| 82 | 104 | 120 | 212 | offset to the label array |
| 34, 46, 50, 62, 74 | 1, 1, 1, 42, 1 | same | same | unidentified and constant |
| 42, 58, 66, 70 | 0 | same | same | unidentified and zero |

### LabelData, and a trap in it

Each entry is **eight bytes: a 32-bit label offset, one byte
`supportsReply`, and three bytes of padding** - and the padding is
uninitialised. A carries `4b 01 00` there and B carries `73 73 00`, which
is `ss` left over from a previous `Dismiss`. C happened to get zeros.

Reading those three bytes as anything would be reading somebody else's
stale buffer. They are skipped on the way in and should be written as zero
on the way out.

### What is left

`categoryId` is among the constants, and it did not move between a
calendar alert and a test app - so both were filed the same way, and
separating it needs a notification whose category genuinely differs: an
SMS, or an incoming call. `message` was empty in all three, so whichever
offset holds it has never been exercised.

One more capture would settle both: **a notification with all three of
title, subtitle and message, and one from a different category.** After
that the encoder can be written against three golden vectors instead of
guessed at.

## The encoder can be written now, without decoding the rest

The constant block - offsets 26, 34, 42, 46, 50, 58, 62, 66, 70 and 74 -
is **byte-identical across all three captures**, which between them differ
in app, title, subtitle, button count and total length (153 against 246
bytes). Nothing about the content moves them.

That is evidence rather than hope, and it has a practical consequence:
those bytes can be replayed verbatim. An encoder that writes the constant
block as captured and fills in only the parts that were observed to
vary - `date`, the three string offsets, the label count, the offset to
the label array, and the string pool itself - should produce a
notification the watch accepts, without anyone knowing which of those
constants is `categoryId`.

So the daemon is not blocked on finishing the decode. What it gives up is
category: every notification would arrive filed the way a calendar alert
and a test app were both filed, because that is the value being replayed.
Sorting mail from messages can come later, and needs exactly one capture
whose category genuinely differs.

The risk worth naming: if one of those constants were secretly a length
or a count, replaying it would break as soon as our strings differed from
the captured ones. B and C differ by 92 bytes of string and the constants
did not move, so that is ruled out for everything except a field that
happens to be constant for both a calendar and a test app.


## The fixed part, from the app's own code (2026-09-30)

The differential capture above left two things open - which constant is
`categoryId`, and whether `message` had ever been exercised. Both are now
answered, and not by another capture: by decompiling the class that builds
the request. That is cheaper than a capture and, unlike a capture, it says
what the values *mean* rather than only what they were on one evening.

`com.suunto.connectivity.notifications.MdsNotification$Companion.create()`
builds a `MdsNotificationRequestData` - a Moshi data class, so its JSON
keys are in the dex verbatim - out of an `AncsMessage`:

```
MdsNotificationRequestData(
    modifyExisting,   // false from create(), true from createUpdate()
    categoryId,       // AncsPackages.getCategory(...)
    eventFlags,       // the literal constant 2
    date,             // AncsMessage.timestamp / 1000
    title, message,   // resolveTitleAndMessage()
    categoryCount,    // the literal constant 1
    appId,            // the posting package name
    labels)           // null when the list is empty
```

There is **no `subtitle` key at all**, in the data class or in its
generated adapter's `options`. The app never sends one.

### Byte 18 is four fields, not one

The walk's field order is `modifyExisting categoryId categoryCount
eventFlags`, and those four are **one byte each**, at 18, 19, 20 and 21:

| byte | field | A | B | C | why that value |
|---|---|---|---|---|---|
| 18 | `modifyExisting` | 0 | 0 | **1** | C followed B from the same app, so it was an update |
| 19 | `categoryId` | 0 | 0 | 0 | see below - 0 is `Other`, and correct for all three |
| 20 | `categoryCount` | 1 | 1 | 1 | a literal `1` in `AncsMessage.create()` |
| 21 | `eventFlags` | 2 | 2 | 2 | a literal `2` in `MdsNotification.create()` |

Four values, four independent reasons, all from the decompiled code rather
than from the bytes - and they happen to be exactly the bytes
`00 00 01 02`, which the differential table had recorded as one uint32
`0x02010000` with only its low byte understood.

`eventFlags = 2` is ANCS's `EventFlagImportant`. The app sets it on every
notification and never anything else.

### `categoryId` is the ANCS category, and how it is chosen

`AncsPackages.getCategory(id, packageName, category)`, in order:

1. `packageName` in `{com.google.android.dialer, com.sonymobile.android.dialer,
   com.android.phone}`: `category == "call"` → 1; no category and
   `id != 1` → 1; anything else → 2.
2. First matching package set wins:
   `com.android.providers.downloads`, `com.sec.android.providers.downloads`,
   `com.android.vending`, `com.android.systemui`,
   `com.sonyericsson.updatecenter`, `com.wssyncmldm`,
   `com.samsung.android.themestore`, `android` → **-1**;
   `com.android.dialer`, `com.android.incallui`, `com.asus.asusincallui`,
   `com.samsung.android.incallui`, `com.samsung.android.dialer` → **1**;
   `com.android.server.telecom` → **2**;
   `com.sonyericsson.conversations`, `com.android.mms`, `com.htc.sense.mms`,
   `com.pantech.app.mms`, `com.asus.message`, `com.android.contacts`,
   `com.samsung.android.messaging`, `com.google.android.talk`,
   `com.google.android.gm`, `ch.protonmail.android`,
   `com.suunto.suuntoandroidtest` → **6**.
3. Otherwise `Notification.category`: `email` → 6, `event` → 5,
   `location_sharing` → 10, `msg` → 6, `reminder` → 5, `social` → 4,
   `workout` → 8.
4. Otherwise **0**.

Those numbers are the standard ANCS `CategoryID`s, and they line up with
the enum the schema walk printed, in its order: 1 IncomingCall,
2 MissedCall, 3 Voicemail, 4 Social, 5 Schedule, 6 Email,
7 News, 8 HealthAndFitness, 9 BusinessAndFinance, 10 Location,
11 Entertainment, with 0 as `Other`. Two independent sources agreeing on
eleven numbers is not a coincidence, so the vocabulary is settled.

Note what the table means for SMS: a text message is filed as **Email**,
because `com.samsung.android.messaging` and friends map to 6, the same
value `category == "msg"` maps to. The watch has no separate message
category and the app does not invent one.

And note why all three captures read 0: `org.lineageos.etar` and
`com.mand.notitest` are in none of those package sets, and neither set a
`Notification.category` the table knows. Zero was the right answer, so the
capture was never going to separate `categoryId` from the other constants
by itself.

**A falsifiable prediction**, which is the point of writing this down:
post a notification from the same test app with
`setCategory(Notification.CATEGORY_MESSAGE)` and byte 19 must read 6.
Nothing else in the body should move.

### Offset 54 is `message`, not `subtitle`

Since the app sends no subtitle, the string at 54 - `8:49 PM` in A,
`Alarivin teksti` in B - is `message`. Both were the notification's body
text, which is what `resolveTitleAndMessage` returns as the second
element. So `message` was exercised in all three captures after all, and
the earlier "`message` was empty in all three" is withdrawn.

`subtitle` is then one of the remaining constants, pointing at an empty
string. Byte 19 is a zero byte, and a string offset of 1 relative to byte
18 lands on it; offset 46 reads exactly 1 in all three captures, which
fits, but a pointer into the middle of the header is odd enough that it is
recorded as a fit rather than a finding.

### How the app picks title and message

`resolveTitleAndMessage(Notification, categoryId)` reads
`EXTRA_TITLE`, then `EXTRA_TEXT`, then `EXTRA_SUB_TEXT`, treating an empty
string as absent throughout, and falls back to `tickerText` and then
`extras["android.infoText"]`. If nothing yields a title it logs the
available extras and drops the notification.

It also carries two special cases - `categoryId == 1` clears both strings
and replaces an empty message with a single space, `categoryId == 2` turns
an absent title into the literal `Unknown` - which appear to be
unreachable, because every package that can produce those two ids is on the
suppressed list below and never reaches this function. Recorded because
they are in the binary, not because they were observed.

### What arrives with its text stripped

`shouldIgnoreNotification` does not discard anything. `create()` still
builds a message and still sends it; it just sets title and message to
empty strings, so the watch gets the category and the count and no text.
That is the mechanism behind an incoming call: the watch renders its own
call screen from `categoryId = 1` rather than from anything we wrote.

Stripped:

- any package in `CALL_PACKAGES`, which is the union of the three call sets
  above - so **every dialer, in-call UI and telecom notification**
- any package with `clock` in its name after the first dot
- `com.miui.screenshot`, `com.miui.securitycenter`,
  `com.mi.globalminusscreen`, `com.miui.cleaner`, `com.miui.gallery`
- `Notification.visibility == VISIBILITY_SECRET`
- `Notification.category` in `{alarm, err, navigation, progress, promo,
  recommendation, status, service, stopwatch, sys, transport}`
- channel id `channel_id_alarm`
- `StatusBarNotification.getTag() == "MissedCallNotifier"`
- `FLAG_LOCAL_ONLY`, `FLAG_ONGOING_EVENT` or `FLAG_GROUP_SUMMARY` set -
  `com.tencent.mm` is exempt from the first two, by name, lowercased

### What this leaves

The encoder can now write `categoryId` as a real value rather than
replaying a zero, which was the one thing the previous section said it
would have to give up. What is still unidentified is which constants are
`subtitle` and the two `LabelData` structs' fixed parts - and neither
blocks anything, because both were constant across captures that differed
by 92 bytes of string.

No further capture is needed to build this. One would still be worth
running as a check rather than as discovery: a notification with
`CATEGORY_MESSAGE`, to confirm byte 19 reads 6.

### Exercising a category without a SIM

The S7 has no SIM, so neither a text message nor a call can arrive on it.
Neither is needed. `getCategory` reads a package name and a string field;
nothing in it touches the radio.

- **`categoryId = 6`**, which is what a text message produces: post a
  notification with `setCategory(Notification.CATEGORY_MESSAGE)` or
  `CATEGORY_EMAIL` from any package. A mail app on an IMAP account does it
  by itself, and so does any XMPP or Matrix client - none of which want a
  phone number.
- **`categoryId = 1` and `2`**, incoming and missed call: these come only
  from the package name, so the notification has to be posted *by* a
  dialer. A SIP client that registers with Telecom (Linphone with its
  system-integration option on) makes `com.android.incallui` post the
  ringing notification on a VoIP call, with no SIM involved. Both arrive
  with their text stripped, per the section above, so the only thing to
  check is byte 19.
- **`categoryId = 5, 4, 8, 10`**: `event`/`reminder`, `social`, `workout`,
  `location_sharing` respectively, all reachable from `setCategory` alone.

`cmd notification post` over adb needs no SIM and no app, and its
`-S bigtext` style will exercise a long `message`, but it posts as
`com.android.shell` and sets no category, so it can only ever produce 0.
The test app that produced captures B and C is the cheaper place to add one
`setCategory` call.


## The encoder (2026-09-30)

`src/ble/notificationcodec.h`/`.cpp`, Qt-free, and
`tests/test_notificationcodec.cpp` rebuilds all three captured requests
**byte for byte** - not "the constants replayed", the whole body computed
from an app id, a title, a message, a date and a list of buttons.

Three things had to be understood first that the differential pass had not
reached:

**Byte 15 is a length.** It reads 136, 137 and 229 in the three captures,
which is each body's total minus 17, every time. So the structure is
length-prefixed and the prefix has to be computed; replaying it would have
worked only for a notification the same size as the captured one. Five
more structure-carrying writes elsewhere in the same log follow the same
rule, so it is not a coincidence of these three.

It is **one byte**, which caps the whole request at 272 bytes - and C, at
246, was already close. `truncateToFit()` shortens the message and then
the title on UTF-8 boundaries; `encodeAdd()` refuses rather than letting
the length wrap.

**The parameter's type code is not a constant.** It reads `0x1209` here,
and its high byte is the ack body's second byte in all six structure
writes in the capture - `0xa4` with an `0xa4` handle, `0x31` with `0x31`,
and so on. A 9 Baro hands out its own handles, so this is computed from
the ack rather than compiled in. The low byte, `0x09`, identifies
`AncsRequestData` within that family and is still only known from a Race.

**`notificationId` is derivable.** `AncsMessage.createId()` is
`abs(((sourceId * 31 + categoryId) * 31 + appId.hashCode()))` clamped at
zero, and it reproduces both captured ids exactly - Etar's notification 1
and the test app's notification 0. That matters because a removal, and an
update, address a notification by this number.

The string pool's own rules fell out of the three captures agreeing: it
starts at a fixed offset 68 from the structure body, holds appId, title
and message in that order, then pads to a four-byte boundary for the label
array, whose entries are an offset, a `supportsReply` byte and three bytes
of padding the watch never initialises.

### Not yet confirmed

**Superseded - see "It works" and the daemon sections below.** Kept as
written because it records what was and was not known at the time.

Nothing here has been sent to a watch. Three captured requests reproduced
byte for byte is a strong check on the layout and no check at all on
whether the watch accepts one we composed. The 9 Baro is a second
question again: its handle differs, and the structure's low type byte
might.

### What the watch answers

A PUT to `/Notification/Add` comes back as a `0x07` with status **200**,
and so does a delete that found something. Deleting an id the watch does
not have answers **403** - measured, in a capture where the app removed a
notification twice.

The delete resource is `/Device/Connectivity/Ble/Ancs/Notification/Del` -
`Del`, not `Remove`. The name appears only on the wire; the app builds the
path at runtime, so nothing in the dex spells it out.

That 403 is worth keeping rather than flattening into "failed": it is the
difference between a notification the user dismissed on the watch and one
that never arrived.

### The probe

`AppController::testNotification()`, on the pairing page, with fields for
a title, a message and a category. It writes to the watch's notification
queue, so unlike `probePath()` it is not read-only and is a button rather
than anything automatic. "Take it back" removes the same id.

The category picker is there because the category is the one field whose
effect cannot be seen from this side at all - only the watch's own screen
shows whether 6 is filed differently from 0.

### It works (2026-10-02)

A notification composed by this app, from the encoder above, **arrives on a
Suunto Race and renders properly** - title and message both. Status 200,
first try, no capture replayed.

So the layout derived from three captures is right, and the parts that were
computed rather than copied - the length byte, the parameter's type code,
the string pool's offsets, the notification id - are all right too. That
was the open question the previous section ended on.

Still untested: the 9 Baro, and whether a non-zero `categoryId` changes
anything the watch shows.


## The daemon, built (2026-10-02)

The decision was made: **a separate package, and the application stays
Store-eligible.** `suuntosync-notifyd`, built from the same tree with

```
sfdk -c specfile=daemon/suuntosync-notifyd.spec build
```

which sets `-DBUILD_APP=OFF -DBUILD_DAEMON=ON`. Its SPEC file lives in
`daemon/` rather than `rpm/` for a dull but load-bearing reason: sfdk
refuses to build when it finds two SPEC files in `rpm/`, and Qt Creator
passes it no `specfile` option, so a second one there breaks the ordinary
application build outright. It installs as a systemd *user* service,
`Requires: harbour-suuntosync` because the app owns the pairing and the
database it reads.

### Two premises, measured on the phone first

Neither of these was assumed, because the whole package split rests on them.

**An unsandboxed process does see other applications' notifications.** A
`dbus-monitor` run over SSH caught a `Notify` call made from a separate
connection in full - app name, summary, body, every hint - and then the
reply carrying the id the server assigned. The bus is D-Bus 1.16.2, so
`BecomeMonitor` is available and succeeded. This is the premise, and it is
why the daemon is a separate package: inside Sailjail the filtering D-Bus
proxy relays only the sandboxed application's own traffic.

**`defaultuser` can drive BlueZ.** It is in the `bluetooth` group, and
enumerating `org.bluez`'s object tree from a plain SSH shell returns the
adapter, both bonded watches, and their services and characteristics.

### What could not be done with a D-Bus name, and what replaced it

The plan above was one well-known name with `AllowReplacement` and
`ReplaceExisting`: the app takes it, the daemon yields. **That does not
work**, and the reason is on the device rather than in the design.
Sailjail's `Base.permission` grants a sandboxed application

```
dbus-user.own       org.sailfishos.coveraction.*
```

and nothing else *from a permission file*. **Correction, 2026-10-02:** the
running application's firejail command line also carries

```
--dbus-user.own=io.github.jarnose.suuntosync
```

which sailjail derives from `OrganizationName` and `ApplicationName`. So a
sandboxed app does get exactly one name of its own - that one, with no
sub-names under it, so `io.github.jarnose.suuntosync.WatchLink` is still
out. Arbitration on the bare name would in fact have worked, with the
daemon watching `NameOwnerChanged`. The flock stays because it is in and
proven, and because it covers "the app is mid-send" as well as "the app is
running"; but the claim that a sandboxed app cannot own a name was too
strong and is withdrawn.

Anything beyond that one name would need a permission file installed into
`/etc/sailjail/permissions` - which is exactly what Whisperfish ships
(`dbus-user.own be.rubdos.harbour-whisperfish.*`), and which is not a thing
a package aimed at the Store should be doing.

So the arbitration is an **advisory file lock** - `flock` on
`watch-link.lock` beside the database, in the directory both processes
already agree on. No permission is involved, and the kernel releases it when
the holder dies, which a D-Bus name does not improve on.

The two sides are deliberately asymmetric:

- The **application** takes the lock when it attaches to the watch, waiting
  up to five seconds in quarter-second steps for it, and holds it until it
  detaches. It wins ties by only being willing to wait that long; after that
  it attaches anyway, because a stuck daemon should not make the watch
  unusable from the app.
- The **daemon** attaches only while it holds the lock, sends what is
  queued, and lets go immediately. So "the app is open" means notifications
  wait, and nothing else does.

**Corrected 2026-10-02.** The application used to attach *regardless*, at
once, on the reasoning that it is the side with a person waiting on it. That
reasoning is wrong in its consequence: barging in puts two writers on one
Whiteboard session, the framing corrupts, and the symptom is the watch never
acknowledging the session handshake - a true error message about a
self-inflicted problem, and one that reads exactly like a broken watch. It
was reported that way. Waiting costs nothing, because a second or two is all
the daemon ever holds the lock for.

It also explains why the notification switch had nothing to show: reading
the watch's setting needs a working session, the session never came up, and
the switch had no way to say so.

The cost, stated plainly: each notification the daemon sends pays for an
attach - StartNotify plus the session handshake, a second or two. On a wrist
that is invisible. The alternative was holding the session permanently and
having to hand it back, which is the part that needed the name.

### What it does with a notification

`src/notify/notificationrouter.*` is Qt-free and tested
(`tests/test_notificationrouter.cpp`, 32 assertions, in CI). Sailfish's
real category strings - read off
`/usr/share/lipstick/notificationcategories` on the device, not taken from
the freedesktop specification - map to ANCS categories by longest prefix:

| Sailfish | ANCS |
|---|---|
| `x-nemo.call.missed*` | 2 MissedCall |
| `x-nemo.messaging.voicemail*` | 3 Voicemail |
| `x-nemo.messaging.*` (sms, mms, im, group) | 6 Email |
| `x-nemo.calendar*` | 5 Schedule |
| `x-nemo.social*` | 4 Social |
| `im.received` | 6 Email |
| `harbour-whisperfish-call` | 1 IncomingCall |
| `harbour-whisperfish-message` | 6 Email |
| anything else | 0 Other |

A text message is **Email**, because that is what the official Android app
does and the watch has no message category. And an incoming call is not
normally a notification on Sailfish at all - voicecall-ui shows its own
screen - so category 1 arrives only from a VoIP app that posts one. That is
also the only way to exercise it on a phone with no SIM, which answers the
question from a week ago about simulating a call.

Dropped before the watch: our own notifications, anything with no title,
and `x-nemo.battery`, `x-nemo.system-update`, `x-nemo.messaging.error`,
`x-nemo.messaging.authorizationrequest`, `x-jolla.lipstick.*`,
`x-jolla.cellular.error`.

`x-nemo-owner` is preferred over `app_name` as the `appId` the watch shows,
and the two preview hints over the plain summary and body, because that is
where the text a person actually reads tends to be.

### Testing it

`suuntosync-notifyd --dry-run` monitors and logs and never touches a watch,
which separates the two halves. The real run wants the app closed, since the
app holds the lock while it is attached.

**The monitoring half is confirmed on the phone (2026-10-02.)** Five
notifications posted from a separate connection, five correct decisions:

```
notification 500 category=x-nemo.messaging.sms      -> ancs 6: Matti Meikalainen / Moi, nahdaanko illalla?
notification 501 category=x-nemo.call.missed        -> ancs 2: Vastaamaton puhelu / Matti Meikalainen
ignoring x-nemo.battery from lipstick: category stays on the phone
notification 503 category=x-nemo.calendar.reminder  -> ancs 5: Testi / 20:49
ignoring x-nemo.messaging.im from someapp: no title
```

`BecomeMonitor` worked from the binary, not only from a shell, and the
preview hints came through as the title and body.

### The bug that found itself immediately

The first run monitored nothing and said

```
QSocketNotifier: Invalid socket 6 and type 'Read', disabling...
```

The filter was returning `DBUS_HANDLER_RESULT_NOT_YET_HANDLED`, with a
comment explaining that a monitor must not claim messages addressed to
somebody else. That reasoning is exactly backwards. `NOT_YET_HANDLED` lets
libdbus fall through to its own default, and its default for a method call
nobody handled is to **send back
`org.freedesktop.DBus.Error.UnknownMethod`** - it does not check who the
message was addressed to. A monitor that sends anything is disconnected by
the bus, so the first notification closed the socket under us, every time.

`DBUS_HANDLER_RESULT_HANDLED`, unconditionally, is correct: on a monitor
connection nothing else is going to act on these messages, and the only
alternative is libdbus answering on our behalf. libwatchfish returns
`HANDLED` from its filter too, which was the confirmation after the fact.

One practical note for testing: Sailfish's Qt sends `qInfo`/`qDebug` to the
journal, so a manual run needs `QT_LOGGING_TO_CONSOLE=1` or the log looks
empty.

### The sending half works too (same day)

End to end, with the application closed and nothing touched by hand:

```
15:17:58 watching the session bus via BecomeMonitor
15:17:58 paired watch: "Suunto Race 2352D0000247" "0C:8C:DC:C2:13:59"
15:17:58 watch connected
15:18:46 notification 513 from jolla-messages category=x-nemo.messaging.sms -> ancs 6: Liisa Virtanen / Nahdaanko kuudelta?
15:18:47 whiteboard session ready
15:18:48 the watch took notification 2073402632
15:18:48 whiteboard session gone
15:19:08 whiteboard session ready
15:19:08 took notification 2073402632 off the watch
15:19:08 whiteboard session gone
```

The watch showed it. The last three lines were not planned for this test and
are the better result: when the phone's own notification expired, lipstick
sent `NotificationClosed`, and the daemon took the notification back off the
watch - so the removal path is confirmed as well.

Two bits of the design hold up in the numbers. Attaching costs about a
second (46.7 -> 47.3 to ready, 48.1 for the send), and re-attaching after
having let go costs about a tenth of that, because the Bluetooth connection
is deliberately left up. Letting go after each burst is what makes the
file-lock arbitration cheap.

### Three more bugs that only running could find

**The daemon never connected to the watch.** It waited for BlueZ to report
a connection and only ever acted on one, which meant it worked exactly once
- in the first test, where the application had just been using the watch so
the link was still up. A watch drops the link within seconds of the last
client letting go, so by the time a notification arrives there is normally
nothing to attach to. It opens the connection itself now.

**`connectFinished` arrives before the `Connected` property.** Pumping the
queue on the strength of the property alone spun: every pump saw "not
connected" and started another connect. The log filled with 15,273 lines of
`connecting to "Suunto Race"`, dozens per millisecond. Taking
`connectFinished(ok)` as connected - which is what the application has
always done - is the fix.

**A stale handshake timeout reported failure for a session that had
succeeded.** The handshake's 10-second timer captured only `this`, and
`detach()` resets the acked flag, so a session handed back cleanly produced
`Watch didn't acknowledge the session handshake` ten seconds later. The
timer now carries the session generation it belongs to and says nothing if
that has moved on. This one was in `MdsWhiteboardClient`, so the
application had it too.

### Email, and what naming the package was for

Two genuine notifications arrived during a test - GitHub emails - and they
carry **no `category` hint at all**, so they landed as Other. The log was
changed to name the sending package for exactly that reason, and a
deliberate test message answered it:

```
notification 531 from messageserver5 category=(none) -> ancs 0: Jarno Selanpaa / testiviestin otsikko
notification 532 from messageserver5 category=(none) -> ancs 0: Jarno Selanpaa / testiviestin otsikko
```

`messageserver5` is Sailfish's own mail server, and it sets no category at
all. So `categoryFor()` has a second table, consulted only when the category
says nothing: one entry, `messageserver5` -> Email, because one is what has
been measured. It grows that way or not at all. A category still wins when
an app sets one, because an app that sets one means it.

**And it posts twice.** Ids 531 and 532, same sender, same subject, within
the same second - and because the watch id is derived from the phone's id,
those are two different notifications as far as the watch is concerned. One
message arriving, two appearing on a wrist. The daemon now drops a
notification whose package, title and message match the last one within
three seconds: long enough to catch a double post, short enough that two
genuinely identical messages a few seconds apart both get through.

Confirmed on the phone, with the same text the mail server sent:

```
21:01:39 notification 533 from messageserver5 category=(none) -> ancs 6: Jarno Selanpaa / testiviestin otsikko
21:01:39 ignoring notification 534: the same one again
21:01:41 ignoring notification 535: the same one again
```

### 403 confirmed live, by accident

Two daemons ran at once for a few minutes - one started by hand while a test
instance was already up - and the log is worth keeping:

```
waiting for the watch: the other process is using the watch
...
removal failed: "The watch has no notification with that id"
```

The first line is the file lock doing exactly its job: the second instance
queued and waited rather than writing into the first one's session. The
second is the first time the **403** path has been seen on hardware, which
confirms the status code taken from the capture - both instances see the same
bus, so both tried the removal, and the one that had never sent anything got
told so by the watch.

A single instance is what the systemd unit gives, so this is not a case to
design for; it is a free measurement.

### The systemd unit, confirmed without being able to read the log

`pkcon install-local` of the RPM enables and starts the service from its
`%post`, and it comes up `enabled` and `active`. Proving it *works* needed a
detour, because the journal is not readable: `defaultuser` is not in
`systemd-journal`, so the service's own output is root-only
(`devel-su journalctl _COMM=suuntosync-noti`).

The flock turned out to be the evidence. With the service as the only daemon
running, a notification posted on the phone produced this in `/proc/locks`:

```
t+0s  FLOCK ADVISORY WRITE 36929 fe:02:6686107 0 EOF
t+1s  FLOCK ADVISORY WRITE 36929 fe:02:6686107 0 EOF
```

pid 36929 being `/usr/bin/suuntosync-notifyd`, the service. It took the lock
for about two seconds and let go - attach, send, release - which is the whole
cycle, observed from outside the process.

Two notes for whoever runs this next:

- **`systemctl-user` is root-only**; from a normal shell it is
  `systemctl --user`. The `%post` scriptlet uses the former because it runs
  as root during installation.
- **`Restart=on-failure` does not restart after a `kill`.** systemd excludes
  SIGTERM, SIGINT, SIGHUP and SIGPIPE from "failure", so killing the daemon
  by hand leaves it stopped until it is started again - and a crash still
  restarts it. That is the behaviour worth having, and it is easy to mistake
  for the unit being broken.

### An accident worth keeping

Two instances ran at once for a while, the service and a hand-started one.
They both sent, and the watch did not end up with two notifications -
because `notificationId` is derived from the phone's id, the category and
the package, so both instances computed the same number and the second send
was a re-add of the same notification. Predictable in hindsight, and not
something that was designed.

### Four hours later, two bugs the clock found

The service was still up after four hours, same pid, no restarts, and it
reacted to a notification immediately - so the monitor connection survives.
What it then did was wrong in two ways, and both needed the watch to have
gone away, which is the state no earlier test had been in.

**It believed a four-hour-old "connected".** `BluezAdapter` does not
subscribe to a device's `PropertiesChanged` - it learns state from
`refresh()` and `InterfacesAdded` only - so a disconnect is invisible to it.
The application gets away with that because a person drives it and the
pairing page refreshes; a daemon does not. Believing it, the daemon claimed
the watch lock and sat in an attach that could never finish, **holding the
lock indefinitely** - the one thing the lock exists to avoid doing to the
application. It asks BlueZ for the property now, every time it matters.

**An attach had no deadline.** `MdsWhiteboardClient` waits for
`ServicesResolved` with no timeout of its own, and its handshake timeout
only starts once the handshake has been sent - so an attach that never got
that far hung for ever. The daemon gives it twenty seconds and lets go.

And one thing that was missing rather than wrong: a queued notification had
no expiry, so a watch out of range meant a connect attempt every five
seconds for as long as the daemon lived. Two minutes is the limit now, and
the queue emptying is what stops the retries.

With all three, a notification arriving while the watch is away reads like
this:

```
19:42:14 notification 528 from jolla-messages category=x-nemo.messaging.sms -> ancs 6: Kello poissa / ...
19:42:14 connecting to "Suunto Race 2352D0000247"
19:44:24 could not connect to the watch: Did not receive a reply ...
19:44:30 giving up on a notification the watch never took
```

One connect attempt, no lock held at any point in those two minutes, and the
queue empty afterwards. BlueZ's `Connect()` took 130 seconds to fail, which
is worth knowing and is not something to fix here.

### The journal is not where to look

`Storage=volatile` and `SplitMode=none` on this device, and the phone
produces enough log noise that the window is **seconds** wide: "Logs begin
at 19:38:02" with the clock at 19:38:11. Adding `defaultuser` to
`systemd-journal` (`gpasswd -a`, since there is no `usermod` here) makes the
journal readable, and there is nothing in it to read. `/proc/locks` turned
out to be the more useful instrument.

So the daemon keeps its own log:
`~/.cache/io.github.jarnose/suuntosync/suuntosync-notifyd.log`, appended to,
rotated once at 256 kB, and still written to stderr as well so running it by
hand is unchanged. `--log <file>` moves it and `--no-log` turns it off.

### A packaging bug worth the two minutes it cost

`%post` ran `systemctl-user enable --now`, and `--now` does nothing to a
service that is already running - so installing an upgrade left the old
binary running and the new one unused on disk, with `ActiveEnterTimestamp`
from before the install as the only clue. It is `enable` followed by
`restart` now: restart starts a stopped service and replaces a running one,
which is what an install and an upgrade both want.

Worth knowing for testing by hand:
`systemctl --user restart suuntosync-notifyd.service` needs no root.

### The 9 Baro refuses it: status 400 (2026-10-02)

Everything up to the watch worked. The application held the lock while it
was open, the daemon queued and waited, the lock freed when the app closed,
the daemon connected, the session opened - and the watch answered the PUT
with **400**:

```
21:07:17 notification 536 ... -> ancs 6: Baro-testi / Nakyyko tama kellossa
21:07:17 waiting for the watch: the other process is using the watch
   (x4, five seconds apart, while the application held the lock)
21:08:07 whiteboard session ready
21:08:07 sending failed: "The watch refused the notification (status 400)"
```

400 means the frame was understood - the CRC passed and the handle resolved
- and the body was not. Two things can account for that, and only one of
them is expensive:

1. **Notifications may simply not be enabled on this watch.** The Race has
   been used with the official app and had them turned on at some point;
   the 9 Baro may never have. The resource exists either way, which is why
   the handle resolved. `/Settings/Ble/AncsEnabled` is readable with the
   application's probe.

   **This was it** - see below.
2. **The structure's type byte or its form byte may differ.** `0x09` and
   `0x6a` are known from a Race and nothing else. The type's *high* byte is
   computed from the ack, so it is not a suspect. If this is the cause the
   answer is not guessable: it needs a capture of the official Android app
   sending a notification to a 9 Baro - the same rig that produced the
   Race's bytes.

   **This is where it stands** - see below.

Worth stating because it is the opposite of a disappointment: the
arbitration, the connect-on-demand, the queue and the lock all behaved
exactly as designed against a watch that then said no.

### `/Settings/Ble/AncsEnabled` is the answer, and it was in the captures

Jarno checked the 9 Baro's own menu and notifications were on there, which
ruled out the obvious reading of hypothesis 1 and not the hypothesis. The
answer had been sitting in two week-old captures: the official app reads
`/Settings/Ble/AncsEnabled` on every connection, and both replies are
there.

```
9 Baro   f0 3b 05 01 80 00  c8 00  01 00 00     value 0
Race     f0 3b 05 01 80 00  c8 00  01 00 01     value 1
```

Same handle, same 200, and the value - a 16-bit little-endian integer at
the end - differs. **ANCS is off on the Baro and on on the Race**, at the
protocol level, whatever the watch's own menu says about notifications.
That is a 400 on a notification PUT, exactly.

The probe could never have found this, and the earlier conclusion that
"one code path covers both watches" was drawn from the resource *existing*
on both. It does. Its value is what matters, and only reading it says so.

### Writing it

The official app's own write of a `/Settings/...` enum is in the same
capture - `/Settings/Unit/WeekType`:

```
f0 39 02 01 80 00  01  03 00  00
[ack handle    ]  [1] [type ] [value]
```

One parameter, type **0x0003**, one byte of value. So
`Mds::kParamSmallEnum` and `MdsWhiteboardClient::putSmallEnum()`, and a
switch in Settings that reads the watch's value and writes it. The switch is
hidden until the watch has answered, because a switch showing a guess is
worse than no switch - and it is labelled as the watch's own setting,
because that is what it is.

What is assumed rather than captured: that `AncsEnabled` takes the same
type as `WeekType`. The read reply's shape agrees, and the cost of being
wrong is a 400 and a switch that snaps back.

### Not the setting, and not Do Not Disturb either

Two things were eliminated by trying them.

The watch had **Do Not Disturb** on. Turning it off changed nothing: still
400.

And `/Settings/Ble/AncsEnabled` **reads 1 on the Baro now**, measured
directly rather than from a week-old capture:

```
/Settings/Ble/AncsEnabled = 11 bytes: f0 3b 05 01 80 00 c8 00 01 00 01
```

So the setting difference was real when the captures were taken and is not
the current cause. Enabled, not disturbed, and still refusing.

### Which leaves the structure, and there is a precedent for that

`libmds.so` has `SDS::WB::ConnectionType` with a `LEGACY` member, and every
`legacy/` source path in it belongs to `legacy/Communist/...` - the
Ambit-era NSP protocol. **A 9 Baro is not a legacy device**: it speaks the
same Whiteboard this project has used to decode its workouts, its ephemeris
and its daily activity. So `legacyNotif` is not what it wants, and the
resource path is not the difference.

That leaves what is inside the structure, and the precedent is strong
rather than speculative: **the Baro's SBEM descriptor ids were completely
different from the Race's**, which is why the field table is read off
whichever watch is connected instead of compiled in. The notification
structure's type id (`0x1209`) and its form byte (`0x6a`) come from the
same kind of per-firmware metadata.

Two ways to get them, and the first has worked four times already:

1. **Capture one.** The official Android app sending a notification to the
   9 Baro, on the rig that produced the Race's three captures. Known cost,
   ground truth, and it would also validate the second route.
2. **Ask the watch.** Implement `protocol_v9`'s structure traversal - the
   schema walk the official app runs before its PUTs and that this project
   has shortcut past everywhere.

The first one took twenty minutes, so the second is still unbuilt.

## The 9 Baro's own bytes (2026-10-05)

The S7 on a USB cable, the 9 Baro paired to it, HCI snoop already running,
one `cmd notification post` over adb, and the log grew by 14 kB. The PUT is
170 bytes on ATT handle `0x000e`.

**The layout is identical.** The length rule, the structure tag, the four
one-byte fields, the string-offset base, the pool order, the four-byte
alignment before the label array, the label entries - all of it. Exactly
five values differ:

| | Race | 9 Baro |
|---|---|---|
| structure type | `0x1209` | `0x1207` |
| form byte | `0x6a` | `0x00` |
| prologue | `01 1f 01 21` | `01 00 00 00` |
| word at rel 28 | 1 | 0 |
| word at rel 44 | 42 | 0 |

Which is the same kind of difference the SBEM descriptor ids turned out to
be: per-firmware metadata, not protocol. Three of the four constants that
were never understood are simply **zero** on the Baro, which says what they
probably are - optional metadata an older firmware does not fill in. Both
watches' values for rel 28 and rel 44 point at a NUL byte inside the header,
so both spellings mean "an empty string".

So `Ancs::Profile` is five fields, there are two of them, and
`profileForAck()` picks one by the third byte of the handle the watch gave
for `/Notification/Add` - `0x04` on a Race, `0x03` on a 9 Baro. **A watch
that is neither gets no profile and `encodeAdd()` refuses**, because sending
a Race's profile to a 9 Baro is precisely what produced the 400 this all
started with, and "nobody has captured this watch" is more useful than
repeating that.

`tests/test_notificationcodec.cpp` now reproduces all four captured
requests byte for byte - three from a Race and this one - and checks that
the same notification through the other watch's profile comes out
different, so the profile cannot quietly stop being applied.

Noted and not relied upon: `2 * ack[2] + 1` gives both type bytes. Two
points fit any line, and the arithmetic does not hold for the other
structure-carrying resources in the capture.

### And it takes one (2026-10-05, twenty minutes later)

Paired back to the Sailfish phone, switched to in the application, and the
daemon composed one from the Baro's profile:

```
11:27:02 paired watch: "Suunto 9 182610000067" "0C:8C:DC:26:57:2C"
11:27:02 watch connected
11:27:08 notification 45 from jolla-messages category=x-nemo.messaging.sms -> ancs 6: Baro profiililla / Hyvaksyyko se nyt
11:27:08 whiteboard session ready
11:27:08 the watch took notification 2073852380
11:27:08 whiteboard session gone
```

And it was on the watch's screen, which is the half of that the log cannot
report.

**Both watches, then.** Which closes the notification path: the monitor, the
routing, the encoder, the arbitration, the connect-on-demand, the queue, the
removal, and two watch models whose requests differ by five constants.

One practical note from the switch. A Suunto watch bonds to one phone at a
time, so pairing the Baro to the Android phone for the capture removed its
BlueZ object from the Sailfish phone entirely - the application fell back to
the Race, and the daemon followed the database. Nothing was broken; it is
just a thing to know before wondering why a watch that is "right here" is
not there.

### A stale watch, found the same minute

The daemon read the active watch from the database **only at startup**, so
switching watches in the application left it addressing the old one. It
re-reads whenever it has something to deliver now - one indexed SELECT on a
local file - and when the address has changed it puts down the session, the
lock and its belief that anything is connected.

This was visible the moment the 9 Baro was connected: the log still said
`paired watch: "Suunto Race ..."` while the database already said
`Suunto 9 ...`, and only a restart moved it.



## An incoming call is not a notification (2026-10-06)

Jarno noticed that the watch said nothing while the phone rang, and then
announced the call once it had been missed. The reason turned out to be
exactly that: **a ringing call never reaches the notification server**, so a
daemon watching notifications cannot see one.

A session-bus capture of one real incoming call, rejected without answering,
settles it. Times are seconds from the same clock:

```
147.833  /calls/a4f3...  VoiceCall.statusChanged   int32 5   "incoming"
147.833  /calls/active   VoiceCall.statusChanged   int32 5   "incoming"
147.834  /              VoiceCallManager.voiceCallsChanged
147.976  /              VoiceCallManager.playRingtone  jolla-ringtone.ogg
157.690  /calls/a4f3...  VoiceCall.statusChanged   int32 7   "disconnected"
157.697  /calls/a4f3...  VoiceCall.statusChanged   int32 0   "null"
157.712  /org/freedesktop/Notifications  Notify          <- the missed call
```

Ten seconds of ringing with no `Notify` in it, and the first one arrives after
the call is over. Corroborated from the other side: the device's
`/usr/share/lipstick/notificationcategories` has `x-nemo.call.missed.conf` and
`harbour-whisperfish-call.conf` and **no** `x-nemo.call.incoming` of any kind.
So there was never a category for `categoryFor()` to map; the router's own
comment had said as much in words since the table was written.

### What the signal gives, and three details that matter

`org.nemomobile.voicecall.VoiceCall.statusChanged(int32, string)`.

- **The text, not the number.** The capture shows `5 "incoming"`,
  `7 "disconnected"` and `0 "null"` - three values of an enum whose definition
  this project has never seen. The string is self-describing and the number is
  not, so `CallMonitor` matches on the string.
- **Every signal arrives twice**, once on the call's own object and once on
  the fixed `/calls/active` alias. Acting on both would ring the watch twice
  and then try to clear one notification twice, so the alias is ignored.
- **The caller's number is a second round trip**, `Properties.Get` for
  `lineId` on the call's object. The property name came from the symbols in
  the device's own `libvoicecall.so.1.0.0` - `lineId`, `isIncoming`,
  `isForwarded`, `statusText` - rather than from guessing at the interface.
  The read is asynchronous: a daemon that blocks on D-Bus while the phone
  rings is a daemon that has stopped answering its own bus. A call can end
  while that read is in flight, which is a second or two for a rejected call,
  and announcing it then would leave a ring on the watch that nothing would
  clear - so an abandoned lookup is dropped rather than delivered late.

### What the watch now sees

ANCS category **1, IncomingCall** - the one value the mapping table could
never produce - with the caller's number as the title and "Incoming call" as
the message, removed again the moment the status stops being `incoming`.
Answered, rejected and missed all mean the same thing to the ring: stop.

A missed call still arrives afterwards as a real notification, category 2.
That division is right rather than redundant: the ring is transient and the
miss is a record.

Deliberately **no Dismiss label** on a ringing call, unlike every other
notification this sends. Dismissing a ring on the watch would mean rejecting
the call on the phone, and whether the watch reports a button press back has
never been established. A button that looks like it rejects a call and does
nothing is worse than no button.

### The watch refused it, and the reason was a missing label (2026-10-06)

The first version of the incoming-call notification carried no label, on the
reasoning that a Dismiss button on a ringing call would imply rejecting the
call and nobody has established whether the watch reports a press back.

The watch refused it. Three `Add` attempts timed out, and the log shows why
that is not a link problem:

```
15:09:04  incoming call on /calls/febda... from +358408264580
15:09:04  incoming call -> ancs 1: +358408264580 / Incoming call
15:09:04  whiteboard session ready
15:09:12  call ... is now null (0) - clearing the watch
15:09:15  sending failed: the watch did not answer the notification
...
15:10:03  whiteboard session ready
15:10:03  removal failed: the watch has no notification with that id   <- answered
15:10:04  the watch took notification 1577272528                       <- accepted
```

Within one session the watch **answered a removal and accepted an ordinary
notification**, and refused this one three times. The difference is the
payload, and the structural difference is the label array: with no labels its
offset points at the end of the pool - past the structure itself - with a
count of zero. The watch drops that in silence, which is how it drops every
malformed frame in this project's history.

So an incoming call carries one Dismiss label like everything else. The
original reasoning was also inconsistent: the uncertainty about whether a
press is reported back applies to every notification this sends, and all of
them carry the label.

Worth keeping from the same log: when the watch is already connected the
Whiteboard session opens inside the same second the call arrives, so the
ring is not going to be late for want of a link.

### With the label, it works - and the label is harmless (2026-10-06)

```
15:13:22  incoming call on /calls/5c00... from +358408264580
15:13:22  incoming call -> ancs 1: +358408264580 / Incoming call
15:13:22  whiteboard session ready
15:13:23  the watch took notification 1446502879
```

One second from the call arriving to the watch accepting it, with the watch
already connected. The Race alerts while the phone is ringing.

**The Dismiss button dismisses the notification, not the call.** Confirmed by
pressing it. That settles a question this document had twice called
unestablished - and it was the whole basis for leaving the label off in the
first place, so that reasoning was wrong twice over: once because the watch
refuses a label-less structure, and once because the button does not do the
thing it was feared to do.

### Still open: the ring is not cleared when the call ends

The run above has no `clearing the watch` line. The call did end - the missed
call notification arrived twenty seconds later - so
`VoiceCall.statusChanged` should have left `incoming` and
`CallMonitor::onStatusChanged` should have logged and emitted `callEnded`.

It did fire on the two earlier attempts, before the label fix, on both a
`null (0)` and a `disconnected (7)`:

```
15:09:12  call /calls/febda... is now null (0) - clearing the watch
15:10:03  call /calls/5afc... is now disconnected (7) - clearing the watch
```

and nothing in that path changed between those builds. So the cause is not
yet known, and the two candidates are worth writing down before they are
forgotten: either the end signal did not arrive at the daemon that time, or
`m_ringing` did not hold the path it arrived for. A full-bus capture across
one whole call - ring to hangup - would separate them, since it shows whether
the signal was on the bus at all.

Until that is settled an incoming call may stay on the watch after the caller
gives up, to be dismissed by hand. The missed-call notification that follows
is unaffected.
