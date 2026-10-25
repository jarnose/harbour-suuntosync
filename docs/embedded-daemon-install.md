# Installing the notification daemon from inside the application

## The problem this solves

Notifications need a second package. `suuntosync-notifyd` cannot be
sandboxed - seeing another application's notification means becoming a
D-Bus monitor, and Sailjail's proxy relays only the sandboxed
application's own traffic (`docs/notifications.md` has the full account).
A package that cannot be sandboxed cannot be in Jolla's Store, so an
application installed from the Store could never offer notifications at
all. The user would have to find an RPM on a web page, enable sideloading
and install it by hand.

## How harbour-situations2application does it

Situations is in Jolla's own Store, is sandboxed
(`Permissions=Documents;Downloads;Internet;Location`), and has an in-app
button that installs a daemon. Read off the copy installed on the test
phone - measured, not inferred:

- **There is no second package in the Store.** `rpm -qa` shows only
  `harbour-situations2application`. `situations-daemon` is not installed at
  all, and Situations runs anyway: its engine is a child process of the
  sandboxed application, `harbour-situations2application server`, under the
  same firejail. The daemon is an *extra*, for what the sandbox cannot do.
- **The daemon's RPM is inside the application binary.** At byte 146468 of
  a 595064-byte binary sits an RPM lead, `ed ab ee db 03 00 00 00`:
  `rpm v3.0, type 0 (binary), archnum 19, name situations-daemon-1.0.4-0`.
  The four bytes before it are a Qt resource's big-endian length, 31998.
  The resource path is in the binary in plain sight:
  `:/rpms/situations-daemon.rpm`.
- **The application does not install it.** The only relevant symbol it
  links is `_ZN16QDesktopServices7openUrlERK4QUrl`. No PackageKit, no
  `pkcon`, no package D-Bus interface. It writes the RPM out and asks the
  *system* to open it; the system's installer asks the user. That is why a
  sandboxed application is allowed to do this - it opens a file.
- **Sideloading has to be allowed.** Its own string: "Background service
  launcher needs to be installed. Please first allow installation of
  uns…". Jolla's page for it is
  `/usr/share/jolla-settings/pages/sideloading/`.
- The embedded package's own `%post` runs
  `systemctl --no-reload preset situations-daemon.service` and
  `try-restart` - a **system** unit, where this project's daemon is a user
  unit.

One thing not measured: which directory it writes the RPM to. The binary
holds only the `/situations-daemon.rpm` suffix, so the directory comes from
a `QStandardPaths` call at runtime. `Downloads` being in its permission
list is strong evidence, and the reason is structural - the installer runs
outside the jail and cannot read the application's private cache - but
measuring it would have meant installing software on somebody's phone.

## What this project does

The same thing, with the directory question answered the way the structure
demands rather than by copying a guess.

- `DAEMON_RPM` in `CMakeLists.txt` embeds a given RPM as a Qt resource at
  `:/rpms/suuntosync-notifyd.rpm`, and defines
  `SUUNTOSYNC_EMBEDDED_DAEMON_RPM`. It checks the file's lead magic at
  configure time, because a path typo would otherwise produce an
  application that offers an install and hands the installer a tarball -
  discoverable only on a phone.
- Without it the application builds exactly as before and the Settings
  section is **absent**, not inert. The daemon's RPM is the product of a
  separate build of this same tree, so a hard requirement would make the
  application unbuildable alone and the ordering circular.
- The option is matched by a conventional path,
  `daemon/embed/suuntosync-notifyd.rpm`, because the package build never
  reaches the cmake command line: the spec calls `%cmake`, and threading an
  option through it would need an rpm macro, an sfdk argument and a CI
  incantation that all have to agree. Dropping the file there needs none of
  it. Gitignored - it is a build product passing through.
- `AppController::installNotifyService()` writes the resource to
  `QStandardPaths::DownloadLocation` under a fixed name and calls
  `QDesktopServices::openUrl()`. Not the application's own cache: the
  installer runs outside the sandbox. Hence `Downloads` in the permission
  list, which is a visible change to what the application asks for.
- `openUrl()` returning true means the request was accepted, not that
  anything was installed, so the user is told to confirm on screen - and
  told about the untrusted-software setting *before* the attempt, because
  the refusal does not explain itself.

### Knowing whether it is already there

`notifyServiceSeen` is the daemon's own log file existing in the cache
directory both halves share. Deliberately not "is it installed": from
inside the sandbox there is no way to ask. Firejail runs the application
with `--private-bin`, so `/usr/bin` holds one file and `stat()` on the
daemon's binary means nothing, and `~/.config/systemd` is outside the
whitelist. The log proves the daemon ran; its absence does not quite prove
the daemon is missing, so it only decides whether to offer the install,
never to refuse it.

The install happens in another process and reports nothing back, so the
Settings page re-reads this whenever it becomes active - which is when the
user is back from the installer.

## Build order

CI builds the daemon's package **first**, stages it at
`daemon/embed/suuntosync-notifyd.rpm`, then builds the application. Built
the other way round, as it was, the application would come out silently
without the installer: a build that succeeds and quietly lacks a feature.
The staging step fails if the glob matches no package or more than one, and
a later step unpacks the finished application RPM and looks for the lead
magic in the binary, so the check is on the artifact rather than on a
configure-time message having appeared.

The embedded package is architecture-specific, and the matrix gives one per
architecture, so the aarch64 application carries the aarch64 daemon by
construction.

## What is still unproven

Everything above builds and the embedding is verified in the artifact. Not
yet confirmed on a phone:

- that Sailfish's installer accepts a file opened from
  `~/Downloads` by a sandboxed application;
- that the daemon's `%post`, which calls `systemctl-user`, behaves the same
  when PackageKit runs it as it does under `pkcon install-local`;
- whether Jolla's Store review accepts an embedded RPM. Situations being in
  the Store doing exactly this is the evidence that matters, but it is not
  a guarantee for another application.
