# Installation: NetworkManager-Tailscale-Plugin (Arch Linux)

Alle Artefakte liegen nach `make container && make build` in `./dist/`:

| Datei | Zielpfad | Zweck |
|---|---|---|
| `nm-tailscale-service` | `/usr/lib/NetworkManager/nm-tailscale-service` | VPN-Service-Daemon (von NM gestartet) |
| `libnm-vpn-plugin-tailscale.so` | `/usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so` | Editor-Plugin-Core (von libnm/libnma geladen) |
| `libnm-gtk4-vpn-plugin-tailscale-editor.so` | `/usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so` | GTK4-Editor für GNOME-Einstellungen |
| `nm-tailscale-service.name` | `/usr/lib/NetworkManager/VPN/nm-tailscale-service.name` | Service-Definition (macht den VPN-Typ bekannt) |
| `nm-tailscale-service.conf` | `/usr/share/dbus-1/system.d/nm-tailscale-service.conf` | D-Bus-Policy (erlaubt root den Busnamen) |
| `plasmanetworkmanagement_tailscaleui.so` | `/usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so` | Natives plasma-nm-Plugin: Anlegen/Bearbeiten/Verbinden in den Plasma-Systemeinstellungen (nur KDE) |
| `plasmanetworkmanagement_tailscaleui-stub.so` | *(Fallback, gleicher Zielpfad/-name)* | Metadaten-Stub: nur Applet-Verbinden, falls das native Plugin nach einem Plasma-Update nicht mehr lädt |

Laufzeit-Abhängigkeiten (auf Arch meist ohnehin vorhanden): `libnm`, `libcurl`,
`json-glib`, `gtk4` sowie natürlich `tailscale` selbst.

## Voraussetzung: tailscaled

```sh
sudo pacman -S --needed tailscale
sudo systemctl enable --now tailscaled
sudo tailscale set --operator=$USER   # optional, empfohlen: erspart den
                                      # polkit-Prompt beim Browser-Login-Button
```

Der Daemon ist nur ein dünner Wrapper um die LocalAPI von tailscaled
(`/var/run/tailscale/tailscaled.sock`) — tailscaled muss also laufen.

## Installation

Aus dem Projektverzeichnis:

```sh
sudo install -m755 dist/nm-tailscale-service                        /usr/lib/NetworkManager/nm-tailscale-service
sudo install -m755 dist/libnm-vpn-plugin-tailscale.so               /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so
sudo install -m755 dist/libnm-gtk4-vpn-plugin-tailscale-editor.so   /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so
sudo install -m644 dist/nm-tailscale-service.name                   /usr/lib/NetworkManager/VPN/nm-tailscale-service.name
sudo install -m644 dist/nm-tailscale-service.conf                   /usr/share/dbus-1/system.d/nm-tailscale-service.conf
# nur bei KDE Plasma:
sudo install -Dm755 dist/plasmanetworkmanagement_tailscaleui.so     /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
```

Danach D-Bus-Policy und NetworkManager neu laden:

```sh
sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

Ein Neustart der Desktop-Sitzung ist nicht nötig; `gnome-control-center` liest
die `.name`-Dateien beim nächsten Start neu ein (ggf. laufende Instanz mit
`gnome-control-center --quit` beenden).

## Test unter GNOME

1. **Einstellungen → Netzwerk → VPN → „+“** — in der Liste erscheint jetzt
   **Tailscale**.
2. Namen vergeben. Neben dem Auth-Key gibt es drei Einstellungen, die beim
   Verbinden als Tailscale-Prefs gesetzt werden. Bei einer **neuen**
   Verbindung werden sie mit den aktuellen tailscaled-Einstellungen
   vorbelegt (per LocalAPI ausgelesen) — auf einem bereits eingerichteten
   Gerät ist das erste Verbinden damit verhaltensneutral. Ist tailscaled
   dabei nicht erreichbar, gelten die Tailscale-Defaults (DNS an, Routen
   aus, kein Exit-Node):
   - **Exit node** — Dropdown mit allen Peers, die sich als Exit-Node
     anbieten („None“ = direkter Traffic). Die Liste wird live von der
     LocalAPI geladen (Lesezugriff, unter Linux für alle lokalen Benutzer
     erlaubt).
   - **Accept DNS (MagicDNS)** — entspricht `--accept-dns` (Standard: an)
   - **Accept advertised routes** — entspricht `--accept-routes` (Standard: aus)

   Für die **Geräteregistrierung** (erstes Anmelden im Tailnet) gibt es drei
   Wege:
   - tailscaled ist bereits eingeloggt: Auth-Key-Feld leer lassen.
   - **Auth-Key** aus der Tailscale-Admin-Konsole (`tskey-auth-…`)
     eintragen. Er wird als NM-Secret (system-owned) in
     `/etc/NetworkManager/system-connections/` gespeichert; die
     Registrierung passiert beim ersten Verbinden — kein Operator nötig,
     das erledigt der root-Daemon.
   - **„Log in via browser instead…“** klicken: öffnet die
     Tailscale-Anmeldeseite (SSO) im Browser; nach Abschluss zeigt der
     Dialog „Device is registered“. Fehlt deinem Benutzer der Schreibzugriff
     auf die LocalAPI, richtet der Button ihn beim ersten Klick selbst ein:
     Es erscheint einmalig die Systemauthentifizierung (polkit), die
     `tailscale set --operator=$USER` als root ausführt — danach läuft der
     Login automatisch weiter. Wer den Prompt vermeiden will, führt den
     Befehl einfach schon bei der Installation aus (siehe oben).
3. Verbindung mit dem VPN-Schalter aktivieren. Der Daemon setzt
   `WantRunning=true` (Äquivalent zu `tailscale up`) und meldet die Verbindung
   als aktiv, sobald tailscaled den Zustand `Running` erreicht.
4. Prüfen: `tailscale status` und `ip addr show tailscale0`.
5. Deaktivieren des Schalters entspricht `tailscale down`.

Fehlersuche: `journalctl -u NetworkManager -f` — die Meldungen des Plugins
erscheinen dort unter `nm-tailscale-service`.

## Nutzung unter KDE Plasma

Mit dem nativen plasma-nm-Plugin funktioniert unter KDE alles direkt in den
**Plasma-Systemeinstellungen** (Netzwerk → Verbindungen → „+“ → VPN →
Tailscale): Anlegen, Bearbeiten (Auth-Key, Browser-Login, Exit-Node,
DNS/Routen) und Verbinden über das Netzwerk-Applet.

**Wichtig — Versionskopplung:** Das native Plugin baut gegen die privaten
Header der exakt installierten plasma-nm-Version (Checkout in
`reference/plasma-nm`, Tag muss zu `pacman -Q plasma-nm` passen) und muss
nach plasma-nm-Updates neu gebaut werden. Lädt es nach einem Update nicht
mehr, den Stub als Fallback installieren (Verbinden übers Applet geht damit
weiter, nur der Einstellungs-Dialog fehlt):

```sh
sudo install -m755 dist/plasmanetworkmanagement_tailscaleui-stub.so \
    /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
```

Alternativ geht `nmcli` (einen GTK3-Editor für den `nm-connection-editor`
gibt es bewusst nicht — unter KDE sind die Plasma-Systemeinstellungen die
grafische Oberfläche):

```sh
nmcli connection add type vpn con-name Tailscale vpn-type tailscale
nmcli connection modify Tailscale vpn.secrets "auth-key=tskey-auth-…"   # optional
nmcli connection modify Tailscale +vpn.data "accept-dns=no"             # optional
nmcli connection modify Tailscale +vpn.data "accept-routes=yes"         # optional
nmcli connection modify Tailscale +vpn.data "exit-node=100.x.y.z"       # optional (Tailscale-IP des Exit-Nodes)
nmcli connection up Tailscale
nmcli connection down Tailscale
```

Fehlen `accept-dns`/`accept-routes`/`exit-node` in der Verbindung, lässt der
Daemon die entsprechenden tailscaled-Einstellungen unangetastet
(CLI-Verhalten bleibt). `exit-node=` mit leerem Wert schaltet den Exit-Node
explizit ab.

## Hinweise / Einschränkungen (Version 1)

- tailscaled verwaltet Interface, Routen und DNS selbst. NM bekommt nur eine
  minimale IP-Konfiguration (`tailscale0`, Tailscale-IPv4 /32,
  `never-default`, `preserve-routes`) zur Statusanzeige.
- NM besteht auf einer VPN-Gateway-Adresse; da Tailscale als Mesh keine hat,
  meldet der Daemon `::1`. Das erfüllt die Prüfung, und weil der Kernel `::1`
  nie über das physische Interface auflöst, legt NM daraus keine Route an —
  es ist keine besondere Verbindungs-Einstellung nötig.
- Die Einstellungen (Exit-Node, Accept DNS/routes) werden beim Verbinden
  gesetzt und bleiben danach in tailscaled gespeichert (wie bei
  `tailscale set`).
- **Status-Sync:** Der Daemon überwacht tailscaled im 5-Sekunden-Takt.
  Ein externes `tailscale down`, `tailscale logout` oder ein Absturz von
  tailscaled beendet die NM-Verbindung automatisch sauber.
- **tailscaled läuft nicht / tailscale nicht installiert:** Die Aktivierung
  schlägt kontrolliert fehl (GNOME zeigt „VPN-Verbindung fehlgeschlagen“);
  im Journal steht eine klare Meldung, ob der Socket fehlt (tailscale nicht
  installiert bzw. Dienst nie gestartet) oder tailscaled nur nicht
  erreichbar ist.
- Kein Multi-Account-Support.

## Deinstallation (UNINSTALL)

Zuerst Verbindungen deaktivieren und löschen:

```sh
nmcli --fields NAME,TYPE connection show | awk '$2=="vpn"' # Tailscale-Verbindungen finden
nmcli connection down Tailscale 2>/dev/null || true
nmcli connection delete Tailscale
```

Dann alle installierten Dateien entfernen und Dienste neu laden:

```sh
sudo rm -f /usr/lib/NetworkManager/nm-tailscale-service \
           /usr/lib/NetworkManager/libnm-vpn-plugin-tailscale.so \
           /usr/lib/NetworkManager/libnm-gtk4-vpn-plugin-tailscale-editor.so \
           /usr/lib/NetworkManager/VPN/nm-tailscale-service.name \
           /usr/share/dbus-1/system.d/nm-tailscale-service.conf \
           /usr/lib/qt6/plugins/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so
sudo systemctl reload dbus
sudo systemctl restart NetworkManager
```

Damit ist das System wieder im Ausgangszustand — keine der Dateien wird von
pacman verwaltet, es bleiben keine weiteren Rückstände. (tailscaled selbst
bleibt unberührt; bei Bedarf `sudo pacman -R tailscale`.)
