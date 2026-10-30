# The notification daemon, packaged separately from the application on
# purpose.
#
# Observing other applications' notifications requires a D-Bus monitor
# connection, and inside Sailjail the filtering D-Bus proxy relays only the
# sandboxed application's own traffic. Rather than set
# Sandboxing=Disabled on harbour-suuntosync and lose Jolla Store
# eligibility for everything it does, this binary lives outside the sandbox
# in its own package, and the application stays inside.
#
# Built from the same source tree with the application switched off, from
# the repository root:
#
#   sfdk -c specfile=daemon/suuntosync-notifyd.spec build
#
# **Deliberately not in rpm/.** sfdk looks for a SPEC file in the package's
# rpm directory and refuses to build when it finds more than one - and Qt
# Creator passes it no specfile option, so a second SPEC file in there
# breaks the ordinary application build with "Multiple RPM SPEC files
# found". Keeping this one beside the service file it installs costs one
# option on the command line and keeps the IDE working.
#
# No harbour- prefix and no .desktop file: this is not a Store application
# and it has no user interface. It is a systemd user service.

Name:       suuntosync-notifyd
Summary:    Forwards phone notifications to a Suunto watch
Version:    1.6.2
Release:    1
License:    MIT
URL:        https://github.com/jarnose/harbour-suuntosync
Source0:    %{name}-%{version}.tar.bz2

# **Deliberately not Requires: harbour-suuntosync**, although the
# application is what owns the pairing and the database this reads.
#
# An RPM dependency here blocks the thing a user actually does: removing
# the application from the app grid fails outright, with no explanation,
# because an installed package requires it. `pkcon remove` works from a
# terminal only because it asks whether to take the dependant too, and the
# app grid has no way to ask. No arrangement of packages makes removing
# the application also remove this one - a subpackage would not either -
# so the choice is between blocking the removal and leaving this behind,
# and leaving it behind is plainly better.
#
# Recommends: was considered and rejected. zypper re-evaluates weak
# dependencies, so an application removed from the app grid could be
# pulled back in by this package on a later refresh - worse than the bug
# being fixed.
#
# Left behind, this daemon keeps working: the paired-watch row lives in
# the user's own data directory, which an RPM removal does not touch, so
# notifications still reach the watch. What it cannot do is be
# reconfigured. It is removed on its own:
#   devel-su pkcon remove suuntosync-notifyd
Requires:   systemd-user-session-targets

BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5DBus)
# Contacts, so a ringing call can show a name instead of a number. See the
# setgid line under -files for the privilege that makes the address book
# readable, and src/notify/contactlookup.h for why that trade was taken.
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(dbus-1)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  cmake

%description
Watches the phone's session bus for notifications and forwards them to a
Suunto watch over Bluetooth LE, using the pairing and the watch that
harbour-suuntosync already set up.

This is a separate package because it cannot run inside the Sailjail
sandbox: seeing another application's notification means becoming a D-Bus
monitor, and the sandbox's D-Bus proxy relays only the sandboxed
application's own messages. harbour-suuntosync itself stays sandboxed.

Only one process may hold a Whiteboard session with the watch at a time, so
the daemon yields the link whenever the application wants it and takes it
back afterwards. A notification arriving while the application is using the
watch is queued, and dropped if it waits too long.

%prep
%setup -q -n %{name}-%{version}

%build
%cmake -DBUILD_APP=OFF -DBUILD_DAEMON=ON
%make_build

%install
%make_install

%post
systemctl-user daemon-reload || :
systemctl-user enable %{name}.service || :
# restart, not "enable --now": --now does nothing to a service that is
# already running, so an upgrade left the old binary in place and the new
# one sitting unused on disk. restart starts it when stopped and replaces it
# when running, which is what both an install and an upgrade want.
systemctl-user restart %{name}.service || :

%preun
if [ "$1" = 0 ]; then
    systemctl-user disable --now %{name}.service || :
fi

%postun
systemctl-user daemon-reload || :

%files
%defattr(-,root,root,-)
# setgid privileged, which is what lets the daemon read the phone's address
# book and put a name on a ringing call. The store is at
# ~/.local/share/system/privileged/Contacts, mode drwxrwx--- and group
# privileged, and defaultuser is in neither - so without this a call shows
# only its number.
#
# Copied from harbour-amazfish, which installs its own daemon exactly this
# way for exactly this reason. It is narrower than adding the user to the
# group, since only this binary gains the access, but it is a real
# privilege: setgid privileged can read every privileged store and not only
# contacts. It was taken because this daemon already observes every
# notification on the session bus with its full content - every message,
# every mail - so a list of names is not a meaningful increase in what it
# could learn. See src/notify/contactlookup.h.
%attr(2755,root,privileged) %{_bindir}/%{name}
%{_userunitdir}/%{name}.service
