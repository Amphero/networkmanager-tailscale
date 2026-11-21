#!/usr/bin/env python3
"""Loads the editor plugin into a GTK3 or GTK4 process and round-trips a
connection through the editor widget.

Usage: editor-test.py 3.0|4.0 path/to/libnm-vpn-plugin-tailscale.so
Needs a display, run it under xvfb-run. tailscaled is not needed, the
LocalAPI calls of the editor just fail.
"""
import sys
import time

import gi

gtk_version, plugin_path = sys.argv[1], sys.argv[2]
gi.require_version('Gtk', gtk_version)
gi.require_version('NM', '1.0')
from gi.repository import GLib, Gtk, NM  # noqa: E402

SERVICE = 'org.freedesktop.NetworkManager.tailscale'
GTK3 = gtk_version == '3.0'

if GTK3:
    Gtk.init([])
else:
    Gtk.init()


def iterate(n=50):
    ctx = GLib.MainContext.default()
    for _ in range(n):
        ctx.iteration(False)


def children(widget):
    if GTK3:
        return widget.get_children() if isinstance(widget, Gtk.Container) else []
    out = []
    child = widget.get_first_child()
    while child:
        out.append(child)
        child = child.get_next_sibling()
    return out


def walk(widget):
    yield widget
    for child in children(widget):
        yield from walk(child)


def read_back(editor):
    conn = NM.SimpleConnection.new()
    conn.add_setting(NM.SettingConnection.new())
    assert editor.update_connection(conn)
    s_vpn = conn.get_setting_vpn()
    data = {k: s_vpn.get_data_item(k) for k in ('accept-dns', 'accept-routes', 'exit-node')}
    return data, s_vpn.get_secret('auth-key')


conn = NM.SimpleConnection.new()
s_con = NM.SettingConnection.new()
s_con.set_property('id', 'test')
s_con.set_property('type', 'vpn')
s_con.set_property('uuid', NM.utils_uuid_generate())
s_vpn = NM.SettingVpn.new()
s_vpn.set_property('service-type', SERVICE)
s_vpn.add_data_item('accept-dns', 'no')
s_vpn.add_data_item('accept-routes', 'yes')
s_vpn.add_data_item('exit-node', '100.1.2.3')
s_vpn.add_secret('auth-key', 'tskey-test')
conn.add_setting(s_con)
conn.add_setting(s_vpn)

plugin = NM.VpnEditorPlugin.load(plugin_path, SERVICE)
editor = plugin.get_editor(conn)
changes = []
editor.connect('changed', lambda *args: changes.append(1))
iterate()

widgets = list(walk(editor.get_widget()))
combo = next(w for w in widgets if type(w).__name__ in ('ComboBoxText', 'DropDown'))

# the stored exit node is only selected once the peer list request is
# done, even a failed one, so wait for it to show up in the combo
for _ in range(100):
    n = len(combo.get_model()) if GTK3 else combo.get_model().get_n_items()
    if n > 1:
        break
    iterate(1)
    time.sleep(0.05)
else:
    raise AssertionError('stored exit node never selected')

checks = {c.get_label(): c for c in widgets if isinstance(c, Gtk.CheckButton)}
dns = next(c for label, c in checks.items() if 'DNS' in label)
routes = next(c for label, c in checks.items() if 'routes' in label)
entry = next(w for w in widgets if type(w).__name__ in ('Entry', 'PasswordEntry'))
if GTK3:
    assert all(w.get_visible() for w in widgets), 'hidden widgets'
    assert not entry.get_visibility(), 'auth key shown in clear text'

data, secret = read_back(editor)
assert data == {'accept-dns': 'no', 'accept-routes': 'yes', 'exit-node': '100.1.2.3'}, data
assert secret == 'tskey-test', secret

dns.set_active(True)
routes.set_active(False)
if GTK3:
    combo.set_active(0)
else:
    combo.set_selected(0)
entry.set_text('')
iterate(20)

data, secret = read_back(editor)
assert data == {'accept-dns': 'yes', 'accept-routes': 'no', 'exit-node': ''}, data
assert secret is None, secret
assert changes, 'no changed signal'
print(f'GTK {gtk_version} editor OK')
