Name:       harbour-suuntosync

Summary:    Suunto Sync
Version:    0.1
Release:    1
License:    MIT
URL:        http://example.org/
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
Syncs workout history and stats from a Suunto smartwatch (Suunto Race,
Suunto 9 Baro) over Bluetooth LE and via the Suunto cloud account, and
forwards phone notifications to the watch. Cloud account tokens are stored
in the Sailfish Secrets vault (block-encrypted, device-lock protected),
never in plain text on disk.

%prep
%setup -q -n %{name}-%{version}

%build

%cmake

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
