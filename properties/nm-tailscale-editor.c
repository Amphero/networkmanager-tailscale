/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GTK4 editor widget shown in the GNOME Settings VPN dialog: optional auth
 * key (stored as an NM secret), browser login, exit node selection (populated
 * live from tailscaled's LocalAPI) and the accept-dns/accept-routes switches.
 */

#include <string.h>

#include <gtk/gtk.h>
#include <NetworkManager.h>
#include <json-glib/json-glib.h>

#include "nm-tailscale.h"
#include "nm-tailscale-localapi.h"

typedef struct {
	GObject parent;
	GtkWidget *widget;
	GtkWidget *entry;
	GtkWidget *exit_combo;
	GtkStringList *exit_labels;
	GPtrArray *exit_ips; /* parallel to the combo entries; [0] = "" (none) */
	GPtrArray *exit_ids; /* stable node IDs, parallel to exit_ips */
	GtkWidget *dns_check;
	GtkWidget *routes_check;
	GtkWidget *login_button;
	GtkWidget *login_status;
	guint login_poll_id;
	guint login_polls;
	guint operator_watch_id;
	GPid operator_pid;
	gboolean url_opened;
	gboolean operator_tried;
	gboolean restore_down;
} TailscaleEditor;

typedef struct {
	GObjectClass parent;
} TailscaleEditorClass;

static void tailscale_editor_interface_init (NMVpnEditorInterface *iface);
GType tailscale_editor_get_type (void);
G_DEFINE_TYPE_EXTENDED (TailscaleEditor, tailscale_editor, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR,
                                               tailscale_editor_interface_init))

#define TAILSCALE_EDITOR(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), tailscale_editor_get_type (), TailscaleEditor))

static void
combo_append (TailscaleEditor *self, const char *label)
{
	gtk_string_list_append (self->exit_labels, label);
}

/*****************************************************************************/

static GObject *
get_widget (NMVpnEditor *editor)
{
	return G_OBJECT (TAILSCALE_EDITOR (editor)->widget);
}

static gboolean
update_connection (NMVpnEditor *editor, NMConnection *connection, GError **error)
{
	TailscaleEditor *self = TAILSCALE_EDITOR (editor);
	NMSettingVpn *s_vpn = NM_SETTING_VPN (nm_setting_vpn_new ());
	const char *key = gtk_editable_get_text (GTK_EDITABLE (self->entry));
	guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->exit_combo));

	g_object_set (s_vpn, NM_SETTING_VPN_SERVICE_TYPE, NM_DBUS_SERVICE_TAILSCALE, NULL);
	nm_setting_vpn_add_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_DNS,
	                              gtk_check_button_get_active (GTK_CHECK_BUTTON (self->dns_check)) ? "yes" : "no");
	nm_setting_vpn_add_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_ROUTES,
	                              gtk_check_button_get_active (GTK_CHECK_BUTTON (self->routes_check)) ? "yes" : "no");
	if (sel < self->exit_ips->len)
		nm_setting_vpn_add_data_item (s_vpn, NM_TAILSCALE_KEY_EXIT_NODE,
		                              g_ptr_array_index (self->exit_ips, sel));
	if (key && key[0]) {
		nm_setting_vpn_add_secret (s_vpn, NM_TAILSCALE_KEY_AUTH_KEY, key);
		/* system-owned: the root service daemon must get it without an agent */
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_TAILSCALE_KEY_AUTH_KEY,
		                             NM_SETTING_SECRET_FLAG_NONE, NULL);
	}
	nm_connection_add_setting (connection, NM_SETTING (s_vpn));
	return TRUE;
}

static void
changed_cb (TailscaleEditor *self)
{
	g_signal_emit_by_name (self, "changed");
}

/* fills the dropdown with peers advertising themselves as exit nodes;
 * silently results in just "None" when the LocalAPI is not readable */
static void
populate_exit_nodes (TailscaleEditor *self)
{
	g_autofree char *resp = NULL;
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *node;
	JsonObjectIter iter;
	const char *member;

	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/status", NULL, NULL, NULL);
	if (!resp || !json_parser_load_from_data (parser, resp, -1, NULL))
		return;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return;
	node = json_object_get_member (json_node_get_object (node), "Peer");
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return;

	json_object_iter_init (&iter, json_node_get_object (node));
	while (json_object_iter_next (&iter, &member, &node)) {
		JsonObject *peer;
		JsonNode *ips;
		const char *ip = NULL;
		g_autofree char *label = NULL;

		if (!JSON_NODE_HOLDS_OBJECT (node))
			continue;
		peer = json_node_get_object (node);
		if (!json_object_get_boolean_member_with_default (peer, "ExitNodeOption", FALSE))
			continue;
		ips = json_object_get_member (peer, "TailscaleIPs");
		if (ips && JSON_NODE_HOLDS_ARRAY (ips)) {
			JsonArray *arr = json_node_get_array (ips);
			guint i;

			for (i = 0; i < json_array_get_length (arr); i++) {
				const char *s = json_array_get_string_element (arr, i);

				if (s && strchr (s, '.')) {
					ip = s;
					break;
				}
			}
		}
		if (!ip)
			continue;
		label = g_strdup_printf ("%s (%s)",
		                         json_object_get_string_member_with_default (peer, "HostName", "?"),
		                         ip);
		combo_append (self, label);
		g_ptr_array_add (self->exit_ips, g_strdup (ip));
		g_ptr_array_add (self->exit_ids,
		                 g_strdup (json_object_get_string_member_with_default (peer, "ID", "")));
	}
}

/* initial values for a freshly created connection: mirror the current
 * tailscaled prefs so that the first connect is behavior-neutral on a
 * device that is already set up */
static void
prefill_from_prefs (TailscaleEditor *self, gboolean *dns, gboolean *routes, guint *exit_idx)
{
	g_autofree char *resp = NULL;
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *node;
	JsonObject *root;
	const char *exit_ip, *exit_id;
	guint i;

	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/prefs", NULL, NULL, NULL);
	if (!resp || !json_parser_load_from_data (parser, resp, -1, NULL))
		return;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return;
	root = json_node_get_object (node);

	*dns = json_object_get_boolean_member_with_default (root, "CorpDNS", TRUE);
	*routes = json_object_get_boolean_member_with_default (root, "RouteAll", FALSE);

	exit_ip = json_object_get_string_member_with_default (root, "ExitNodeIP", "");
	exit_id = json_object_get_string_member_with_default (root, "ExitNodeID", "");
	if (exit_ip[0]) {
		for (i = 1; i < self->exit_ips->len; i++) {
			if (g_strcmp0 (g_ptr_array_index (self->exit_ips, i), exit_ip) == 0) {
				*exit_idx = i;
				return;
			}
		}
		combo_append (self, exit_ip);
		g_ptr_array_add (self->exit_ips, g_strdup (exit_ip));
		g_ptr_array_add (self->exit_ids, g_strdup (""));
		*exit_idx = self->exit_ips->len - 1;
	} else if (exit_id[0]) {
		/* `tailscale set --exit-node` stores the stable ID, not the IP */
		for (i = 1; i < self->exit_ids->len; i++) {
			if (g_strcmp0 (g_ptr_array_index (self->exit_ids, i), exit_id) == 0) {
				*exit_idx = i;
				return;
			}
		}
	}
}

/*****************************************************************************/
/* interactive browser login (device registration without an auth key) */

#define LOGIN_TIMEOUT_POLLS 180 /* seconds */

static void
get_login_state (char **out_state, char **out_auth_url)
{
	g_autofree char *resp = NULL;
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *node;
	JsonObject *root;

	*out_state = NULL;
	*out_auth_url = NULL;

	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/status", NULL, NULL, NULL);
	if (!resp || !json_parser_load_from_data (parser, resp, -1, NULL))
		return;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return;
	root = json_node_get_object (node);
	*out_state = g_strdup (json_object_get_string_member_with_default (root, "BackendState", ""));
	*out_auth_url = g_strdup (json_object_get_string_member_with_default (root, "AuthURL", ""));
}

/* FALSE when the prefs could not be read — not the same as WantRunning
 * being off, the caller must not act on the pref then */
static gboolean
prefs_get_want_running (gboolean *out_want)
{
	g_autofree char *resp = NULL;
	g_autoptr(JsonParser) parser = json_parser_new ();
	JsonNode *node;

	*out_want = FALSE;
	resp = nm_tailscale_localapi_call ("GET", "/localapi/v0/prefs", NULL, NULL, NULL);
	if (!resp || !json_parser_load_from_data (parser, resp, -1, NULL))
		return FALSE;
	node = json_parser_get_root (parser);
	if (!node || !JSON_NODE_HOLDS_OBJECT (node))
		return FALSE;
	*out_want = json_object_get_boolean_member_with_default (json_node_get_object (node),
	                                                         "WantRunning", FALSE);
	return TRUE;
}

static void
set_want_running (gboolean on)
{
	g_autofree char *resp = NULL;

	resp = nm_tailscale_localapi_call ("PATCH", "/localapi/v0/prefs",
	                                   on ? "{\"WantRunning\":true,\"WantRunningSet\":true}"
	                                      : "{\"WantRunning\":false,\"WantRunningSet\":true}",
	                                   NULL, NULL);
}

static void
login_finish (TailscaleEditor *self, const char *message)
{
	gtk_label_set_text (GTK_LABEL (self->login_status), message);
	gtk_widget_set_sensitive (self->login_button, TRUE);
	self->login_poll_id = 0;
	if (self->restore_down) {
		set_want_running (FALSE);
		self->restore_down = FALSE;
	}
}

static gboolean
login_poll_cb (gpointer user_data)
{
	TailscaleEditor *self = user_data;
	g_autofree char *state = NULL;
	g_autofree char *auth_url = NULL;

	self->login_polls++;
	get_login_state (&state, &auth_url);

	/* a pending AuthURL means the login is not done, no matter what
	 * BackendState claims from cached state */
	if (   !(auth_url && auth_url[0])
	    && (g_strcmp0 (state, "Running") == 0 || g_strcmp0 (state, "Stopped") == 0)) {
		login_finish (self, "Device is registered — you can connect now.");
		return G_SOURCE_REMOVE;
	}
	if (auth_url && auth_url[0] && !self->url_opened) {
		GtkUriLauncher *launcher = gtk_uri_launcher_new (auth_url);

		self->url_opened = TRUE;
		gtk_uri_launcher_launch (launcher, NULL, NULL, NULL, NULL);
		g_object_unref (launcher);
		gtk_label_set_text (GTK_LABEL (self->login_status),
		                    "Complete the login in your browser and keep this dialog "
		                    "open until it confirms the registration…");
	}
	if (self->login_polls >= LOGIN_TIMEOUT_POLLS) {
		login_finish (self, "Timed out waiting for the browser login.");
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void login_start (TailscaleEditor *self);

static void
reap_operator_cb (GPid pid, gint wait_status, gpointer user_data)
{
	g_spawn_close_pid (pid);
}

static void
operator_done_cb (GPid pid, gint wait_status, gpointer user_data)
{
	TailscaleEditor *self = user_data;

	self->operator_watch_id = 0;
	g_spawn_close_pid (pid);
	if (g_spawn_check_wait_status (wait_status, NULL)) {
		login_start (self); /* retry, now as operator */
	} else {
		login_finish (self, "Could not grant LocalAPI access (authentication cancelled?).");
	}
}

/* one-time: make the desktop user the tailscaled operator, authenticated
 * via a polkit system prompt */
static void
grant_operator (TailscaleEditor *self)
{
	g_autofree char *arg = g_strdup_printf ("--operator=%s", g_get_user_name ());
	char *argv[] = { "pkexec", "tailscale", "set", arg, NULL };
	GPid pid;

	if (!g_spawn_async (NULL, argv, NULL,
	                    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
	                    NULL, NULL, &pid, NULL)) {
		login_finish (self, "Could not run pkexec to grant LocalAPI access.");
		return;
	}
	gtk_label_set_text (GTK_LABEL (self->login_status),
	                    "Granting LocalAPI access (system authentication)…");
	self->operator_pid = pid;
	self->operator_watch_id = g_child_watch_add (pid, operator_done_cb, self);
}

static void
login_start (TailscaleEditor *self)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *resp = NULL;
	long http_code = 0;
	gboolean want_running = FALSE;

	gtk_widget_set_sensitive (self->login_button, FALSE);

	/* the login only completes while tailscaled talks to the control
	 * server, which it does not do while stopped — wake it up for the
	 * duration of the login */
	self->restore_down = prefs_get_want_running (&want_running) && !want_running;
	if (self->restore_down)
		set_want_running (TRUE);

	resp = nm_tailscale_localapi_call ("POST", "/localapi/v0/login-interactive", "", &http_code, &error);
	if (!resp) {
		if (http_code == 403 && !self->operator_tried) {
			self->operator_tried = TRUE;
			grant_operator (self);
		} else {
			login_finish (self, error ? error->message : "Unknown error");
		}
		return;
	}

	gtk_label_set_text (GTK_LABEL (self->login_status), "Requesting login link…");
	self->url_opened = FALSE;
	self->login_polls = 0;
	if (!self->login_poll_id)
		self->login_poll_id = g_timeout_add (1000, login_poll_cb, self);
}

static void
login_clicked_cb (GtkButton *button, gpointer user_data)
{
	TailscaleEditor *self = user_data;

	self->operator_tried = FALSE;
	login_start (self);
}

/*****************************************************************************/

static void
dispose (GObject *object)
{
	TailscaleEditor *self = TAILSCALE_EDITOR (object);

	/* deliberately no set_want_running(FALSE) here: if the dialog is closed
	 * mid-login, tailscaled must keep talking to the control server or the
	 * pending browser login can never complete */
	if (self->login_poll_id) {
		g_source_remove (self->login_poll_id);
		self->login_poll_id = 0;
	}
	if (self->operator_watch_id) {
		/* the polkit prompt may outlive the dialog; the child still needs
		 * reaping, but the callback must not touch freed widgets */
		g_source_remove (self->operator_watch_id);
		self->operator_watch_id = 0;
		g_child_watch_add (self->operator_pid, reap_operator_cb, NULL);
	}
	g_clear_object (&self->widget);
	g_clear_pointer (&self->exit_ips, g_ptr_array_unref);
	g_clear_pointer (&self->exit_ids, g_ptr_array_unref);
	G_OBJECT_CLASS (tailscale_editor_parent_class)->dispose (object);
}

static void
tailscale_editor_init (TailscaleEditor *self)
{
}

static void
tailscale_editor_interface_init (NMVpnEditorInterface *iface)
{
	iface->get_widget = get_widget;
	iface->update_connection = update_connection;
}

static void
tailscale_editor_class_init (TailscaleEditorClass *klass)
{
	G_OBJECT_CLASS (klass)->dispose = dispose;
}

G_MODULE_EXPORT NMVpnEditor *
nm_vpn_editor_factory_tailscale (NMVpnEditorPlugin *editor_plugin,
                                 NMConnection *connection,
                                 GError **error)
{
	TailscaleEditor *self;
	GtkWidget *grid, *label, *exit_label, *hint;
	NMSettingVpn *s_vpn;
	const char *key = NULL;
	const char *item;
	gboolean dns = TRUE, routes = FALSE; /* tailscale defaults */
	guint exit_idx = 0;

	g_return_val_if_fail (NM_IS_CONNECTION (connection), NULL);
	g_return_val_if_fail (!error || !*error, NULL);

	self = g_object_new (tailscale_editor_get_type (), NULL);

	grid = gtk_grid_new ();
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);

	label = gtk_label_new_with_mnemonic ("Auth _key");
	gtk_widget_set_halign (label, GTK_ALIGN_START);

	self->entry = gtk_password_entry_new ();
	g_object_set (self->entry,
	              "show-peek-icon", TRUE,
	              "placeholder-text", "tskey-auth-…",
	              NULL);
	gtk_widget_set_hexpand (self->entry, TRUE);
	gtk_label_set_mnemonic_widget (GTK_LABEL (label), self->entry);

	self->login_button = gtk_button_new_with_mnemonic ("Log in via _browser instead…");
	gtk_widget_set_halign (self->login_button, GTK_ALIGN_START);
	self->login_status = gtk_label_new ("");
	gtk_widget_add_css_class (self->login_status, "dim-label");
	gtk_label_set_wrap (GTK_LABEL (self->login_status), TRUE);
	gtk_label_set_xalign (GTK_LABEL (self->login_status), 0.0);

	exit_label = gtk_label_new_with_mnemonic ("E_xit node");
	gtk_widget_set_halign (exit_label, GTK_ALIGN_START);

	self->exit_labels = gtk_string_list_new (NULL);
	/* the dropdown takes ownership of the list model */
	self->exit_combo = gtk_drop_down_new (G_LIST_MODEL (self->exit_labels), NULL);
	combo_append (self, "None");
	self->exit_ips = g_ptr_array_new_with_free_func (g_free);
	self->exit_ids = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (self->exit_ips, g_strdup (""));
	g_ptr_array_add (self->exit_ids, g_strdup (""));
	populate_exit_nodes (self);
	gtk_widget_set_hexpand (self->exit_combo, TRUE);
	gtk_label_set_mnemonic_widget (GTK_LABEL (exit_label), self->exit_combo);

	self->dns_check = gtk_check_button_new_with_mnemonic ("Accept _DNS (MagicDNS)");
	self->routes_check = gtk_check_button_new_with_mnemonic ("Accept advertised _routes");

	hint = gtk_label_new ("Auth key is optional: only needed while tailscaled is logged out. "
	                      "Without a key, use the browser login once.");
	gtk_widget_add_css_class (hint, "dim-label");
	gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
	gtk_label_set_xalign (GTK_LABEL (hint), 0.0);

	gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->entry, 1, 0, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->login_button, 1, 1, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->login_status, 0, 2, 2, 1);
	gtk_grid_attach (GTK_GRID (grid), exit_label, 0, 3, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->exit_combo, 1, 3, 1, 1);
	gtk_grid_attach (GTK_GRID (grid), self->dns_check, 0, 4, 2, 1);
	gtk_grid_attach (GTK_GRID (grid), self->routes_check, 0, 5, 2, 1);
	gtk_grid_attach (GTK_GRID (grid), hint, 0, 6, 2, 1);

	self->widget = g_object_ref_sink (grid);

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn)
		key = nm_setting_vpn_get_secret (s_vpn, NM_TAILSCALE_KEY_AUTH_KEY);
	if (   s_vpn
	    && (   nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_DNS)
	        || nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_ROUTES)
	        || nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_EXIT_NODE))) {
		/* existing connection: show the stored values */
		item = nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_DNS);
		if (item)
			dns = g_strcmp0 (item, "yes") == 0;
		item = nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_ACCEPT_ROUTES);
		if (item)
			routes = g_strcmp0 (item, "yes") == 0;
		item = nm_setting_vpn_get_data_item (s_vpn, NM_TAILSCALE_KEY_EXIT_NODE);
		if (item && item[0]) {
			guint i;

			for (i = 1; i < self->exit_ips->len; i++) {
				if (g_strcmp0 (g_ptr_array_index (self->exit_ips, i), item) == 0) {
					exit_idx = i;
					break;
				}
			}
			if (exit_idx == 0) {
				/* stored exit node is not in the current peer list */
				combo_append (self, item);
				g_ptr_array_add (self->exit_ips, g_strdup (item));
				g_ptr_array_add (self->exit_ids, g_strdup (""));
				exit_idx = self->exit_ips->len - 1;
			}
		}
	} else {
		/* new connection: mirror the current tailscaled prefs */
		prefill_from_prefs (self, &dns, &routes, &exit_idx);
	}
	if (key)
		gtk_editable_set_text (GTK_EDITABLE (self->entry), key);
	gtk_drop_down_set_selected (GTK_DROP_DOWN (self->exit_combo), exit_idx);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (self->dns_check), dns);
	gtk_check_button_set_active (GTK_CHECK_BUTTON (self->routes_check), routes);

	g_signal_connect_swapped (self->entry, "changed", G_CALLBACK (changed_cb), self);
	g_signal_connect (self->login_button, "clicked", G_CALLBACK (login_clicked_cb), self);
	g_signal_connect_swapped (self->exit_combo, "notify::selected", G_CALLBACK (changed_cb), self);
	g_signal_connect_swapped (self->dns_check, "toggled", G_CALLBACK (changed_cb), self);
	g_signal_connect_swapped (self->routes_check, "toggled", G_CALLBACK (changed_cb), self);

	return NM_VPN_EDITOR (self);
}
