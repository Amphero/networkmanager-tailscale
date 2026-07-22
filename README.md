# NetworkManager-Tailscale

NetworkManager-VPN-Plugin für [Tailscale](https://tailscale.com). Steuert
tailscaled über dessen LocalAPI (`WantRunning` ≙ `tailscale up`/`down`);
Interface, Routen und DNS verwaltet tailscaled selbst.

- Verbinden/Trennen über GNOME-Einstellungen, KDE Plasma (Systemeinstellungen
  und Netzwerk-Applet) oder `nmcli`
- Geräteregistrierung per Auth-Key (als NM-Secret gespeichert) oder
  Browser-Login; fehlende Operator-Rechte werden per polkit eingerichtet
- Exit-Node-Auswahl, Accept-DNS, Accept-Routes; neue Verbindungen übernehmen
  die aktuellen tailscaled-Einstellungen als Vorbelegung
- Externes `tailscale down`/Logout trennt die NM-Verbindung automatisch

| Verzeichnis | Inhalt |
|---|---|
| `src/` | VPN-Service-Daemon (C, libnm `NMVpnServicePlugin`) |
| `properties/` | Editor-Plugin-Core (libnm) + GTK4-Editor für GNOME |
| `plasma/` | plasma-nm-Plugin (Qt/C++) + Metadaten-Stub als Fallback |
| `shared/` | Schlüssel-Definitionen, LocalAPI-HTTP-Client |
| `tests/` | D-Bus-Smoke-Test mit gemocktem tailscaled |

## Bauen

Builds laufen in rootless Podman-Containern (Arch-Basis), Artefakte landen
in `./dist/`:

```sh
make container   # Build-Image erstellen
make build
make check       # Smoke-Test: privater D-Bus + LocalAPI-Mock
```

Das plasma-nm-Plugin baut gegen die privaten Header der installierten
plasma-nm-Version und muss nach deren Updates neu gebaut werden:

```sh
git clone --depth 1 --branch v$(pacman -Q plasma-nm | cut -d' ' -f2 | cut -d- -f1) \
    https://invent.kde.org/plasma/plasma-nm.git reference/plasma-nm
```

## Installation

Voraussetzung: `tailscale` ist installiert und `tailscaled` läuft
(`systemctl enable --now tailscaled`).

```sh
sudo install -m755 dist/nm-tailscale-service                       /usr/lib/NetworkManager/nm-tailscale-service
sudo install -m755 dist/libnm-vpn-plugin-tailscale.so              /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so
sudo install -m755 dist/libnm-gtk4-vpn-plugin-tailscale-editor.so  /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so
sudo install -Dm644 dist/nm-tailscale-service.name                 /usr/lib/NetworkManager/VPN/nm-tailscale-service.name
sudo install -m644 dist/nm-tailscale-service.conf                  /usr/share/dbus-1/system.d/nm-tailscale-service.conf
# nur bei KDE Plasma:
sudo install -Dm755 dist/plasmanetworkmanagement_tailscaleui.so    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so

sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

Lädt das plasma-nm-Plugin nach einem Plasma-Update nicht mehr, den Stub an
denselben Pfad installieren — Verbinden über das Applet funktioniert damit
weiter, nur der Einstellungs-Dialog entfällt:

```sh
sudo install -m755 dist/plasmanetworkmanagement_tailscaleui-stub.so \
    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
```

## Nutzung

**GNOME:** Einstellungen → Netzwerk → VPN → „+“ → Tailscale.

**KDE Plasma:** Systemeinstellungen → Netzwerk → Verbindungen → „+“ → VPN →
Tailscale; schalten über das Netzwerk-Applet.

**nmcli:**

```sh
nmcli connection add type vpn con-name Tailscale vpn-type tailscale
nmcli connection modify Tailscale vpn.secrets "auth-key=tskey-auth-…"   # optional
nmcli connection modify Tailscale +vpn.data "accept-dns=no"             # optional
nmcli connection modify Tailscale +vpn.data "accept-routes=yes"         # optional
nmcli connection modify Tailscale +vpn.data "exit-node=100.x.y.z"       # optional
nmcli connection up Tailscale
```

Der Auth-Key ist nur nötig, solange tailscaled ausgeloggt ist; alternativ
einmalig `tailscale login` oder der Browser-Login-Button im Editor. Ohne die
`vpn.data`-Einträge bleiben die entsprechenden tailscaled-Einstellungen
unangetastet.

## Fehlersuche

```sh
journalctl -u NetworkManager -f      # Meldungen erscheinen als nm-tailscale-service
```

Bleibt die Exit-Node-Liste im Editor leer, fehlen Leserechte auf die
LocalAPI: `sudo tailscale set --operator=$USER`.

## Deinstallation

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

## Lizenz

GPL-2.0-or-later
