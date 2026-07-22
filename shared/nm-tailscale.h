/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NM_TAILSCALE_H
#define NM_TAILSCALE_H

#define NM_DBUS_SERVICE_TAILSCALE "org.freedesktop.NetworkManager.tailscale"
#define NM_TAILSCALE_KEY_AUTH_KEY "auth-key"

/* data items, values "yes"/"no"; absent = tailscaled prefs stay untouched */
#define NM_TAILSCALE_KEY_ACCEPT_DNS    "accept-dns"
#define NM_TAILSCALE_KEY_ACCEPT_ROUTES "accept-routes"

/* data item, Tailscale IPv4 of the exit node, "" = none; absent = untouched */
#define NM_TAILSCALE_KEY_EXIT_NODE     "exit-node"

#endif
