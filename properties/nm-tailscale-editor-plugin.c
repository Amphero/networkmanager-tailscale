/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Core editor plugin loaded by libnm/libnma from the .name file. Contains
 * no GTK code itself; get_editor() dlopens the GTK4 editor module next to
 * this library (the same split NetworkManager-openvpn uses).
 */

#define _GNU_SOURCE /* dladdr() */
#include <dlfcn.h>
#include <gmodule.h>
#include <NetworkManager.h>

#include "nm-tailscale.h"

#define TAILSCALE_PLUGIN_NAME "Tailscale"
#define TAILSCALE_PLUGIN_DESC "Connect this machine to your Tailscale network."
#define GTK4_EDITOR_MODULE    "libnm-gtk4-vpn-plugin-tailscale-editor.so"

enum { PROP_0, PROP_NAME, PROP_DESC, PROP_SERVICE };

typedef NMVpnEditor *(*TailscaleEditorFactory) (NMVpnEditorPlugin *editor_plugin,
                                                NMConnection *connection,
                                                GError **error);

typedef struct {
	GObject parent;
} TailscaleEditorPlugin;

typedef struct {
	GObjectClass parent;
} TailscaleEditorPluginClass;

static void tailscale_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface);
GType tailscale_editor_plugin_get_type (void);
G_DEFINE_TYPE_EXTENDED (TailscaleEditorPlugin, tailscale_editor_plugin, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (NM_TYPE_VPN_EDITOR_PLUGIN,
                                               tailscale_editor_plugin_interface_init))

static NMVpnEditor *
get_editor (NMVpnEditorPlugin *iface, NMConnection *connection, GError **error)
{
	static TailscaleEditorFactory factory = NULL;
	gpointer gtk3_symbol = NULL;
	GModule *self_module;

	/* a GTK3 host (nm-connection-editor) cannot load the GTK4 editor */
	self_module = g_module_open (NULL, 0);
	g_module_symbol (self_module, "gtk_container_add", &gtk3_symbol);
	g_module_close (self_module);
	if (gtk3_symbol) {
		g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
		             "the Tailscale plugin only ships a GTK4 editor; "
		             "use GNOME Settings, Plasma System Settings or nmcli");
		return NULL;
	}

	if (!factory) {
		g_autofree char *dir = NULL;
		g_autofree char *path = NULL;
		Dl_info info;
		void *module;

		if (!dladdr (get_editor, &info)) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
			             "unable to determine the plugin path: %s", dlerror ());
			return NULL;
		}
		dir = g_path_get_dirname (info.dli_fname);
		path = g_build_filename (dir, GTK4_EDITOR_MODULE, NULL);

		/* stays loaded forever: the GTypes it registers cannot be unregistered */
		module = dlopen (path, RTLD_LAZY | RTLD_LOCAL);
		if (!module) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
			             "cannot load %s: %s", path, dlerror ());
			return NULL;
		}
		factory = (TailscaleEditorFactory) dlsym (module, "nm_vpn_editor_factory_tailscale");
		if (!factory) {
			g_set_error (error, NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
			             "cannot find nm_vpn_editor_factory_tailscale in %s: %s",
			             path, dlerror ());
			dlclose (module);
			return NULL;
		}
	}
	return factory (iface, connection, error);
}

static guint32
get_capabilities (NMVpnEditorPlugin *iface)
{
	return NM_VPN_EDITOR_PLUGIN_CAPABILITY_NONE;
}

static void
get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, TAILSCALE_PLUGIN_NAME);
		break;
	case PROP_DESC:
		g_value_set_string (value, TAILSCALE_PLUGIN_DESC);
		break;
	case PROP_SERVICE:
		g_value_set_string (value, NM_DBUS_SERVICE_TAILSCALE);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
tailscale_editor_plugin_init (TailscaleEditorPlugin *plugin)
{
}

static void
tailscale_editor_plugin_interface_init (NMVpnEditorPluginInterface *iface)
{
	iface->get_editor = get_editor;
	iface->get_capabilities = get_capabilities;
}

static void
tailscale_editor_plugin_class_init (TailscaleEditorPluginClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = get_property;
	g_object_class_override_property (object_class, PROP_NAME, NM_VPN_EDITOR_PLUGIN_NAME);
	g_object_class_override_property (object_class, PROP_DESC, NM_VPN_EDITOR_PLUGIN_DESCRIPTION);
	g_object_class_override_property (object_class, PROP_SERVICE, NM_VPN_EDITOR_PLUGIN_SERVICE);
}

G_MODULE_EXPORT NMVpnEditorPlugin *
nm_vpn_editor_plugin_factory (GError **error)
{
	g_return_val_if_fail (!error || !*error, NULL);

	return g_object_new (tailscale_editor_plugin_get_type (), NULL);
}
