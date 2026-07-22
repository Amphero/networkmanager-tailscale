/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native plasma-nm VPN UI plugin for Tailscale (System Settings dialog).
 *
 * Built against the private plasma-nm headers of the exact installed
 * version (see reference/plasma-nm) and linked against the installed
 * libplasmanm_editor.so — rebuild after plasma-nm updates. If it ever
 * fails to load, the plasma applet keeps working: the activation check
 * only reads the embedded metadata (see stub.cpp fallback).
 */

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <KPluginFactory>

#include "settingwidget.h"
#include "vpnuiplugin.h"

#include "nm-tailscale.h"
#include "nm-tailscale-localapi.h"

/*****************************************************************************/
/* thin Qt wrapper around the shared C LocalAPI client */

static QByteArray localapiCall(const char *method, const char *path, const QByteArray &body, long *httpCode = nullptr, bool *transportOk = nullptr)
{
    long code = 0;
    char *resp = nm_tailscale_localapi_call(method, path, body.isNull() ? nullptr : body.constData(), &code, nullptr);
    const QByteArray result(resp ? resp : "");

    g_free(resp);
    if (httpCode)
        *httpCode = code;
    if (transportOk)
        *transportOk = (code != 0); /* 0 = tailscaled not reachable */
    return resp ? result : QByteArray();
}

static QJsonObject localapiGet(const char *path)
{
    return QJsonDocument::fromJson(localapiCall("GET", path, QByteArray())).object();
}

static QString firstV4(const QJsonObject &obj, const char *member)
{
    const auto ips = obj.value(QLatin1String(member)).toArray();
    for (const auto &v : ips) {
        const QString s = v.toString();
        if (s.contains(QLatin1Char('.')))
            return s;
    }
    return {};
}

/*****************************************************************************/

class TailscaleWidget : public SettingWidget
{
    Q_OBJECT
public:
    ~TailscaleWidget() override
    {
        if (m_pollTimer->isActive() && m_restoreDown)
            setWantRunning(false);
    }

    explicit TailscaleWidget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent = nullptr)
        : SettingWidget(setting, parent)
        , m_setting(setting)
    {
        auto *layout = new QFormLayout(this);

        m_authKey = new QLineEdit(this);
        m_authKey->setEchoMode(QLineEdit::Password);
        m_authKey->setPlaceholderText(QStringLiteral("tskey-auth-…"));
        layout->addRow(QStringLiteral("Auth key:"), m_authKey);

        m_loginButton = new QPushButton(QStringLiteral("Log in via browser instead…"), this);
        layout->addRow(QString(), m_loginButton);
        m_loginStatus = new QLabel(this);
        m_loginStatus->setWordWrap(true);
        layout->addRow(QString(), m_loginStatus);

        m_exitNode = new QComboBox(this);
        m_exitNode->addItem(QStringLiteral("None"), QString());
        m_exitIds << QString();
        layout->addRow(QStringLiteral("Exit node:"), m_exitNode);

        m_dns = new QCheckBox(QStringLiteral("Accept DNS (MagicDNS)"), this);
        m_dns->setChecked(true);
        layout->addRow(QString(), m_dns);
        m_routes = new QCheckBox(QStringLiteral("Accept advertised routes"), this);
        layout->addRow(QString(), m_routes);

        auto *hint = new QLabel(QStringLiteral("Auth key is optional: only needed while tailscaled is logged out. "
                                               "Without a key, use the browser login once."),
                                this);
        hint->setWordWrap(true);
        hint->setEnabled(false);
        layout->addRow(QString(), hint);

        populateExitNodes();

        m_pollTimer = new QTimer(this);
        m_pollTimer->setInterval(1000);
        connect(m_pollTimer, &QTimer::timeout, this, &TailscaleWidget::pollLogin);
        connect(m_loginButton, &QPushButton::clicked, this, [this] {
            m_operatorTried = false;
            doLogin();
        });

        watchChangedSetting();

        if (setting && !setting->isNull())
            loadConfig(setting);
        else
            prefillFromPrefs(); /* brand-new connection: loadConfig is never called */
    }

    void loadConfig(const NetworkManager::Setting::Ptr &setting) override
    {
        const NMStringMap data = m_setting->data();

        if (data.contains(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_DNS)) || data.contains(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_ROUTES))
            || data.contains(QLatin1String(NM_TAILSCALE_KEY_EXIT_NODE))) {
            /* existing connection: show the stored values */
            if (data.contains(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_DNS)))
                m_dns->setChecked(data.value(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_DNS)) == QLatin1String("yes"));
            if (data.contains(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_ROUTES)))
                m_routes->setChecked(data.value(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_ROUTES)) == QLatin1String("yes"));
            const QString exitNode = data.value(QLatin1String(NM_TAILSCALE_KEY_EXIT_NODE));
            if (!exitNode.isEmpty())
                selectExitNodeByIp(exitNode, true);
        } else {
            prefillFromPrefs();
        }
        loadSecrets(setting);
    }

    void loadSecrets(const NetworkManager::Setting::Ptr &setting) override
    {
        const auto vpnSetting = setting.staticCast<NetworkManager::VpnSetting>();
        if (vpnSetting) {
            const QString key = vpnSetting->secrets().value(QLatin1String(NM_TAILSCALE_KEY_AUTH_KEY));
            if (!key.isEmpty())
                m_authKey->setText(key);
        }
    }

    QVariantMap setting() const override
    {
        NetworkManager::VpnSetting setting;
        setting.setServiceType(QLatin1String(NM_DBUS_SERVICE_TAILSCALE));

        NMStringMap data;
        NMStringMap secrets;
        data.insert(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_DNS), m_dns->isChecked() ? QStringLiteral("yes") : QStringLiteral("no"));
        data.insert(QLatin1String(NM_TAILSCALE_KEY_ACCEPT_ROUTES), m_routes->isChecked() ? QStringLiteral("yes") : QStringLiteral("no"));
        data.insert(QLatin1String(NM_TAILSCALE_KEY_EXIT_NODE), m_exitNode->currentData().toString());
        if (!m_authKey->text().isEmpty()) {
            secrets.insert(QLatin1String(NM_TAILSCALE_KEY_AUTH_KEY), m_authKey->text());
            /* system-owned: the root service daemon must get it without an agent */
            data.insert(QLatin1String(NM_TAILSCALE_KEY_AUTH_KEY "-flags"), QString::number(NetworkManager::Setting::None));
        }
        setting.setData(data);
        setting.setSecrets(secrets);
        return setting.toMap();
    }

private:
    void populateExitNodes()
    {
        const QJsonObject peers = localapiGet("/localapi/v0/status").value(QLatin1String("Peer")).toObject();
        for (auto it = peers.begin(); it != peers.end(); ++it) {
            const QJsonObject peer = it.value().toObject();
            if (!peer.value(QLatin1String("ExitNodeOption")).toBool())
                continue;
            const QString ip = firstV4(peer, "TailscaleIPs");
            if (ip.isEmpty())
                continue;
            m_exitNode->addItem(QStringLiteral("%1 (%2)").arg(peer.value(QLatin1String("HostName")).toString(), ip), ip);
            m_exitIds << peer.value(QLatin1String("ID")).toString();
        }
    }

    void selectExitNodeByIp(const QString &ip, bool addIfMissing)
    {
        const int idx = m_exitNode->findData(ip);
        if (idx >= 0) {
            m_exitNode->setCurrentIndex(idx);
        } else if (addIfMissing) {
            /* stored exit node is not in the current peer list */
            m_exitNode->addItem(ip, ip);
            m_exitIds << QString();
            m_exitNode->setCurrentIndex(m_exitNode->count() - 1);
        }
    }

    /* initial values for a freshly created connection: mirror the current
     * tailscaled prefs so the first connect is behavior-neutral */
    void prefillFromPrefs()
    {
        const QJsonObject prefs = localapiGet("/localapi/v0/prefs");
        if (prefs.isEmpty())
            return;
        m_dns->setChecked(prefs.value(QLatin1String("CorpDNS")).toBool(true));
        m_routes->setChecked(prefs.value(QLatin1String("RouteAll")).toBool(false));

        const QString exitIp = prefs.value(QLatin1String("ExitNodeIP")).toString();
        const QString exitId = prefs.value(QLatin1String("ExitNodeID")).toString();
        if (!exitIp.isEmpty()) {
            selectExitNodeByIp(exitIp, true);
        } else if (!exitId.isEmpty()) {
            /* `tailscale set --exit-node` stores the stable ID, not the IP */
            const int idx = int(m_exitIds.indexOf(exitId));
            if (idx > 0)
                m_exitNode->setCurrentIndex(idx);
        }
    }

    /* interactive browser login (device registration without an auth key) */

    void setWantRunning(bool on)
    {
        localapiCall("PATCH", "/localapi/v0/prefs",
                     on ? QByteArray("{\"WantRunning\":true,\"WantRunningSet\":true}")
                        : QByteArray("{\"WantRunning\":false,\"WantRunningSet\":true}"));
    }

    void doLogin()
    {
        long code = 0;
        bool transportOk = false;

        m_loginButton->setEnabled(false);

        /* the login only completes while tailscaled talks to the control
         * server, which it does not do while stopped — wake it up for the
         * duration of the login */
        m_restoreDown = !localapiGet("/localapi/v0/prefs").value(QLatin1String("WantRunning")).toBool(false);
        if (m_restoreDown)
            setWantRunning(true);

        localapiCall("POST", "/localapi/v0/login-interactive", QByteArray(""), &code, &transportOk);
        if (!transportOk) {
            finishLogin(QStringLiteral("tailscaled is not reachable — is tailscale installed and tailscaled.service running?"));
            return;
        }
        if (code == 403 && !m_operatorTried) {
            m_operatorTried = true;
            grantOperator();
            return;
        }
        if (code < 200 || code > 299) {
            finishLogin(QStringLiteral("LocalAPI error (HTTP %1)").arg(code));
            return;
        }
        m_loginStatus->setText(QStringLiteral("Requesting login link…"));
        m_urlOpened = false;
        m_polls = 0;
        m_pollTimer->start();
    }

    /* one-time: make the desktop user the tailscaled operator, authenticated
     * via a polkit system prompt */
    void grantOperator()
    {
        auto *proc = new QProcess(this);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, proc](int exitCode, QProcess::ExitStatus) {
            proc->deleteLater();
            if (exitCode == 0)
                doLogin(); /* retry, now as operator */
            else
                finishLogin(QStringLiteral("Could not grant LocalAPI access (authentication cancelled?)."));
        });
        m_loginStatus->setText(QStringLiteral("Granting LocalAPI access (system authentication)…"));
        proc->start(QStringLiteral("pkexec"), {QStringLiteral("tailscale"), QStringLiteral("set"), QStringLiteral("--operator=") + qEnvironmentVariable("USER")});
    }

    void pollLogin()
    {
        m_polls++;
        const QJsonObject status = localapiGet("/localapi/v0/status");
        const QString state = status.value(QLatin1String("BackendState")).toString();
        const QString authUrl = status.value(QLatin1String("AuthURL")).toString();

        /* a pending AuthURL means the login is not done, no matter what
         * BackendState claims from cached state */
        if (authUrl.isEmpty() && (state == QLatin1String("Running") || state == QLatin1String("Stopped"))) {
            m_pollTimer->stop();
            finishLogin(QStringLiteral("Device is registered — you can connect now."));
            return;
        }
        if (!authUrl.isEmpty() && !m_urlOpened) {
            m_urlOpened = true;
            QDesktopServices::openUrl(QUrl(authUrl));
            m_loginStatus->setText(QStringLiteral("Complete the login in your browser…"));
        }
        if (m_polls >= 180) {
            m_pollTimer->stop();
            finishLogin(QStringLiteral("Timed out waiting for the browser login."));
        }
    }

    void finishLogin(const QString &message)
    {
        m_loginStatus->setText(message);
        m_loginButton->setEnabled(true);
        if (m_restoreDown) {
            setWantRunning(false);
            m_restoreDown = false;
        }
    }

    NetworkManager::VpnSetting::Ptr m_setting;
    QLineEdit *m_authKey;
    QPushButton *m_loginButton;
    QLabel *m_loginStatus;
    QComboBox *m_exitNode;
    QStringList m_exitIds; /* stable node IDs, parallel to combo entries */
    QCheckBox *m_dns;
    QCheckBox *m_routes;
    QTimer *m_pollTimer;
    int m_polls = 0;
    bool m_urlOpened = false;
    bool m_operatorTried = false;
    bool m_restoreDown = false;
};

/*****************************************************************************/

class TailscaleUiPlugin : public VpnUiPlugin
{
    Q_OBJECT
public:
    explicit TailscaleUiPlugin(QObject *parent = nullptr, const QVariantList & = QVariantList())
        : VpnUiPlugin(parent)
    {
    }

    SettingWidget *widget(const NetworkManager::VpnSetting::Ptr &setting, QWidget *parent) override
    {
        return new TailscaleWidget(setting, parent);
    }

    SettingWidget *askUser(const NetworkManager::VpnSetting::Ptr &, const QStringList &, QWidget *) override
    {
        /* never called: the auth key is system-owned, no agent secrets */
        return nullptr;
    }

    QString suggestedFileName(const NetworkManager::ConnectionSettings::Ptr &) const override
    {
        return {};
    }
};

K_PLUGIN_CLASS_WITH_JSON(TailscaleUiPlugin, "plasmanetworkmanagement_tailscaleui.json")

#include "tailscaleui.moc"
