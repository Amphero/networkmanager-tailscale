/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Metadata-only stub for plasma-nm: before activating a VPN connection,
 * plasma-nm's Handler checks KPluginMetaData::findPlugins("plasma/network/vpn")
 * for a plugin whose X-NetworkManager-Services matches the service type and
 * refuses otherwise — without ever loading the plugin. This stub carries just
 * that metadata so the Plasma applet can connect; it implements no editor
 * (plasma-nm has no public API for that — use nm-connection-editor instead).
 */

#include <QObject>

class TailscaleVpnStub : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.kde.KPluginFactory" FILE "plasmanetworkmanagement_tailscaleui.json")
};

#include "stub.moc"
