# NetworkManager-Tailscale

NetworkManager-VPN-Plugin für [Tailscale](https://tailscale.com): Tailscale
als VPN-Verbindung in GNOME (Einstellungen → Netzwerk → VPN), KDE Plasma
(Systemeinstellungen und Netzwerk-Applet) und per `nmcli` verwalten.

Der Service-Daemon ist ein dünner Wrapper um die LocalAPI von `tailscaled`
(`WantRunning` ein/aus, entspricht `tailscale up`/`down`) — Interface, Routen
und DNS verwaltet tailscaled selbst. Features: optionaler Auth-Key
(NM-Secret), Browser-Login (SSO) inklusive automatischer Operator-Einrichtung
per polkit, Exit-Node-Auswahl, Accept-DNS/Accept-Routes, Vorbelegung aus den
aktuellen tailscaled-Einstellungen und Status-Sync (externes `tailscale down`
beendet die NM-Verbindung).

## Komponenten

| Verzeichnis | Inhalt |
|---|---|
| `src/` | VPN-Service-Daemon (C, libnm `NMVpnServicePlugin`) |
| `properties/` | Editor-Plugin-Core (libnm) + GTK4-Editor für GNOME |
| `plasma/` | Natives plasma-nm-Plugin (Qt/C++) + Metadaten-Stub als Fallback |
| `shared/` | Gemeinsamer Code: Schlüssel-Definitionen, LocalAPI-HTTP-Client |
| `tests/` | D-Bus-Smoke-Test mit gemocktem tailscaled |

## Bauen

Alles läuft in rootless Podman-Containern, auf dem Host wird nichts
installiert:

```sh
make container   # Build-Image erstellen (Arch-Basis)
make build       # kompilieren → ./dist/
make check       # Smoke-Test (privater D-Bus + LocalAPI-Mock)
```

Für das native Plasma-Plugin wird der plasma-nm-Quellcode passend zur
installierten Version benötigt (private Header, kein öffentliches API):

```sh
git clone --depth 1 --branch v$(pacman -Q plasma-nm | cut -d' ' -f2 | cut -d- -f1) \
    https://invent.kde.org/plasma/plasma-nm.git reference/plasma-nm
```

Nach einem plasma-nm-Update: Checkout aktualisieren und neu bauen. Alle
anderen Komponenten hängen nur an stabilen APIs (libnm, GTK4, Qt-Metadaten).

## Installation

Siehe [INSTALL.md](INSTALL.md) — Zielpfade, GNOME-/KDE-Nutzung,
Fehlersuche und vollständige Deinstallation.

## Lizenz

GPL-2.0-or-later (siehe SPDX-Header in den Quelldateien).
