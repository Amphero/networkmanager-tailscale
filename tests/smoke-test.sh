#!/bin/sh
# D-Bus-Smoke-Test für den VPN-Service-Daemon — läuft im Container ohne
# echten tailscaled: privater Session-Bus als "System"-Bus, Python-Mock
# als LocalAPI. Prüft Namensregistrierung, Connect (inkl. Auth-Key-Login,
# Config/Ip4Config-Signale) und Disconnect.
set -eu

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- sh "$0" "$@"
fi
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"

TMP=$(mktemp -d)
export NM_TAILSCALE_SOCKET="$TMP/tailscaled.sock"
LOG="$TMP/localapi.log"
: > "$LOG"

python3 tests/mock-tailscaled.py "$NM_TAILSCALE_SOCKET" "$LOG" &
MOCK=$!
./dist/nm-tailscale-service --debug &
SVC=$!
MON=
cleanup() { kill $MOCK $SVC $MON 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
sleep 1

DEST=org.freedesktop.NetworkManager.tailscale
OBJ=/org/freedesktop/NetworkManager/VPN/Plugin
IFACE=org.freedesktop.NetworkManager.VPN.Plugin

echo "1/6 D-Bus-Name und Interface vorhanden"
gdbus introspect --system --dest "$DEST" --object-path "$OBJ" | grep -q "$IFACE"

gdbus monitor --system --dest "$DEST" > "$TMP/signals.log" 2>/dev/null &
MON=$!
sleep 1

echo "2/6 Connect (mit Auth-Key, tailscaled ausgeloggt)"
gdbus call --system --dest "$DEST" --object-path "$OBJ" --method "$IFACE.Connect" \
    "{'connection': {'id': <'Tailscale'>, 'uuid': <'52b3ad95-6a3f-4a62-9df8-4d0a9d07b5a2'>, 'type': <'vpn'>}, 'vpn': {'service-type': <'org.freedesktop.NetworkManager.tailscale'>, 'data': <@a{ss} {'accept-dns': 'no', 'accept-routes': 'yes', 'exit-node': '100.100.100.100'}>, 'secrets': <@a{ss} {'auth-key': 'tskey-test-123'}>}}" \
    > /dev/null

echo "3/6 warte auf Config/Ip4Config-Signale"
ok=0
i=0
while [ $i -lt 30 ]; do
    if grep -q "Ip4Config" "$TMP/signals.log"; then ok=1; break; fi
    i=$((i + 1))
    sleep 0.5
done
[ "$ok" = 1 ] || { echo "FEHLER: kein Ip4Config-Signal"; cat "$TMP/signals.log" "$LOG"; exit 1; }
grep -q "tailscale0" "$TMP/signals.log"
grep -q "'gateway'" "$TMP/signals.log"

echo "4/6 Auth-Key, WantRunning, DNS/Routen und Exit-Node kamen bei der LocalAPI an"
grep -qF '"AuthKey":"tskey-test-123"' "$LOG"
grep -qF '"WantRunning":true' "$LOG"
grep -qF '"CorpDNS":false,"CorpDNSSet":true' "$LOG"
grep -qF '"RouteAll":true,"RouteAllSet":true' "$LOG"
grep -qF '"ExitNodeIP":"100.100.100.100","ExitNodeIPSet":true' "$LOG"

echo "5/6 externes 'tailscale down' wird erkannt (Status-Sync)"
curl -s --unix-socket "$NM_TAILSCALE_SOCKET" -X PATCH \
    -d '{"WantRunning":false}' http://local-tailscaled.sock/localapi/v0/prefs > /dev/null
ok=0
i=0
while [ $i -lt 30 ]; do
    if grep -q "StateChanged (uint32 6" "$TMP/signals.log"; then ok=1; break; fi
    i=$((i + 1))
    sleep 1
done
[ "$ok" = 1 ] || { echo "FEHLER: Daemon hat externes down nicht erkannt"; cat "$TMP/signals.log" "$LOG"; exit 1; }

echo "6/6 Daemon hat die Verbindung sauber beendet (StateChanged -> STOPPED)"

echo "SMOKE-TEST BESTANDEN"
