/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * nm-tailscale-service - NetworkManager VPN service for Tailscale
 *
 * Thin wrapper around tailscaled's LocalAPI: Connect flips WantRunning on
 * (the equivalent of `tailscale up`), Disconnect flips it off. tailscaled
 * itself manages the tunnel device, routes and DNS, so NetworkManager only
 * gets a minimal IP config for status display. While connected, the daemon
 * keeps polling the backend state so an external `tailscale down`/logout
 * cleanly tears down the NM connection as well.
 */

#include <arpa/inet.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <glib-unix.h>
#include <NetworkManager.h>
#include <json-glib/json-glib.h>

#include "nm-tailscale.h"
#include "nm-tailscale-localapi.h"

#define TUNDEV               "tailscale0"
#define POLL_INTERVAL_MS     500
#define CONNECT_TIMEOUT_MS   (90 * 1000)
/* how long a submitted auth key may sit in NeedsLogin before we call it
 * rejected — accepting one normally takes tailscaled a few seconds */
#define AUTH_KEY_GRACE_MS    (15 * 1000)
#define MONITOR_INTERVAL_MS  5000
/* a minute of transport failures before giving up: a plain tailscaled
 * restart (package upgrade) already takes the socket away for ~20 s */
#define MONITOR_MAX_FAILURES 12

typedef struct {
	NMVpnServicePlugin parent;
	guint poll_id;
	gint64 connect_start;
	guint monitor_fails;
	gboolean sent_auth_key;
	gboolean call_busy; /* a poll/monitor status GET is in flight */
	GCancellable *cancellable;
	char *rollback; /* prefs PATCH undoing real_connect, for failed connects */
	/* context of the running connect chain */
	char *pending_auth_key;
	char *pending_mask;
	gboolean pending_needs_login;
	gboolean pending_set_dns;
	gboolean pending_set_routes;
	gboolean pending_set_exit;
} NMTailscalePlugin;

typedef struct {
	NMVpnServicePluginClass parent;
} NMTailscalePluginClass;

#define NM_TYPE_TAILSCALE_PLUGIN (nm_tailscale_plugin_get_type ())
#define NM_TAILSCALE_PLUGIN(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), NM_TYPE_TAILSCALE_PLUGIN, NMTailscalePlugin))

GType nm_tailscale_plugin_get_type (void);
G_DEFINE_TYPE (NMTailscalePlugin, nm_tailscale_plugin, NM_TYPE_VPN_SERVICE_PLUGIN)

/*****************************************************************************/

static char *
json_quote (const char *s)
{
	JsonNode *node = json_node_init_string (json_node_alloc (), s);
	char *out = json_to_string (node, FALSE);

	json_node_free (node);
	return out;
}

/* @out_auth_url non-empty means tailscale demands (re-)authentication, and
 * @out_online only turns TRUE once the control server accepted the node —
 * BackendState alone can claim "Running" from cached state */
static gboolean
parse_status (const char *json, char **out_state, char **out_ip4, char **out_ip6,
              char **out_auth_url, gboolean *out_online, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonObject *root;
	JsonNode *node;

	*out_state = NULL;
	*out_ip4 = NULL;
	*out_ip6 = NULL;
	*out_auth_url = NULL;
	*out_online = FALSE;

	if (!json_parser_load_from_data (parser, json, -1, error))
		return FALSE;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node)) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
		             "unexpected LocalAPI status reply");
		return FALSE;
	}
	root = json_node_get_object (node);
	*out_state = g_strdup (json_object_get_string_member_with_default (root, "BackendState", ""));
	*out_auth_url = g_strdup (json_object_get_string_member_with_default (root, "AuthURL", ""));

	node = json_object_get_member (root, "Self");
	if (node && JSON_NODE_HOLDS_OBJECT (node)) {
		JsonNode *ips = json_object_get_member (json_node_get_object (node), "TailscaleIPs");

		*out_online = json_object_get_boolean_member_with_default (json_node_get_object (node),
		                                                          "Online", FALSE);

		if (ips && JSON_NODE_HOLDS_ARRAY (ips)) {
			JsonArray *arr = json_node_get_array (ips);
			guint i;

			for (i = 0; i < json_array_get_length (arr); i++) {
				const char *s = json_array_get_string_element (arr, i);

				if (!s)
					continue;
				if (!*out_ip4 && strchr (s, '.'))
					*out_ip4 = g_strdup (s);
				else if (!*out_ip6 && strchr (s, ':'))
					*out_ip6 = g_strdup (s);
			}
		}
	}
	return TRUE;
}

/*****************************************************************************/

static void
stop_poll (NMTailscalePlugin *self)
{
	if (self->poll_id) {
		g_source_remove (self->poll_id);
		self->poll_id = 0;
	}
}

/* invalidates every in-flight LocalAPI call; their callbacks see a
 * cancelled error and bail without touching the plugin state */
static void
cancel_calls (NMTailscalePlugin *self)
{
	if (self->cancellable)
		g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);
	self->cancellable = g_cancellable_new ();
	self->call_busy = FALSE;
}

/* captures the prefs real_connect is about to overwrite, so a failed
 * connect does not leave the connection's settings behind in tailscaled */
static char *
build_rollback (const char *prefs_json, gboolean set_dns, gboolean set_routes, gboolean set_exit)
{
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *node;
	JsonObject *root;
	GString *out;

	if (!prefs_json || !json_parser_load_from_data (parser, prefs_json, -1, NULL))
		return NULL;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return NULL;
	root = json_node_get_object (node);

	out = g_string_new (NULL);
	g_string_append_printf (out, "{\"WantRunning\":%s,\"WantRunningSet\":true",
	                        json_object_get_boolean_member_with_default (root, "WantRunning", FALSE) ? "true" : "false");
	if (set_dns)
		g_string_append_printf (out, ",\"CorpDNS\":%s,\"CorpDNSSet\":true",
		                        json_object_get_boolean_member_with_default (root, "CorpDNS", TRUE) ? "true" : "false");
	if (set_routes)
		g_string_append_printf (out, ",\"RouteAll\":%s,\"RouteAllSet\":true",
		                        json_object_get_boolean_member_with_default (root, "RouteAll", FALSE) ? "true" : "false");
	if (set_exit) {
		g_autofree char *ip = json_quote (json_object_get_string_member_with_default (root, "ExitNodeIP", ""));
		g_autofree char *id = json_quote (json_object_get_string_member_with_default (root, "ExitNodeID", ""));

		g_string_append_printf (out, ",\"ExitNodeIP\":%s,\"ExitNodeIPSet\":true"
		                             ",\"ExitNodeID\":%s,\"ExitNodeIDSet\":true", ip, id);
	}
	g_string_append_c (out, '}');
	return g_string_free (out, FALSE);
}

static void
rollback_done_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);

	if (!resp)
		g_warning ("could not restore the previous tailscaled prefs: %s", error->message);
}

static void
apply_rollback (NMTailscalePlugin *self)
{
	if (!self->rollback)
		return;
	/* fire and forget, deliberately without the cancellable: the prefs
	 * should go back even when the connect attempt is already history */
	nm_tailscale_localapi_call_async ("PATCH", "/localapi/v0/prefs", self->rollback, 0,
	                                  NULL, rollback_done_cb, NULL);
	g_clear_pointer (&self->rollback, g_free);
}

static gboolean
send_config (NMTailscalePlugin *self, const char *ip4, const char *ip6)
{
	GVariantBuilder config, ip_config;
	guint32 addr4 = 0;
	struct in6_addr addr6;
	gboolean has4 = ip4 && inet_pton (AF_INET, ip4, &addr4) == 1;
	gboolean has6 = ip6 && inet_pton (AF_INET6, ip6, &addr6) == 1;

	if (!has4 && !has6) {
		g_warning ("no usable Tailscale address (v4: %s, v6: %s)", ip4 ?: "none", ip6 ?: "none");
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_BAD_IP_CONFIG);
		return FALSE;
	}

	static const guint8 loopback6[16] = { [15] = 1 };

	g_variant_builder_init (&config, G_VARIANT_TYPE ("a{sv}"));
	g_variant_builder_add (&config, "{sv}", NM_VPN_PLUGIN_CONFIG_TUNDEV, g_variant_new_string (TUNDEV));
	/* NM refuses configs without an external gateway; tailscale is a mesh
	 * without one. Report ::1: it satisfies the check, and since it can
	 * never resolve via the parent device, NM creates no route from it. */
	g_variant_builder_add (&config, "{sv}", NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY,
	                       g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, loopback6, 16, 1));
	g_variant_builder_add (&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP4, g_variant_new_boolean (has4));
	g_variant_builder_add (&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP6, g_variant_new_boolean (has6));
	nm_vpn_service_plugin_set_config (NM_VPN_SERVICE_PLUGIN (self), g_variant_builder_end (&config));

	/* tailscaled already configured the interface; never-default and
	 * preserve-routes keep NM from touching routing and default route. */
	if (has4) {
		g_variant_builder_init (&ip_config, G_VARIANT_TYPE ("a{sv}"));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS, g_variant_new_uint32 (addr4));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, g_variant_new_uint32 (32));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_NEVER_DEFAULT, g_variant_new_boolean (TRUE));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_PRESERVE_ROUTES, g_variant_new_boolean (TRUE));
		nm_vpn_service_plugin_set_ip4_config (NM_VPN_SERVICE_PLUGIN (self), g_variant_builder_end (&ip_config));
	}
	if (has6) {
		g_variant_builder_init (&ip_config, G_VARIANT_TYPE ("a{sv}"));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_ADDRESS,
		                       g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, addr6.s6_addr, 16, 1));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_PREFIX, g_variant_new_uint32 (128));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_NEVER_DEFAULT, g_variant_new_boolean (TRUE));
		g_variant_builder_add (&ip_config, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_PRESERVE_ROUTES, g_variant_new_boolean (TRUE));
		nm_vpn_service_plugin_set_ip6_config (NM_VPN_SERVICE_PLUGIN (self), g_variant_builder_end (&ip_config));
	}

	g_message ("tailscale is up (%s%s%s on %s)",
	           has4 ? ip4 : "", has4 && has6 ? ", " : "", has6 ? ip6 : "", TUNDEV);
	return TRUE;
}

/* the poll and monitor timers only kick off worker-thread requests; all
 * handling happens in the done-callbacks on the main context. A callback
 * arriving after cancel_calls() or stop_poll() drops out immediately. */

static gboolean monitor_tick_cb (gpointer user_data);

static void
monitor_done_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	gboolean online;

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	self->call_busy = FALSE;
	if (!self->poll_id)
		goto out; /* stopped while the request ran */

	if (resp && parse_status (resp, &state, &ip4, &ip6, &auth_url, &online, &error)) {
		self->monitor_fails = 0;
		if (   g_strcmp0 (state, "Stopped") == 0
		    || g_strcmp0 (state, "NeedsLogin") == 0
		    || auth_url[0]) {
			g_message ("tailscale was stopped or logged out outside of NetworkManager (%s%s); disconnecting",
			           state, auth_url[0] ? ", re-authentication required" : "");
			stop_poll (self);
			nm_vpn_service_plugin_disconnect (NM_VPN_SERVICE_PLUGIN (self), NULL);
		}
		goto out;
	}

	self->monitor_fails++;
	g_warning ("monitoring tailscaled failed (%u/%u): %s",
	           self->monitor_fails, MONITOR_MAX_FAILURES,
	           error ? error->message : "unknown error");
	if (self->monitor_fails >= MONITOR_MAX_FAILURES) {
		stop_poll (self);
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
	}
out:
	g_object_unref (self);
}

/* watches the backend while connected: an external `tailscale down` or an
 * expired login tears the NM connection down as well */
static gboolean
monitor_tick_cb (gpointer user_data)
{
	NMTailscalePlugin *self = user_data;

	if (!self->call_busy) {
		self->call_busy = TRUE;
		nm_tailscale_localapi_call_async ("GET", "/localapi/v0/status", NULL, 2000,
		                                  self->cancellable, monitor_done_cb, g_object_ref (self));
	}
	return G_SOURCE_CONTINUE;
}

static void
poll_done_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	gboolean online;
	gint64 elapsed_ms = (g_get_monotonic_time () - self->connect_start) / 1000;

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	self->call_busy = FALSE;
	if (!self->poll_id)
		goto out; /* stopped while the request ran */

	if (resp && parse_status (resp, &state, &ip4, &ip6, &auth_url, &online, &error)) {
		if (nm_tailscale_debug)
			g_message ("backend state: %s online=%d%s", state, online, auth_url[0] ? " (auth pending)" : "");
		if (g_strcmp0 (state, "Running") == 0 && (ip4 || ip6) && online && !auth_url[0]) {
			stop_poll (self);
			if (send_config (self, ip4, ip6)) {
				g_clear_pointer (&self->rollback, g_free);
				self->monitor_fails = 0;
				self->poll_id = g_timeout_add (MONITOR_INTERVAL_MS, monitor_tick_cb, self);
			} else {
				apply_rollback (self);
			}
			goto out;
		}
		if (g_strcmp0 (state, "NeedsLogin") == 0 || auth_url[0]) {
			if (!self->sent_auth_key) {
				g_warning ("tailscale requires (re-)authentication; use the browser login "
				           "in the connection editor or store an auth key in the connection");
				stop_poll (self);
				apply_rollback (self);
				nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
				goto out;
			}
			/* the key already went to /start: an AuthURL or a lingering
			 * NeedsLogin means the control server did not accept it */
			if (auth_url[0] || elapsed_ms >= AUTH_KEY_GRACE_MS) {
				g_warning ("tailscale did not accept the stored auth key (expired or revoked?)");
				stop_poll (self);
				apply_rollback (self);
				nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
				goto out;
			}
		}
	} else {
		g_warning ("polling tailscaled status failed: %s", error ? error->message : "unknown error");
	}
	/* the connect timeout lives in poll_tick_cb */
out:
	g_object_unref (self);
}

static gboolean
poll_tick_cb (gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	gint64 elapsed_ms = (g_get_monotonic_time () - self->connect_start) / 1000;

	if (elapsed_ms >= CONNECT_TIMEOUT_MS) {
		g_warning ("timeout waiting for tailscale to reach the Running state");
		self->poll_id = 0;
		apply_rollback (self);
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		return G_SOURCE_REMOVE;
	}
	if (!self->call_busy) {
		self->call_busy = TRUE;
		nm_tailscale_localapi_call_async ("GET", "/localapi/v0/status", NULL, 2000,
		                                  self->cancellable, poll_done_cb, g_object_ref (self));
	}
	return G_SOURCE_CONTINUE;
}

static void
begin_poll (NMTailscalePlugin *self)
{
	stop_poll (self);
	self->poll_id = g_timeout_add (POLL_INTERVAL_MS, poll_tick_cb, self);
}

static gboolean
data_item_bool (NMSettingVpn *s_vpn, const char *key, gboolean *out)
{
	const char *v = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, key) : NULL;

	if (!v)
		return FALSE;
	*out = g_strcmp0 (v, "yes") == 0 || g_strcmp0 (v, "true") == 0;
	return TRUE;
}

/* connect chain: status -> prefs snapshot -> prefs PATCH -> optional auth
 * key login -> poll. real_connect only kicks it off; errors surface via
 * nm_vpn_service_plugin_failure once the responses come in. */

static void
connect_start_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	if (!resp) {
		g_warning ("starting the auth key login failed: %s", error->message);
		apply_rollback (self);
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
		goto out;
	}
	begin_poll (self);
out:
	g_object_unref (self);
}

static void
connect_patch_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	if (!resp) {
		g_warning ("applying the connection prefs failed: %s", error->message);
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		goto out;
	}

	self->sent_auth_key = self->pending_needs_login;
	if (self->pending_needs_login) {
		g_autofree char *quoted = json_quote (self->pending_auth_key);
		g_autofree char *body = g_strdup_printf ("{\"AuthKey\":%s}", quoted);

		nm_tailscale_localapi_call_async ("POST", "/localapi/v0/start", body, 0,
		                                  self->cancellable, connect_start_cb, g_object_ref (self));
	} else {
		begin_poll (self);
	}
out:
	g_object_unref (self);
}

static void
connect_prefs_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	/* the snapshot is best effort: without it there is just no rollback */
	g_clear_pointer (&self->rollback, g_free);
	self->rollback = build_rollback (resp, self->pending_set_dns, self->pending_set_routes,
	                                 self->pending_set_exit);

	nm_tailscale_localapi_call_async ("PATCH", "/localapi/v0/prefs", self->pending_mask, 0,
	                                  self->cancellable, connect_patch_cb, g_object_ref (self));
out:
	g_object_unref (self);
}

static void
connect_status_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = nm_tailscale_localapi_call_finish (result, NULL, &error);
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	gboolean online;

	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		goto out;
	if (!resp || !parse_status (resp, &state, &ip4, &ip6, &auth_url, &online, &error)) {
		g_warning ("cannot connect: %s", error ? error->message : "unexpected LocalAPI status reply");
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		goto out;
	}

	self->pending_needs_login = g_strcmp0 (state, "NeedsLogin") == 0 || auth_url[0];
	if (self->pending_needs_login && !self->pending_auth_key) {
		g_warning ("tailscale requires (re-)authentication; use the browser login "
		           "in the connection editor or store an auth key in the connection");
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
		goto out;
	}

	nm_tailscale_localapi_call_async ("GET", "/localapi/v0/prefs", NULL, 2000,
	                                  self->cancellable, connect_prefs_cb, g_object_ref (self));
out:
	g_object_unref (self);
}

static gboolean
real_connect (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN (plugin);
	NMSettingVpn *s_vpn = nm_connection_get_setting_vpn (connection);
	const char *auth_key = s_vpn ? nm_setting_vpn_get_secret (s_vpn, NM_TAILSCALE_KEY_AUTH_KEY) : NULL;
	const char *exit_node = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_EXIT_NODE) : NULL;
	g_autoptr(GString) mask = NULL;
	gboolean set_dns, set_routes, dns_on = FALSE, routes_on = FALSE;

	/* the equivalent of `tailscale up`; the optional settings are only
	 * touched when the connection carries the corresponding data item */
	set_dns = data_item_bool (s_vpn, NM_TAILSCALE_KEY_ACCEPT_DNS, &dns_on);
	set_routes = data_item_bool (s_vpn, NM_TAILSCALE_KEY_ACCEPT_ROUTES, &routes_on);
	mask = g_string_new ("{\"WantRunning\":true,\"WantRunningSet\":true");
	if (set_dns)
		g_string_append_printf (mask, ",\"CorpDNS\":%s,\"CorpDNSSet\":true", dns_on ? "true" : "false");
	if (set_routes)
		g_string_append_printf (mask, ",\"RouteAll\":%s,\"RouteAllSet\":true", routes_on ? "true" : "false");
	if (exit_node) {
		g_autofree char *quoted = json_quote (exit_node);

		/* clear any ExitNodeID a previous `tailscale set` left behind;
		 * tailscaled resolves the IP back to an ID itself */
		g_string_append_printf (mask, ",\"ExitNodeIP\":%s,\"ExitNodeIPSet\":true"
		                              ",\"ExitNodeID\":\"\",\"ExitNodeIDSet\":true", quoted);
	}
	g_string_append_c (mask, '}');

	stop_poll (self);
	cancel_calls (self);
	g_clear_pointer (&self->pending_auth_key, g_free);
	g_clear_pointer (&self->pending_mask, g_free);
	self->pending_auth_key = auth_key && auth_key[0] ? g_strdup (auth_key) : NULL;
	self->pending_mask = g_strdup (mask->str);
	self->pending_set_dns = set_dns;
	self->pending_set_routes = set_routes;
	self->pending_set_exit = exit_node != NULL;
	self->sent_auth_key = FALSE;
	self->connect_start = g_get_monotonic_time ();

	nm_tailscale_localapi_call_async ("GET", "/localapi/v0/status", NULL, 2000,
	                                  self->cancellable, connect_status_cb, g_object_ref (self));
	return TRUE;
}

static gboolean
real_need_secrets (NMVpnServicePlugin *plugin,
                   NMConnection *connection,
                   const char **setting_name,
                   GError **error)
{
	/* the auth key is optional and system-stored; never prompt */
	return FALSE;
}

static gboolean
real_disconnect (NMVpnServicePlugin *plugin, GError **error)
{
	NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN (plugin);
	g_autofree char *resp = NULL;

	stop_poll (self);
	resp = nm_tailscale_localapi_call ("PATCH", "/localapi/v0/prefs",
	                                   "{\"WantRunning\":false,\"WantRunningSet\":true}", 0, NULL, error);
	return resp != NULL;
}

/*****************************************************************************/

static void
state_changed_cb (NMTailscalePlugin *self, NMVpnServiceState state, gpointer user_data)
{
	switch (state) {
	case NM_VPN_SERVICE_STATE_UNKNOWN:
	case NM_VPN_SERVICE_STATE_INIT:
	case NM_VPN_SERVICE_STATE_SHUTDOWN:
	case NM_VPN_SERVICE_STATE_STOPPING:
	case NM_VPN_SERVICE_STATE_STOPPED:
		stop_poll (self);
		cancel_calls (self);
		break;
	default:
		break;
	}
}

static void
dispose (GObject *object)
{
	NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN (object);

	stop_poll (self);
	if (self->cancellable)
		g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);
	g_clear_pointer (&self->rollback, g_free);
	g_clear_pointer (&self->pending_auth_key, g_free);
	g_clear_pointer (&self->pending_mask, g_free);
	G_OBJECT_CLASS (nm_tailscale_plugin_parent_class)->dispose (object);
}

static void
nm_tailscale_plugin_init (NMTailscalePlugin *self)
{
	self->cancellable = g_cancellable_new ();
}

static void
nm_tailscale_plugin_class_init (NMTailscalePluginClass *klass)
{
	NMVpnServicePluginClass *parent_class = NM_VPN_SERVICE_PLUGIN_CLASS (klass);

	G_OBJECT_CLASS (klass)->dispose = dispose;
	parent_class->connect = real_connect;
	parent_class->need_secrets = real_need_secrets;
	parent_class->disconnect = real_disconnect;
}

/*****************************************************************************/

static gboolean
quit_cb (gpointer user_data)
{
	g_main_loop_quit (user_data);
	return G_SOURCE_CONTINUE;
}

int
main (int argc, char *argv[])
{
	NMTailscalePlugin *plugin;
	GMainLoop *loop;
	GError *error = NULL;
	gboolean persist = FALSE;
	char *bus_name = NM_DBUS_SERVICE_TAILSCALE;
	GOptionContext *opt_ctx;
	GOptionEntry options[] = {
		{ "persist", 0, 0, G_OPTION_ARG_NONE, &persist, "Don't quit when VPN connection terminates", NULL },
		{ "debug", 0, 0, G_OPTION_ARG_NONE, &nm_tailscale_debug, "Enable verbose debug logging (may expose secrets)", NULL },
		{ "bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name, "D-Bus name to use for this instance", NULL },
		{ NULL }
	};

	setlocale (LC_ALL, "");

	opt_ctx = g_option_context_new (NULL);
	g_option_context_add_main_entries (opt_ctx, options, NULL);
	g_option_context_set_summary (opt_ctx,
	                              "nm-tailscale-service provides integrated Tailscale capability to NetworkManager.");
	if (!g_option_context_parse (opt_ctx, &argc, &argv, &error)) {
		g_printerr ("Error parsing options: %s\n", error->message);
		return EXIT_FAILURE;
	}
	g_option_context_free (opt_ctx);

	if (getenv ("NM_TAILSCALE_DEBUG"))
		nm_tailscale_debug = TRUE;

	signal (SIGPIPE, SIG_IGN);

	plugin = g_initable_new (NM_TYPE_TAILSCALE_PLUGIN, NULL, &error,
	                         NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, bus_name,
	                         NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !nm_tailscale_debug,
	                         NULL);
	if (!plugin) {
		g_printerr ("Failed to initialize the plugin: %s\n", error->message);
		return EXIT_FAILURE;
	}
	g_signal_connect (plugin, "state-changed", G_CALLBACK (state_changed_cb), NULL);

	loop = g_main_loop_new (NULL, FALSE);
	if (!persist)
		g_signal_connect_swapped (plugin, "quit", G_CALLBACK (g_main_loop_quit), loop);
	g_unix_signal_add (SIGTERM, quit_cb, loop);
	g_unix_signal_add (SIGINT, quit_cb, loop);

	g_main_loop_run (loop);

	g_object_unref (plugin);
	g_main_loop_unref (loop);
	return EXIT_SUCCESS;
}
