# NetworkManager-Tailscale

A NetworkManager VPN plugin for [Tailscale](https://tailscale.com).

It drives tailscaled over the LocalAPI, so connecting comes down to
`tailscale up` and disconnecting to `tailscale down`. The actual networking
(interface, routes, DNS) stays with tailscaled, NetworkManager never touches
it.

What you get:

- Connect and disconnect from GNOME Settings, KDE Plasma (System Settings
  and the network applet) or nmcli
- Device registration with an auth key or through browser login. The auth
  key is stored as an NM secret. If your user is missing operator rights,
  polkit asks and grants them
- Exit node, accept-dns and accept-routes settings. New connections start
  out with whatever tailscaled is currently set to
- Running `tailscale down` or logging out outside of NM also disconnects
  the NM connection

Code layout: `src/` has the VPN service daemon (C, libnm), `properties/`
the connection editor (libnm core plus a GTK4 UI), `plasma/` the plasma-nm
plugin (Qt/C++) and a metadata stub as fallback, `shared/` common key
definitions and the LocalAPI HTTP client, `tests/` a D-Bus smoke test
against a mocked tailscaled.

## Building

Everything builds in a rootless Podman container (Arch based), the results
land in `./dist/`:

```sh
make container   # build the image once
make build
make check       # smoke test: private D-Bus + LocalAPI mock
```

The plasma-nm plugin needs private plasma-nm headers matching the installed
version, so it has to be rebuilt whenever plasma-nm updates. Get the
matching sources with:

```sh
git clone --depth 1 --branch v$(pacman -Q plasma-nm | cut -d' ' -f2 | cut -d- -f1) \
    https://invent.kde.org/plasma/plasma-nm.git reference/plasma-nm
```

## Arch packages

Prebuilt x86_64 packages are attached to the
[releases](https://github.com/Amphero/networkmanager-tailscale/releases).
Install with `sudo pacman -U <package>` and restart NetworkManager,
dependencies come from the official repos.

To build the packages yourself, `make pkg` builds both from the committed
state (HEAD) into `./dist/`:

```sh
make pkg
sudo pacman -U dist/networkmanager-tailscale-*-x86_64.pkg.tar.zst          # daemon + GNOME
sudo pacman -U dist/networkmanager-tailscale-plasma-*-x86_64.pkg.tar.zst   # KDE only
sudo systemctl reload dbus && sudo systemctl restart NetworkManager
```

Remove with `sudo pacman -R networkmanager-tailscale-plasma networkmanager-tailscale`.
The plasma package installs a pacman hook that prints a rebuild reminder
when plasma-nm gets updated.

The PKGBUILD in `packaging/` also works standalone. It fetches the release
tarball for tag `v<pkgver>` and verifies pinned checksums. `make pkg`
builds a local HEAD snapshot instead and skips the checksum check.

## Manual installation

You need `tailscale` installed and `tailscaled` running
(`systemctl enable --now tailscaled`). Then:

```sh
sudo install -m755 dist/nm-tailscale-service                       /usr/lib/NetworkManager/nm-tailscale-service
sudo install -m755 dist/libnm-vpn-plugin-tailscale.so              /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so
sudo install -m755 dist/libnm-gtk4-vpn-plugin-tailscale-editor.so  /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so
sudo install -Dm644 dist/nm-tailscale-service.name                 /usr/lib/NetworkManager/VPN/nm-tailscale-service.name
sudo install -m644 dist/nm-tailscale-service.conf                  /usr/share/dbus-1/system.d/nm-tailscale-service.conf
# KDE Plasma only:
sudo install -Dm755 dist/plasmanetworkmanagement_tailscaleui.so    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so

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

nmcli:

```sh
nmcli connection add type vpn con-name Tailscale vpn-type tailscale
nmcli connection modify Tailscale vpn.secrets "auth-key=tskey-auth-…"   # optional
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

Package installs: `sudo pacman -R networkmanager-tailscale-plasma
networkmanager-tailscale`. Manual installs:

```sh
nmcli connection delete Tailscale
sudo rm -f /usr/lib/NetworkManager/nm-tailscale-service \
           /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so \
           /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so \
           /usr/lib/NetworkManager/VPN/nm-tailscale-service.name \
           /usr/share/dbus-1/system.d/nm-tailscale-service.conf \
           /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).
