# NetworkManager-Tailscale

A NetworkManager VPN plugin for [Tailscale](https://tailscale.com).

It drives tailscaled over the LocalAPI, so connecting comes down to
`tailscale up` and disconnecting to `tailscale down`. The actual networking
(interface, routes, DNS) stays with tailscaled, NetworkManager never touches
it.

What you get:

- Connect and disconnect from GNOME Settings, KDE Plasma (System Settings
  and the network applet), nm-applet and nm-connection-editor (XFCE, MATE
  and other desktops) or nmcli
- Device registration with an auth key or through browser login. The auth
  key is stored as an NM secret. If your user is missing operator rights,
  polkit asks and grants them
- Exit node, accept-dns and accept-routes settings. New connections start
  out with whatever tailscaled is currently set to
- Running `tailscale down` or logging out outside of NM also disconnects
  the NM connection
- After a reboot tailscale stays down until you connect through NM. If
  tailscaled still comes up on its own (power loss, tailscaled crash),
  the NM connection is brought up as soon as a network is connected, so
  the applet shows it

Code layout: `src/` has the VPN service daemon (C, libnm), `properties/`
the connection editor (libnm core plus a UI built for GTK3 and GTK4),
`plasma/` the plasma-nm plugin (Qt/C++) and a metadata stub as fallback,
`shared/` common key definitions and the LocalAPI HTTP client, `tests/` a
D-Bus smoke test against a mocked tailscaled and an editor test,
`packaging/` the Arch, Debian and RPM packaging, `tools/` helper scripts.

## Building

Everything builds in a rootless Podman container (Arch based), the results
land in `./dist/`:

```sh
make container   # build the image once
make build
make check       # smoke test (private D-Bus + LocalAPI mock) and editor test
```

The plasma-nm plugin needs private plasma-nm headers matching the installed
version, so it has to be rebuilt whenever plasma-nm updates. Get the
matching sources with:

```sh
tools/fetch-plasma-nm.sh   # works with pacman, dpkg and rpm
```

## Packages

Prebuilt x86_64 packages are attached to the
[releases](https://github.com/Amphero/networkmanager-tailscale/releases):

| | Daemon | GTK editors | Plasma editor |
|---|---|---|---|
| Arch | `networkmanager-tailscale` | included, needs `gtk3`/`gtk4` | `networkmanager-tailscale-plasma` |
| Debian 13, Ubuntu 26.04 | `network-manager-tailscale` | `network-manager-tailscale-gnome` | `network-manager-tailscale-plasma` |
| Fedora | `NetworkManager-tailscale` | `NetworkManager-tailscale-gnome` | `NetworkManager-tailscale-plasma` |

On Debian, Ubuntu and Fedora `tailscale` comes from the
[Tailscale repos](https://tailscale.com/download/linux). You need the
daemon and the editor for your desktop, the lines below install all
packages of a release:

```sh
sudo pacman -U ./networkmanager-tailscale-*.pkg.tar.zst      # Arch
sudo apt install ./network-manager-tailscale*_amd64.deb      # Debian, Ubuntu
sudo dnf install ./NetworkManager-tailscale*.rpm             # Fedora
sudo systemctl daemon-reload && sudo systemctl restart NetworkManager
```

The plasma package only works with the plasma-nm version it was built
against. On Arch a pacman hook prints a reminder when plasma-nm gets
updated, the deb and rpm packages depend on that exact version.

To build the packages yourself:

```sh
make pkg   # Arch, from the committed state (HEAD)
make deb   # Debian 13 and Ubuntu 26.04, from the working tree
make rpm   # Fedora, from the working tree
```

The PKGBUILD in `packaging/` also works standalone. It fetches the release
tarball for tag `v<pkgver>` and verifies pinned checksums. `make pkg`
builds a local HEAD snapshot instead and skips the checksum check.

## Manual installation

You need `tailscale` installed and `tailscaled` running
(`systemctl enable --now tailscaled`). The container is Arch based, on
other distributions build natively with `make -f build.mk all`, on
Fedora with `NM_LIBEXECDIR=/usr/libexec` for build and install. Then:

```sh
sudo make -f build.mk install-core
sudo make -f build.mk install-plasma   # KDE Plasma only
sudo systemctl daemon-reload
sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

If the plasma-nm plugin stops loading after a Plasma update, install the
stub to the same path until you get around to rebuilding. Connecting
through the applet keeps working, only the settings dialog is gone:

```sh
sudo install -m755 dist/plasmanetworkmanagement_tailscaleui-stub.so \
    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
```

## Usage

GNOME: Settings -> Network -> VPN -> "+" -> Tailscale.

KDE Plasma: System Settings -> Network -> Connections -> "+" -> VPN ->
Tailscale. Toggle via the network applet.

XFCE, MATE and other desktops with nm-applet: Network Connections
(nm-connection-editor) -> "+" -> Tailscale. Toggle via nm-applet.

nmcli:

```sh
nmcli connection add type vpn con-name Tailscale vpn-type tailscale
nmcli connection modify Tailscale vpn.secrets "auth-key=<auth-key>"     # optional
nmcli connection modify Tailscale +vpn.data "accept-dns=no"             # optional
nmcli connection modify Tailscale +vpn.data "accept-routes=yes"         # optional
nmcli connection modify Tailscale +vpn.data "exit-node=100.x.y.z"       # optional
nmcli connection up Tailscale
```

The auth key is only needed while tailscaled is logged out. You can also
run `tailscale login` once, or use the browser login button in the editor.
Any `vpn.data` entry you leave out keeps the corresponding tailscaled
preference as it is.

## Troubleshooting

The service logs to the NetworkManager journal as `nm-tailscale-service`:

```sh
journalctl -u NetworkManager -f
```

An empty exit node list in the editor means your user cannot read the
LocalAPI. Fix: `sudo tailscale set --operator=$USER`.

## Uninstall

Packages:

```sh
sudo pacman -R networkmanager-tailscale-plasma networkmanager-tailscale   # Arch
sudo apt remove 'network-manager-tailscale*'                              # Debian, Ubuntu
sudo dnf remove 'NetworkManager-tailscale*'                               # Fedora
```

Manual installs on Arch:

```sh
nmcli connection delete Tailscale
sudo rm -f /usr/lib/NetworkManager/nm-tailscale-service \
           /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so \
           /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale-editor.so \
           /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so \
           /usr/lib/NetworkManager/VPN/nm-tailscale-service.name \
           /usr/share/dbus-1/system.d/nm-tailscale-service.conf \
           /usr/lib/NetworkManager/dispatcher.d/90-nm-tailscale \
           /usr/lib/NetworkManager/nm-tailscale-shutdown \
           /usr/lib/systemd/system/tailscaled.service.d/nm-tailscale.conf \
           /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
sudo systemctl daemon-reload
sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
