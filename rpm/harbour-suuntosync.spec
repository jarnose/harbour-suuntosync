Name:       harbour-suuntosync

Summary:    Suunto Sync
Version:    1.2
Release:    1
License:    MIT
URL:        https://github.com/jarnose/harbour-suuntosync
Source0:    %{name}-%{version}.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(sailfishsecrets)
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Concurrent)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5DBus)
# Raw deflate for the sml.zip upload part - see src/cloud/zipwriter.cpp.
BuildRequires:  pkgconfig(zlib)
# No pkgconfig(Qt5Bluetooth) - confirmed by a real build attempt against
# this target (zypper found no provider by name or capability) that
# Sailfish OS's Qt5 distribution doesn't include QtConnectivity/QtBluetooth
# at all. BLE (Phase 5+) goes through BlueZ 5's D-Bus API instead, via the
# Qt5DBus module above.
# No pkgconfig(Qt5LinguistTools) virtual exists on Sailfish OS - CMake's
# find_package(Qt5 COMPONENTS LinguistTools) locates it via the CMake
# package config instead, which comes from this plain package.
BuildRequires:  qt5-qttools-linguist
BuildRequires:  desktop-file-utils
BuildRequires:  cmake

%description
Syncs workouts, routes, sleep, recovery and daily activity with a Suunto
watch (Suunto Race, Suunto 9 Baro) over Bluetooth LE and with the Suunto
cloud, in either direction, and sends the watch the GPS assist data that
makes it find satellites in seconds. Cloud account tokens are stored in the
Sailfish Secrets vault (block-encrypted, device-lock protected), never in
plain text on disk.

Forwarding the phone's own notifications to the watch is deliberately NOT
part of this package: observing other applications' notifications requires
leaving the Sailjail sandbox, which this package does not do. That feature
lives in a separate, optional daemon package.

%prep
%setup -q -n %{name}-%{version}

%build

# Both options, explicitly. CMake's option() honours a cached value, and
# sfdk configures in the source directory - so building the daemon with
# BUILD_APP off leaves the cache saying so, and a later application build
# would quietly produce nothing and then fail at install time with
# "No such file or directory" for its own .desktop file.
# (No per-cent signs in this comment: rpm parses macros inside comments too,
# and a section name in one makes it see a second section.)
%cmake -DBUILD_APP=ON -DBUILD_DAEMON=OFF

%make_build


%install
%make_install


desktop-file-install --delete-original         --dir %{buildroot}%{_datadir}/applications                %{buildroot}%{_datadir}/applications/*.desktop

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
