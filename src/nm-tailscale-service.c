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
#define MONITOR_INTERVAL_MS  5000
#define MONITOR_MAX_FAILURES 3

typedef struct {
	NMVpnServicePlugin parent;
	guint poll_id;
	guint poll_count;
	guint monitor_fails;
	gboolean sent_auth_key;
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

/* watches the backend while connected: an external `tailscale down` or an
 * expired login tears the NM connection down as well */
static gboolean
monitor_cb (gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = NULL;
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	gboolean online;

	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/status", NULL, NULL, &error);
	if (resp && parse_status (resp, &state, &ip4, &ip6, &auth_url, &online, &error)) {
		self->monitor_fails = 0;
		if (   g_strcmp0 (state, "Stopped") == 0
		    || g_strcmp0 (state, "NeedsLogin") == 0
		    || auth_url[0]) {
			g_message ("tailscale was stopped or logged out outside of NetworkManager (%s%s); disconnecting",
			           state, auth_url[0] ? ", re-authentication required" : "");
			self->poll_id = 0;
			nm_vpn_service_plugin_disconnect (NM_VPN_SERVICE_PLUGIN (self), NULL);
			return G_SOURCE_REMOVE;
		}
		return G_SOURCE_CONTINUE;
	}

	self->monitor_fails++;
	g_warning ("monitoring tailscaled failed (%u/%u): %s",
	           self->monitor_fails, MONITOR_MAX_FAILURES,
	           error ? error->message : "unknown error");
	if (self->monitor_fails >= MONITOR_MAX_FAILURES) {
		self->poll_id = 0;
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
poll_status_cb (gpointer user_data)
{
	NMTailscalePlugin *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = NULL;
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	gboolean online;

	self->poll_count++;

	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/status", NULL, NULL, &error);
	if (resp && parse_status (resp, &state, &ip4, &ip6, &auth_url, &online, &error)) {
		if (nm_tailscale_debug)
			g_message ("backend state: %s online=%d%s", state, online, auth_url[0] ? " (auth pending)" : "");
		if (g_strcmp0 (state, "Running") == 0 && (ip4 || ip6) && online && !auth_url[0]) {
			if (send_config (self, ip4, ip6)) {
				self->monitor_fails = 0;
				self->poll_id = g_timeout_add (MONITOR_INTERVAL_MS, monitor_cb, self);
			} else {
				self->poll_id = 0;
			}
			return G_SOURCE_REMOVE;
		}
		if ((g_strcmp0 (state, "NeedsLogin") == 0 || auth_url[0]) && !self->sent_auth_key) {
			g_warning ("tailscale requires (re-)authentication; use the browser login "
			           "in the connection editor or store an auth key in the connection");
			self->poll_id = 0;
			nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
			return G_SOURCE_REMOVE;
		}
	} else {
		g_warning ("polling tailscaled status failed: %s", error ? error->message : "unknown error");
	}

	if (self->poll_count * POLL_INTERVAL_MS >= CONNECT_TIMEOUT_MS) {
		g_warning ("timeout waiting for tailscale to reach the Running state");
		self->poll_id = 0;
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (self), NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
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

static gboolean
real_connect (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN (plugin);
	NMSettingVpn *s_vpn = nm_connection_get_setting_vpn (connection);
	const char *auth_key = s_vpn ? nm_setting_vpn_get_secret (s_vpn, NM_TAILSCALE_KEY_AUTH_KEY) : NULL;
	const char *exit_node = s_vpn ? nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_EXIT_NODE) : NULL;
	g_autofree char *status = NULL;
	g_autofree char *state = NULL;
	g_autofree char *ip4 = NULL;
	g_autofree char *ip6 = NULL;
	g_autofree char *auth_url = NULL;
	g_autofree char *prefs = NULL;
	g_autoptr(GString) mask = NULL;
	gboolean needs_login, on, online;

	status = nm_tailscale_localapi_call ("GET", "/localapi/v0/status", NULL, NULL, error);
	if (!status || !parse_status (status, &state, &ip4, &ip6, &auth_url, &online, error))
		return FALSE;

	needs_login = g_strcmp0 (state, "NeedsLogin") == 0 || auth_url[0];
	if (needs_login && !(auth_key && auth_key[0])) {
		g_set_error (error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
		             "tailscale requires (re-)authentication; use the browser login "
		             "in the connection editor or store an auth key in the connection");
		return FALSE;
	}

	/* the equivalent of `tailscale up`; the optional settings are only
	 * touched when the connection carries the corresponding data item */
	mask = g_string_new ("{\"WantRunning\":true,\"WantRunningSet\":true");
	if (data_item_bool (s_vpn, NM_TAILSCALE_KEY_ACCEPT_DNS, &on))
		g_string_append_printf (mask, ",\"CorpDNS\":%s,\"CorpDNSSet\":true", on ? "true" : "false");
	if (data_item_bool (s_vpn, NM_TAILSCALE_KEY_ACCEPT_ROUTES, &on))
		g_string_append_printf (mask, ",\"RouteAll\":%s,\"RouteAllSet\":true", on ? "true" : "false");
	if (exit_node) {
		g_autofree char *quoted = json_quote (exit_node);

		/* clear any ExitNodeID a previous `tailscale set` left behind;
		 * tailscaled resolves the IP back to an ID itself */
		g_string_append_printf (mask, ",\"ExitNodeIP\":%s,\"ExitNodeIPSet\":true"
		                              ",\"ExitNodeID\":\"\",\"ExitNodeIDSet\":true", quoted);
	}
	g_string_append_c (mask, '}');

	prefs = nm_tailscale_localapi_call ("PATCH", "/localapi/v0/prefs", mask->str, NULL, error);
	if (!prefs)
		return FALSE;

	self->sent_auth_key = needs_login;
	if (needs_login) {
		g_autofree char *quoted = json_quote (auth_key);
		g_autofree char *body = g_strdup_printf ("{\"AuthKey\":%s}", quoted);
		g_autofree char *resp = NULL;

		resp = nm_tailscale_localapi_call ("POST", "/localapi/v0/start", body, NULL, error);
		if (!resp)
			return FALSE;
	}

	stop_poll (self);
	self->poll_count = 0;
	self->poll_id = g_timeout_add (POLL_INTERVAL_MS, poll_status_cb, self);
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
	                                   "{\"WantRunning\":false,\"WantRunningSet\":true}", NULL, error);
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
		break;
	default:
		break;
	}
}

static void
dispose (GObject *object)
{
	stop_poll (NM_TAILSCALE_PLUGIN (object));
	G_OBJECT_CLASS (nm_tailscale_plugin_parent_class)->dispose (object);
}

static void
nm_tailscale_plugin_init (NMTailscalePlugin *self)
{
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
