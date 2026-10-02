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
Version:    0.1
Release:    1
License:    MIT
URL:        https://github.com/jarnose/harbour-suuntosync
Source0:    %{name}-%{version}.tar.bz2

# The application owns the pairing and the database this reads; without it
# there is no watch to send to and nothing to configure.
Requires:   harbour-suuntosync
Requires:   systemd-user-session-targets

BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5DBus)
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
systemctl-user enable --now %{name}.service || :

%preun
if [ "$1" = 0 ]; then
    systemctl-user disable --now %{name}.service || :
fi

%postun
systemctl-user daemon-reload || :

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_userunitdir}/%{name}.service
