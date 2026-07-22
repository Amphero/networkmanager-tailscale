# NetworkManager-Tailscale

NetworkManager VPN plugin for [Tailscale](https://tailscale.com). Drives
tailscaled through its LocalAPI (`WantRunning` ≙ `tailscale up`/`down`);
tailscaled itself manages the interface, routes and DNS.

- Connect/disconnect from GNOME Settings, KDE Plasma (System Settings and
  the network applet) or `nmcli`
- Device registration via auth key (stored as an NM secret) or browser
  login; missing operator rights are granted through polkit
- Exit node selection, accept-DNS, accept-routes; new connections are
  pre-filled with the current tailscaled preferences
- An external `tailscale down`/logout disconnects the NM connection
  automatically

| Directory | Contents |
|---|---|
| `src/` | VPN service daemon (C, libnm `NMVpnServicePlugin`) |
| `properties/` | Editor plugin core (libnm) + GTK4 editor for GNOME |
| `plasma/` | plasma-nm plugin (Qt/C++) + metadata stub as fallback |
| `shared/` | Key definitions, LocalAPI HTTP client |
| `tests/` | D-Bus smoke test against a mocked tailscaled |

## Building

Builds run in rootless Podman containers (Arch based); artifacts end up in
`./dist/`:

```sh
make container   # create the build image
make build
make check       # smoke test: private D-Bus + LocalAPI mock
```

The plasma-nm plugin builds against the private headers of the installed
plasma-nm version and must be rebuilt after its updates:

```sh
git clone --depth 1 --branch v$(pacman -Q plasma-nm | cut -d' ' -f2 | cut -d- -f1) \
    https://invent.kde.org/plasma/plasma-nm.git reference/plasma-nm
```

## Installation

Prerequisite: `tailscale` is installed and `tailscaled` is running
(`systemctl enable --now tailscaled`).

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

If the plasma-nm plugin no longer loads after a Plasma update, install the
stub to the same path — connecting through the applet keeps working, only
the settings dialog is unavailable:

```sh
sudo install -m755 dist/plasmanetworkmanagement_tailscaleui-stub.so \
    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
```

## Usage

**GNOME:** Settings → Network → VPN → “+” → Tailscale.

**KDE Plasma:** System Settings → Network → Connections → “+” → VPN →
Tailscale; toggle via the network applet.

**nmcli:**

```sh
nmcli connection add type vpn con-name Tailscale vpn-type tailscale
nmcli connection modify Tailscale vpn.secrets "auth-key=tskey-auth-…"   # optional
nmcli connection modify Tailscale +vpn.data "accept-dns=no"             # optional
nmcli connection modify Tailscale +vpn.data "accept-routes=yes"         # optional
nmcli connection modify Tailscale +vpn.data "exit-node=100.x.y.z"       # optional
nmcli connection up Tailscale
```

The auth key is only needed while tailscaled is logged out; alternatively
run `tailscale login` once or use the browser login button in the editor.
Without the `vpn.data` entries the corresponding tailscaled preferences are
left untouched.

## Troubleshooting

```sh
journalctl -u NetworkManager -f      # messages appear as nm-tailscale-service
```

If the exit node list in the editor stays empty, the LocalAPI is not
readable: `sudo tailscale set --operator=$USER`.

## Uninstall

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

GPL-2.0-or-later
