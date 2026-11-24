# the plasma plugin needs the plasma-nm sources of the installed version,
# tools/build-rpm.sh passes it in
%{!?plasmanm_version: %global plasmanm_version 6.8.3}

Name:           NetworkManager-tailscale
Version:        0.1.6
Release:        1%{?dist}
Summary:        NetworkManager VPN plugin for Tailscale
License:        GPL-2.0-or-later
URL:            https://github.com/Amphero/networkmanager-tailscale
Source0:        %{url}/archive/v%{version}.tar.gz#/networkmanager-tailscale-%{version}.tar.gz
Source1:        https://invent.kde.org/plasma/plasma-nm/-/archive/v%{plasmanm_version}/plasma-nm-v%{plasmanm_version}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  pkgconf
BuildRequires:  systemd-rpm-macros
BuildRequires:  NetworkManager-libnm-devel
BuildRequires:  gtk3-devel
BuildRequires:  gtk4-devel
BuildRequires:  json-glib-devel
BuildRequires:  libcurl-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kwidgetsaddons-devel
BuildRequires:  kf6-networkmanager-qt-devel
BuildRequires:  plasma-nm
# smoke test
BuildRequires:  dbus-daemon
BuildRequires:  glib2
BuildRequires:  curl
BuildRequires:  python3

Requires:       NetworkManager
Requires:       tailscale

%description
Connects and disconnects Tailscale through NetworkManager, using the
tailscaled LocalAPI. This package has the VPN service and the
NetworkManager plugin, enough for nmcli and the network applets.

%package gnome
Summary:        NetworkManager VPN plugin for Tailscale (GTK editors)
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description gnome
Connection editor for GNOME Settings (GTK4) and nm-connection-editor
(GTK3).

%package plasma
Summary:        NetworkManager VPN plugin for Tailscale (Plasma editor)
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       (plasma-nm >= %{plasmanm_version} with plasma-nm < %{plasmanm_version}.1)

%description plasma
Connection editor for KDE Plasma. Built against the private headers of
one plasma-nm version, so it has to be rebuilt when plasma-nm changes.

%prep
%autosetup -n networkmanager-tailscale-%{version}
mkdir -p reference
tar -xzf %{SOURCE1} -C reference
mv reference/plasma-nm-v%{plasmanm_version} reference/plasma-nm

%build
%set_build_flags
%make_build -f build.mk all NM_LIBEXECDIR=%{_libexecdir}

%install
%make_install -f build.mk NM_LIBEXECDIR=%{_libexecdir}

%check
make -f build.mk check NM_LIBEXECDIR=%{_libexecdir}

%files
%license LICENSE
%doc README.md
%{_libexecdir}/nm-tailscale-service
%{_libexecdir}/nm-tailscale-shutdown
%{_libdir}/NetworkManager/libnm-vpn-plugin-tailscale.so
%{_prefix}/lib/NetworkManager/VPN/nm-tailscale-service.name
%{_prefix}/lib/NetworkManager/dispatcher.d/90-nm-tailscale
%dir %{_unitdir}/tailscaled.service.d
%{_unitdir}/tailscaled.service.d/nm-tailscale.conf
%{_datadir}/dbus-1/system.d/nm-tailscale-service.conf

%files gnome
%{_libdir}/NetworkManager/libnm-vpn-plugin-tailscale-editor.so
%{_libdir}/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so

%files plasma
%{_libdir}/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so

%changelog
* Mon Nov 23 2026 Amphero <301399868+Amphero@users.noreply.github.com> - 0.1.6-1
- RPM packages, GTK3 editor
