#include "tunnelmanager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace {

QByteArray generateRandomHexToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(raw.data()),
                                         reinterpret_cast<quint32*>(raw.data() + raw.size()));
    return raw.toHex();
}

QStringList splitPeers(const QString& text)
{
    QStringList out;
    const auto parts = text.split(QRegularExpression(QStringLiteral("[\\r\\n,;\\s]+")), Qt::SkipEmptyParts);
    for (const auto& p : parts) {
        const auto trimmed = p.trimmed();
        if (!trimmed.isEmpty() && !out.contains(trimmed)) {
            out.append(trimmed);
        }
    }
    return out;
}

} // namespace

TunnelManager::TunnelManager(QObject* parent)
    : QObject(parent)
{
    // Never send localhost tunnel-daemon API calls through a system HTTP proxy.
    m_Network.setProxy(QNetworkProxy::NoProxy);

    loadSettings();

    m_PollTimer.setInterval(1200);
    connect(&m_PollTimer, &QTimer::timeout, this, &TunnelManager::pollStatus);

    connect(&m_Daemon, &QProcess::readyReadStandardOutput, this, [this] {
        m_StdoutBuf += m_Daemon.readAllStandardOutput();
        while (true) {
            const int nl = m_StdoutBuf.indexOf('\n');
            if (nl < 0) {
                break;
            }
            const QByteArray line = m_StdoutBuf.left(nl).trimmed();
            m_StdoutBuf.remove(0, nl + 1);
            if (line.startsWith("READY ")) {
                bool ok = false;
                const quint16 port = line.mid(6).trimmed().toUShort(&ok);
                if (ok && port > 0) {
                    m_DaemonPort = port;
                    m_DaemonReady = true;
                    queryCapabilities();
                    if (m_PendingStart) {
                        m_PendingStart = false;
                        sendStartRequest();
                    } else {
                        pollStatus();
                    }
                    m_PollTimer.start();
                }
            }
        }
    });

    connect(&m_Daemon, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
                m_PollTimer.stop();
                m_DaemonReady = false;
                m_DaemonPort = 0;
                const bool wasConnected = (m_State == QStringLiteral("connected"));
                if (m_State != QStringLiteral("stopped")) {
                    m_State = QStringLiteral("error");
                    if (m_ErrorText.isEmpty()) {
                        m_ErrorText = tr("Tunnel daemon stopped unexpectedly.");
                    }
                }
                m_VirtualIp.clear();
                m_ExposeIp.clear();
                m_AuthUrl.clear();
                m_HostExposed = false;
                emit changed();
                if (wasConnected) {
                    emit connectedStateChanged();
                }
            });
}

TunnelManager::~TunnelManager()
{
    stopDaemon();
}

QString TunnelManager::daemonExecutable() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN32
    const QString name = QStringLiteral("sunandmon-tunnel.exe");
#else
    const QString name = QStringLiteral("sunandmon-tunnel");
#endif
    const QStringList candidates{
        QDir(appDir).filePath(QStringLiteral("tunnel/") + name),
        QDir(appDir).filePath(name),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isFile()) {
            return candidate;
        }
    }
    return {};
}

bool TunnelManager::available() const
{
    return !daemonExecutable().isEmpty();
}

QString TunnelManager::tunnelStateDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir(base.isEmpty() ? QDir::homePath() + QStringLiteral("/.sunandmon") : base);
    const QString path = dir.filePath(QStringLiteral("tunnel"));
    QDir().mkpath(path);
    return path;
}

void TunnelManager::loadSettings()
{
    QSettings settings;
    m_Provider = settings.value(QStringLiteral("tunnel/provider"), QStringLiteral("none")).toString().trimmed().toLower();
    if (m_Provider != QStringLiteral("tailscale") && m_Provider != QStringLiteral("easytier")) {
        m_Provider = QStringLiteral("none");
    }
    m_AutoConnect = settings.value(QStringLiteral("tunnel/autoConnect"), false).toBool();
    m_Hostname = settings.value(QStringLiteral("tunnel/hostname")).toString().trimmed();
    m_TsControlUrl = settings.value(QStringLiteral("tunnel/tsControlUrl")).toString().trimmed();
    m_TsAuthKey = settings.value(QStringLiteral("tunnel/tsAuthKey")).toString().trimmed();
    m_TsUseWebOAuth = settings.value(QStringLiteral("tunnel/tsUseWebOAuth"), true).toBool();
    m_EtNetworkName = settings.value(QStringLiteral("tunnel/etNetworkName")).toString().trimmed();
    m_EtNetworkSecret = settings.value(QStringLiteral("tunnel/etNetworkSecret")).toString().trimmed();
    m_EtPeers = settings.value(QStringLiteral("tunnel/etPeers"), QStringLiteral("tcp://public.easytier.top:11010")).toString().trimmed();
    m_EtVirtualIp = settings.value(QStringLiteral("tunnel/etVirtualIp")).toString().trimmed();
    m_EtCoreBinary = settings.value(QStringLiteral("tunnel/etCoreBinary")).toString().trimmed();
}

void TunnelManager::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("tunnel/provider"), m_Provider);
    settings.setValue(QStringLiteral("tunnel/autoConnect"), m_AutoConnect);
    settings.setValue(QStringLiteral("tunnel/hostname"), m_Hostname);
    settings.setValue(QStringLiteral("tunnel/tsControlUrl"), m_TsControlUrl);
    settings.setValue(QStringLiteral("tunnel/tsAuthKey"), m_TsAuthKey);
    settings.setValue(QStringLiteral("tunnel/tsUseWebOAuth"), m_TsUseWebOAuth);
    settings.setValue(QStringLiteral("tunnel/etNetworkName"), m_EtNetworkName);
    settings.setValue(QStringLiteral("tunnel/etNetworkSecret"), m_EtNetworkSecret);
    settings.setValue(QStringLiteral("tunnel/etPeers"), m_EtPeers);
    settings.setValue(QStringLiteral("tunnel/etVirtualIp"), m_EtVirtualIp);
    settings.setValue(QStringLiteral("tunnel/etCoreBinary"), m_EtCoreBinary);
}

void TunnelManager::startOnLaunch()
{
    if (!available()) {
        return;
    }
    if (m_AutoConnect && m_Provider != QStringLiteral("none")) {
        m_PendingStart = true;
        m_State = QStringLiteral("starting");
        m_ErrorText.clear();
        emit changed();
        ensureDaemonRunning();
    }
}

void TunnelManager::refreshCapabilities()
{
    if (!available()) {
        return;
    }
    if (!m_DaemonReady) {
        ensureDaemonRunning();
    } else {
        queryCapabilities();
    }
}

void TunnelManager::saveAndConnect(const QString& provider,
                                   bool autoConnect,
                                   const QString& hostname,
                                   const QString& tsControlUrl,
                                   const QString& tsAuthKey,
                                   bool tsUseWebOAuth,
                                   const QString& etNetworkName,
                                   const QString& etNetworkSecret,
                                   const QString& etPeers,
                                   const QString& etVirtualIp,
                                   const QString& etCoreBinary)
{
    const QString p = provider.trimmed().toLower();
    m_Provider = (p == QStringLiteral("tailscale") || p == QStringLiteral("easytier")) ? p : QStringLiteral("none");
    m_AutoConnect = autoConnect;
    m_Hostname = hostname.trimmed();
    m_TsControlUrl = tsControlUrl.trimmed();
    m_TsAuthKey = tsAuthKey.trimmed();
    m_TsUseWebOAuth = tsUseWebOAuth;
    m_EtNetworkName = etNetworkName.trimmed();
    m_EtNetworkSecret = etNetworkSecret.trimmed();
    m_EtPeers = etPeers.trimmed();
    m_EtVirtualIp = etVirtualIp.trimmed();
    m_EtCoreBinary = etCoreBinary.trimmed();
    saveSettings();

    if (m_Provider == QStringLiteral("none")) {
        stopTunnel();
        return;
    }
    if (!available()) {
        m_State = QStringLiteral("error");
        m_ErrorText = tr("Built-in tunnel component (sunandmon-tunnel) is not installed.");
        emit changed();
        return;
    }

    m_State = QStringLiteral("starting");
    m_ErrorText.clear();
    m_AuthUrl.clear();
    emit changed();

    if (!m_DaemonReady) {
        m_PendingStart = true;
        ensureDaemonRunning();
    } else {
        sendStartRequest();
    }
}

void TunnelManager::stopTunnel()
{
    m_PendingStart = false;
    const bool wasConnected = (m_State == QStringLiteral("connected"));
    m_State = QStringLiteral("stopped");
    m_VirtualIp.clear();
    m_ExposeIp.clear();
    m_AuthUrl.clear();
    m_ErrorText.clear();
    m_PeersOnline = 0;
    m_HostExposed = false;

    if (m_DaemonReady) {
        auto* reply = apiRequest("POST", QStringLiteral("/v1/stop"));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    emit changed();
    if (wasConnected) {
        emit connectedStateChanged();
    }
}

void TunnelManager::openAuthUrl() const
{
    if (!m_AuthUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_AuthUrl));
    }
}

void TunnelManager::applyCenterDefaults(const QVariantMap& defaults)
{
    const QString provider = defaults.value(QStringLiteral("provider")).toString().trimmed().toLower();
    if (provider != QStringLiteral("tailscale") && provider != QStringLiteral("easytier")) {
        return;
    }
    m_Provider = provider;
    if (provider == QStringLiteral("tailscale")) {
        if (defaults.contains(QStringLiteral("controlUrl"))) {
            m_TsControlUrl = defaults.value(QStringLiteral("controlUrl")).toString().trimmed();
        }
        const QString key = defaults.value(QStringLiteral("authKey")).toString().trimmed();
        if (!key.isEmpty()) {
            m_TsAuthKey = key;
            m_TsUseWebOAuth = false;
        }
    } else if (provider == QStringLiteral("easytier")) {
        m_EtNetworkName = defaults.value(QStringLiteral("networkName")).toString().trimmed();
        m_EtNetworkSecret = defaults.value(QStringLiteral("networkSecret")).toString().trimmed();
        const QVariantList peers = defaults.value(QStringLiteral("peers")).toList();
        QStringList peerList;
        for (const auto& item : peers) {
            const QString s = item.toString().trimmed();
            if (!s.isEmpty()) {
                peerList.append(s);
            }
        }
        if (!peerList.isEmpty()) {
            m_EtPeers = peerList.join(QStringLiteral("\n"));
        }
    }
    saveSettings();
    emit changed();
}

void TunnelManager::ensureDaemonRunning()
{
    if (m_Daemon.state() != QProcess::NotRunning) {
        return;
    }
    const QString exe = daemonExecutable();
    if (exe.isEmpty()) {
        return;
    }
    m_StdoutBuf.clear();
    m_DaemonReady = false;
    m_DaemonPort = 0;
    m_Token = generateRandomHexToken();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("SUNANDMON_TUNNEL_TOKEN"), QString::fromLatin1(m_Token));
    m_Daemon.setProcessEnvironment(env);
    m_Daemon.setWorkingDirectory(QFileInfo(exe).absolutePath());
    m_Daemon.start(exe, {QStringLiteral("-listen"), QStringLiteral("127.0.0.1:0"), QStringLiteral("-watch-stdin")});
}

void TunnelManager::stopDaemon()
{
    m_PollTimer.stop();
    if (m_Daemon.state() != QProcess::NotRunning) {
        m_Daemon.closeWriteChannel();
        if (!m_Daemon.waitForFinished(1500)) {
            m_Daemon.terminate();
            if (!m_Daemon.waitForFinished(1000)) {
                m_Daemon.kill();
                m_Daemon.waitForFinished(1000);
            }
        }
    }
    m_DaemonReady = false;
    m_DaemonPort = 0;
}

QNetworkReply* TunnelManager::apiRequest(const QByteArray& method, const QString& path, const QByteArray& body)
{
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(m_DaemonPort);
    const int qIdx = path.indexOf(QLatin1Char('?'));
    if (qIdx >= 0) {
        url.setPath(path.left(qIdx));
        url.setQuery(path.mid(qIdx + 1));
    } else {
        url.setPath(path);
    }
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + m_Token);
    if (!body.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    return m_Network.sendCustomRequest(req, method, body);
}

void TunnelManager::queryCapabilities()
{
    if (!m_DaemonReady) {
        return;
    }
    QString path = QStringLiteral("/v1/capabilities");
    if (!m_EtCoreBinary.isEmpty()) {
        path += QStringLiteral("?coreBinary=") + QString::fromUtf8(QUrl::toPercentEncoding(m_EtCoreBinary));
    }
    auto* reply = apiRequest("GET", path);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const bool et = obj.value(QStringLiteral("easytier")).toBool();
        if (et != m_EasyTierAvailable) {
            m_EasyTierAvailable = et;
            emit changed();
        }
    });
}

void TunnelManager::sendStartRequest()
{
    if (!m_DaemonReady) {
        return;
    }
    QJsonObject cfg;
    cfg.insert(QStringLiteral("provider"), m_Provider);
    cfg.insert(QStringLiteral("stateDir"), tunnelStateDir());
    cfg.insert(QStringLiteral("hostname"), m_Hostname);

    if (m_Provider == QStringLiteral("tailscale")) {
        cfg.insert(QStringLiteral("controlUrl"), m_TsControlUrl);
        cfg.insert(QStringLiteral("useWebOAuth"), m_TsUseWebOAuth);
        if (!m_TsUseWebOAuth && !m_TsAuthKey.isEmpty()) {
            cfg.insert(QStringLiteral("authKey"), m_TsAuthKey);
        }
    } else if (m_Provider == QStringLiteral("easytier")) {
        cfg.insert(QStringLiteral("networkName"), m_EtNetworkName);
        cfg.insert(QStringLiteral("networkSecret"), m_EtNetworkSecret);
        QJsonArray peersArr;
        for (const auto& p : splitPeers(m_EtPeers)) {
            peersArr.append(p);
        }
        cfg.insert(QStringLiteral("peers"), peersArr);
        cfg.insert(QStringLiteral("virtualIp"), m_EtVirtualIp);
        cfg.insert(QStringLiteral("coreBinary"), m_EtCoreBinary);
    }

    auto* reply = apiRequest("POST", QStringLiteral("/v1/start"), QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code >= 400) {
            m_State = QStringLiteral("error");
            m_ErrorText = obj.value(QStringLiteral("error")).toString();
            if (m_ErrorText.isEmpty()) {
                m_ErrorText = reply->errorString();
            }
            emit changed();
            return;
        }
        pollStatus();
    });
}

void TunnelManager::pollStatus()
{
    if (!m_DaemonReady) {
        return;
    }
    auto* reply = apiRequest("GET", QStringLiteral("/v1/status"));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString newState = obj.value(QStringLiteral("state")).toString(QStringLiteral("stopped"));
        const QString newVip = obj.value(QStringLiteral("virtualIp")).toString();
        const QString newExposeIp = obj.value(QStringLiteral("exposeIp")).toString();
        const QString newAuthUrl = obj.value(QStringLiteral("authUrl")).toString();
        const QString newErr = obj.value(QStringLiteral("error")).toString();
        const int newPeers = obj.value(QStringLiteral("peersOnline")).toInt();
        const bool newHostExposed = obj.value(QStringLiteral("hostExposed")).toBool();

        const bool connChanged = (newState != m_State) || (newExposeIp != m_ExposeIp);
        const bool anyChanged = connChanged || (newVip != m_VirtualIp) || (newAuthUrl != m_AuthUrl) ||
                                (newErr != m_ErrorText) || (newPeers != m_PeersOnline) || (newHostExposed != m_HostExposed);

        m_State = newState;
        m_VirtualIp = newVip;
        m_ExposeIp = newExposeIp;
        m_AuthUrl = newAuthUrl;
        m_ErrorText = newErr;
        m_PeersOnline = newPeers;
        m_HostExposed = newHostExposed;

        if (m_State == QStringLiteral("connected") && m_WantHostExpose && !m_HostExposed && m_HostHttpPort > 0) {
            sendExposeRequest();
        }
        if (anyChanged) {
            emit changed();
        }
        if (connChanged) {
            emit connectedStateChanged();
        }
    });
}

void TunnelManager::syncHostExposure(bool hostReady, quint16 httpPort, quint16 httpsPort)
{
    m_WantHostExpose = hostReady;
    m_HostHttpPort = httpPort;
    m_HostHttpsPort = httpsPort;

    if (!m_DaemonReady || m_State != QStringLiteral("connected")) {
        return;
    }
    if (hostReady && httpPort > 0) {
        sendExposeRequest();
    } else if (!hostReady && m_HostExposed) {
        auto* reply = apiRequest("POST", QStringLiteral("/v1/unexpose"));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
}

void TunnelManager::sendExposeRequest()
{
    if (!m_DaemonReady || m_HostHttpPort == 0) {
        return;
    }
    QJsonObject ports{
        {QStringLiteral("httpPort"), m_HostHttpPort},
        {QStringLiteral("httpsPort"), m_HostHttpsPort},
    };
    auto* reply = apiRequest("POST", QStringLiteral("/v1/expose"), QJsonDocument(ports).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            m_HostExposed = true;
            emit changed();
        }
    });
}

void TunnelManager::ensureForwardAsync(const QString& remoteIp, quint16 httpPort, quint16 httpsPort,
                                       std::function<void(QString localHost, QString error)> callback)
{
    if (!m_DaemonReady || m_State != QStringLiteral("connected") || remoteIp.trimmed().isEmpty()) {
        callback({}, tr("Built-in tunnel is not connected."));
        return;
    }
    QJsonObject body{
        {QStringLiteral("remoteIp"), remoteIp.trimmed()},
        {QStringLiteral("ports"), QJsonObject{
            {QStringLiteral("httpPort"), httpPort},
            {QStringLiteral("httpsPort"), httpsPort},
        }},
    };
    auto* reply = apiRequest("POST", QStringLiteral("/v1/forward"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)]() mutable {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code >= 400) {
            QString err = obj.value(QStringLiteral("error")).toString();
            if (err.isEmpty()) {
                err = reply->errorString();
            }
            callback({}, err);
            return;
        }
        callback(obj.value(QStringLiteral("localHost")).toString(), {});
    });
}
