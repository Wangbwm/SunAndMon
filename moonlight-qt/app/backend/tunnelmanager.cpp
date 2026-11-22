#include "tunnelmanager.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <memory>

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

QString normalizeRoutePolicy(const QString& p)
{
    const QString v = p.trimmed().toLower();
    if (v == QStringLiteral("auto") || v == QStringLiteral("auto_ts") ||
        v == QStringLiteral("prefer_ts") || v == QStringLiteral("prefer_et")) {
        return v;
    }
    return QStringLiteral("auto");
}

} // namespace

TunnelManager::TunnelManager(QObject* parent)
    : QObject(parent)
{
    // Never route localhost tunnel-daemon API calls through a system proxy.
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
                    m_AttachedToExistingDaemon = false;
                    m_PollFailures = 0;
                    writeDaemonEndpointFile();
                    queryCapabilities();
                    if (m_PendingStartTs) {
                        m_PendingStartTs = false;
                        sendStartTailscale();
                    }
                    if (m_PendingStartEt) {
                        m_PendingStartEt = false;
                        sendStartEasyTier();
                    }
                    pollStatus();
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
                const bool wasAnyConnected = (m_TsState == QStringLiteral("connected") || m_EtState == QStringLiteral("connected"));
                if (m_TsState != QStringLiteral("stopped")) {
                    m_TsState = QStringLiteral("error");
                    if (m_TsError.isEmpty()) m_TsError = tr("Tunnel daemon stopped unexpectedly.");
                }
                if (m_EtState != QStringLiteral("stopped")) {
                    m_EtState = QStringLiteral("error");
                    if (m_EtError.isEmpty()) m_EtError = tr("Tunnel daemon stopped unexpectedly.");
                }
                m_TsVirtualIp.clear(); m_TsExposeIp.clear(); m_TsAuthUrl.clear(); m_TsHostExposed = false;
                m_EtVirtualIp.clear(); m_EtExposeIp.clear(); m_EtHostExposed = false;
                emit changed();
                if (wasAnyConnected) {
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

QString TunnelManager::state() const
{
    if (m_TsState == QStringLiteral("connected") || m_EtState == QStringLiteral("connected")) {
        return QStringLiteral("connected");
    }
    if (m_TsState == QStringLiteral("needs_auth")) {
        return QStringLiteral("needs_auth");
    }
    if (m_TsState == QStringLiteral("starting") || m_EtState == QStringLiteral("starting")) {
        return QStringLiteral("starting");
    }
    if (m_TsState == QStringLiteral("error") || m_EtState == QStringLiteral("error")) {
        return QStringLiteral("error");
    }
    return QStringLiteral("stopped");
}

QString TunnelManager::summaryBadge() const
{
    const bool tsUp = m_TsState == QStringLiteral("connected");
    const bool etUp = m_EtState == QStringLiteral("connected");
    if (tsUp && etUp) return QStringLiteral("TS + ET");
    if (tsUp) return QStringLiteral("Tailscale");
    if (etUp) return QStringLiteral("EasyTier");
    if (m_TsState == QStringLiteral("needs_auth")) return tr("Sign in");
    if (m_TsState == QStringLiteral("starting") || m_EtState == QStringLiteral("starting")) return tr("Connecting");
    if (m_TsState == QStringLiteral("error") || m_EtState == QStringLiteral("error")) return tr("Error");
    return {};
}

void TunnelManager::setRoutePolicy(const QString& policy)
{
    const QString norm = normalizeRoutePolicy(policy);
    if (norm != m_RoutePolicy) {
        m_RoutePolicy = norm;
        saveSettings();
        emit changed();
    }
}

bool TunnelManager::preferTunnelFirst() const
{
    return m_RoutePolicy == QStringLiteral("prefer_ts") || m_RoutePolicy == QStringLiteral("prefer_et");
}

QStringList TunnelManager::orderedTunnelProviders() const
{
    if (m_RoutePolicy == QStringLiteral("auto_ts") || m_RoutePolicy == QStringLiteral("prefer_ts")) {
        return {QStringLiteral("tailscale"), QStringLiteral("easytier")};
    }
    // Default ("auto" or "prefer_et"): EasyTier first (optimized UDP), Tailscale seamless failover.
    return {QStringLiteral("easytier"), QStringLiteral("tailscale")};
}

bool TunnelManager::isProviderConnected(const QString& provider) const
{
    if (provider == QStringLiteral("tailscale")) {
        return m_TsState == QStringLiteral("connected");
    }
    if (provider == QStringLiteral("easytier")) {
        return m_EtState == QStringLiteral("connected");
    }
    return false;
}

QList<QPair<QString, QString>> TunnelManager::connectedHostTunnels() const
{
    QList<QPair<QString, QString>> out;
    for (const auto& p : orderedTunnelProviders()) {
        if (p == QStringLiteral("tailscale") && m_TsState == QStringLiteral("connected") && !m_TsExposeIp.isEmpty()) {
            out.append({p, m_TsExposeIp});
        } else if (p == QStringLiteral("easytier") && m_EtState == QStringLiteral("connected") && !m_EtExposeIp.isEmpty()) {
            out.append({p, m_EtExposeIp});
        }
    }
    return out;
}

QString TunnelManager::tunnelStateDir() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir dir(base.isEmpty() ? QDir::homePath() + QStringLiteral("/.sunandmon") : base);
    const QString path = dir.filePath(QStringLiteral("tunnel"));
    QDir().mkpath(path);
    return path;
}

QString TunnelManager::daemonEndpointFile() const
{
    return QDir(tunnelStateDir()).filePath(QStringLiteral("daemon.json"));
}

void TunnelManager::writeDaemonEndpointFile() const
{
    if (m_DaemonPort == 0 || m_Token.isEmpty() || m_AttachedToExistingDaemon) {
        return;
    }
    const QString path = daemonEndpointFile();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QJsonObject obj{
        {QStringLiteral("port"), static_cast<int>(m_DaemonPort)},
        {QStringLiteral("token"), QString::fromLatin1(m_Token)},
        {QStringLiteral("ownerPid"), QCoreApplication::applicationPid()},
    };
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (file.commit()) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}

void TunnelManager::removeDaemonEndpointFileIfOwned() const
{
    const QString path = daemonEndpointFile();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const auto obj = QJsonDocument::fromJson(file.read(4096)).object();
    file.close();
    if (obj.value(QStringLiteral("ownerPid")).toVariant().toLongLong() == QCoreApplication::applicationPid()) {
        QFile::remove(path);
    }
}

void TunnelManager::loadSettings()
{
    QSettings settings;
    m_RoutePolicy = normalizeRoutePolicy(settings.value(QStringLiteral("tunnel/routePolicy"), QStringLiteral("auto")).toString());
    m_Hostname = settings.value(QStringLiteral("tunnel/hostname")).toString().trimmed();

    // Migrate legacy single-provider autoConnect if present
    const QString legacyProvider = settings.value(QStringLiteral("tunnel/provider")).toString().trimmed().toLower();
    const bool legacyAuto = settings.value(QStringLiteral("tunnel/autoConnect"), false).toBool();

    m_TsAutoConnect = settings.value(QStringLiteral("tunnel/tsAutoConnect"),
                                     legacyAuto && legacyProvider == QStringLiteral("tailscale")).toBool();
    m_TsControlUrl = settings.value(QStringLiteral("tunnel/tsControlUrl")).toString().trimmed();
    m_TsAuthKey = settings.value(QStringLiteral("tunnel/tsAuthKey")).toString().trimmed();
    m_TsUseWebOAuth = settings.value(QStringLiteral("tunnel/tsUseWebOAuth"), true).toBool();

    m_EtAutoConnect = settings.value(QStringLiteral("tunnel/etAutoConnect"),
                                     legacyAuto && legacyProvider == QStringLiteral("easytier")).toBool();
    m_EtNetworkName = settings.value(QStringLiteral("tunnel/etNetworkName")).toString().trimmed();
    m_EtNetworkSecret = settings.value(QStringLiteral("tunnel/etNetworkSecret")).toString().trimmed();
    const QVariant rawPeers = settings.value(QStringLiteral("tunnel/etPeers"), QStringLiteral("tcp://public.easytier.top:11010"));
    if (rawPeers.typeId() == QMetaType::QStringList) {
        m_EtPeers = rawPeers.toStringList().join(QStringLiteral(" ")).trimmed();
    } else {
        m_EtPeers = rawPeers.toString().trimmed();
    }
    m_EtStaticIp = settings.value(QStringLiteral("tunnel/etVirtualIp")).toString().trimmed();
    m_EtCoreBinary = settings.value(QStringLiteral("tunnel/etCoreBinary")).toString().trimmed();
}

void TunnelManager::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("tunnel/routePolicy"), m_RoutePolicy);
    settings.setValue(QStringLiteral("tunnel/hostname"), m_Hostname);
    settings.setValue(QStringLiteral("tunnel/tsAutoConnect"), m_TsAutoConnect);
    settings.setValue(QStringLiteral("tunnel/tsControlUrl"), m_TsControlUrl);
    settings.setValue(QStringLiteral("tunnel/tsAuthKey"), m_TsAuthKey);
    settings.setValue(QStringLiteral("tunnel/tsUseWebOAuth"), m_TsUseWebOAuth);
    settings.setValue(QStringLiteral("tunnel/etAutoConnect"), m_EtAutoConnect);
    settings.setValue(QStringLiteral("tunnel/etNetworkName"), m_EtNetworkName);
    settings.setValue(QStringLiteral("tunnel/etNetworkSecret"), m_EtNetworkSecret);
    settings.setValue(QStringLiteral("tunnel/etPeers"), m_EtPeers);
    settings.setValue(QStringLiteral("tunnel/etVirtualIp"), m_EtStaticIp);
    settings.setValue(QStringLiteral("tunnel/etCoreBinary"), m_EtCoreBinary);
}

void TunnelManager::startOnLaunch(bool secondaryInstance)
{
    if (!available()) {
        return;
    }
    m_SecondaryMode = secondaryInstance;
    bool needDaemon = false;
    if (m_TsAutoConnect) {
        m_PendingStartTs = true;
        m_TsState = QStringLiteral("starting");
        m_TsError.clear();
        needDaemon = true;
    }
    if (m_EtAutoConnect && !m_EtNetworkName.isEmpty()) {
        m_PendingStartEt = true;
        m_EtState = QStringLiteral("starting");
        m_EtError.clear();
        needDaemon = true;
    }
    if (needDaemon) {
        emit changed();
        tryAttachExistingDaemon(true);
    } else if (secondaryInstance) {
        // Always attach to the primary instance's running tunnel daemon if present,
        // even when auto-connect wasn't enabled in settings.
        tryAttachExistingDaemon(false);
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

void TunnelManager::saveAndConnectTailscale(bool autoConnect,
                                            const QString& hostname,
                                            const QString& controlUrl,
                                            const QString& authKey,
                                            bool useWebOAuth)
{
    m_TsAutoConnect = autoConnect;
    if (!hostname.trimmed().isEmpty() || m_Hostname != hostname.trimmed()) {
        m_Hostname = hostname.trimmed();
    }
    m_TsControlUrl = controlUrl.trimmed();
    m_TsAuthKey = authKey.trimmed();
    m_TsUseWebOAuth = useWebOAuth;
    saveSettings();

    if (!available()) {
        m_TsState = QStringLiteral("error");
        m_TsError = tr("Built-in tunnel component (sunandmon-tunnel) is not installed.");
        emit changed();
        return;
    }

    m_TsState = QStringLiteral("starting");
    m_TsError.clear();
    m_TsAuthUrl.clear();
    emit changed();

    if (!m_DaemonReady) {
        m_PendingStartTs = true;
        ensureDaemonRunning();
    } else {
        sendStartTailscale();
    }
}

void TunnelManager::stopTailscale()
{
    m_PendingStartTs = false;
    const bool wasConnected = (m_TsState == QStringLiteral("connected"));
    m_TsState = QStringLiteral("stopped");
    m_TsVirtualIp.clear();
    m_TsExposeIp.clear();
    m_TsAuthUrl.clear();
    m_TsError.clear();
    m_TsPeersOnline = 0;
    m_TsHostExposed = false;
    sendStopProvider(QStringLiteral("tailscale"));
    emit changed();
    if (wasConnected) {
        emit connectedStateChanged();
    }
}

void TunnelManager::saveAndConnectEasyTier(bool autoConnect,
                                           const QString& hostname,
                                           const QString& networkName,
                                           const QString& networkSecret,
                                           const QString& peers,
                                           const QString& staticIp,
                                           const QString& coreBinary)
{
    m_EtAutoConnect = autoConnect;
    if (!hostname.trimmed().isEmpty() || m_Hostname != hostname.trimmed()) {
        m_Hostname = hostname.trimmed();
    }
    m_EtNetworkName = networkName.trimmed();
    m_EtNetworkSecret = networkSecret.trimmed();
    m_EtPeers = peers.trimmed();
    m_EtStaticIp = staticIp.trimmed();
    m_EtCoreBinary = coreBinary.trimmed();
    saveSettings();

    if (!available()) {
        m_EtState = QStringLiteral("error");
        m_EtError = tr("Built-in tunnel component (sunandmon-tunnel) is not installed.");
        emit changed();
        return;
    }

    m_EtState = QStringLiteral("starting");
    m_EtError.clear();
    emit changed();

    if (!m_DaemonReady) {
        m_PendingStartEt = true;
        ensureDaemonRunning();
    } else {
        sendStartEasyTier();
    }
}

void TunnelManager::stopEasyTier()
{
    m_PendingStartEt = false;
    const bool wasConnected = (m_EtState == QStringLiteral("connected"));
    m_EtState = QStringLiteral("stopped");
    m_EtVirtualIp.clear();
    m_EtExposeIp.clear();
    m_EtError.clear();
    m_EtPeersOnline = 0;
    m_EtHostExposed = false;
    sendStopProvider(QStringLiteral("easytier"));
    emit changed();
    if (wasConnected) {
        emit connectedStateChanged();
    }
}

void TunnelManager::stopAll()
{
    stopTailscale();
    stopEasyTier();
}

void TunnelManager::openTailscaleAuthUrl() const
{
    if (!m_TsAuthUrl.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_TsAuthUrl));
    }
}

void TunnelManager::applyCenterDefaults(const QVariantMap& defaults)
{
    const QString provider = defaults.value(QStringLiteral("provider")).toString().trimmed().toLower();
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
            if (!s.isEmpty()) peerList.append(s);
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
    if (m_DaemonReady || m_Daemon.state() != QProcess::NotRunning || m_ProbingExistingDaemon) {
        return;
    }
    tryAttachExistingDaemon(true);
}

void TunnelManager::tryAttachExistingDaemon(bool spawnFallback)
{
    if (m_DaemonReady || m_Daemon.state() != QProcess::NotRunning || m_ProbingExistingDaemon) {
        return;
    }
    QFile file(daemonEndpointFile());
    if (!file.open(QIODevice::ReadOnly)) {
        if (spawnFallback) {
            spawnLocalDaemon();
        }
        return;
    }
    const auto obj = QJsonDocument::fromJson(file.read(4096)).object();
    file.close();
    const int port = obj.value(QStringLiteral("port")).toInt();
    const QByteArray token = obj.value(QStringLiteral("token")).toString().toLatin1();
    const qint64 ownerPid = obj.value(QStringLiteral("ownerPid")).toVariant().toLongLong();
    if (port <= 0 || port > 65535 || token.isEmpty() || ownerPid == QCoreApplication::applicationPid()) {
        if (spawnFallback) {
            spawnLocalDaemon();
        }
        return;
    }

    m_ProbingExistingDaemon = true;
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(QStringLiteral("/v1/status"));
    QNetworkRequest req(url);
    req.setRawHeader("Authorization", "Bearer " + token);
    req.setTransferTimeout(800);
    auto* reply = m_Network.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, port, token, spawnFallback] {
        reply->deleteLater();
        m_ProbingExistingDaemon = false;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && code == 200) {
            const auto root = QJsonDocument::fromJson(reply->readAll()).object();
            m_AttachedToExistingDaemon = true;
            m_DaemonPort = static_cast<quint16>(port);
            m_Token = token;
            m_DaemonReady = true;
            m_PollFailures = 0;
            applyStatusPayload(root);
            queryCapabilities();
            if (m_PendingStartTs) {
                m_PendingStartTs = false;
                if (m_TsState == QStringLiteral("stopped") || m_TsState == QStringLiteral("error")) {
                    sendStartTailscale();
                }
            }
            if (m_PendingStartEt) {
                m_PendingStartEt = false;
                if (m_EtState == QStringLiteral("stopped") || m_EtState == QStringLiteral("error")) {
                    sendStartEasyTier();
                }
            }
            m_PollTimer.start();
            emit daemonAttachFinished();
            return;
        }
        // Stale descriptor or primary daemon exited; spawn locally if requested.
        if (reply->error() == QNetworkReply::ConnectionRefusedError) {
            QFile::remove(daemonEndpointFile());
        }
        if (spawnFallback) {
            spawnLocalDaemon();
        }
        emit daemonAttachFinished();
    });
}

void TunnelManager::spawnLocalDaemon()
{
    if (m_Daemon.state() != QProcess::NotRunning || m_DaemonReady) {
        return;
    }
    const QString exe = daemonExecutable();
    if (exe.isEmpty()) {
        return;
    }
    m_StdoutBuf.clear();
    m_DaemonReady = false;
    m_AttachedToExistingDaemon = false;
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
    if (m_AttachedToExistingDaemon) {
        m_AttachedToExistingDaemon = false;
        m_DaemonReady = false;
        m_DaemonPort = 0;
        return;
    }
    removeDaemonEndpointFileIfOwned();
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

void TunnelManager::sendStartTailscale()
{
    if (!m_DaemonReady) {
        return;
    }
    QJsonObject cfg{
        {QStringLiteral("provider"), QStringLiteral("tailscale")},
        {QStringLiteral("stateDir"), tunnelStateDir()},
        {QStringLiteral("hostname"), m_Hostname},
        {QStringLiteral("controlUrl"), m_TsControlUrl},
        {QStringLiteral("useWebOAuth"), m_TsUseWebOAuth},
    };
    if (!m_TsUseWebOAuth && !m_TsAuthKey.isEmpty()) {
        cfg.insert(QStringLiteral("authKey"), m_TsAuthKey);
    }
    auto* reply = apiRequest("POST", QStringLiteral("/v1/start"), QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code >= 400) {
            m_TsState = QStringLiteral("error");
            m_TsError = obj.value(QStringLiteral("error")).toString();
            if (m_TsError.isEmpty()) m_TsError = reply->errorString();
            emit changed();
            return;
        }
        pollStatus();
    });
}

void TunnelManager::sendStartEasyTier()
{
    if (!m_DaemonReady) {
        return;
    }
    QJsonArray peersArr;
    for (const auto& p : splitPeers(m_EtPeers)) {
        peersArr.append(p);
    }
    QJsonObject cfg{
        {QStringLiteral("provider"), QStringLiteral("easytier")},
        {QStringLiteral("stateDir"), tunnelStateDir()},
        {QStringLiteral("hostname"), m_Hostname},
        {QStringLiteral("networkName"), m_EtNetworkName},
        {QStringLiteral("networkSecret"), m_EtNetworkSecret},
        {QStringLiteral("peers"), peersArr},
        {QStringLiteral("virtualIp"), m_EtStaticIp},
        {QStringLiteral("coreBinary"), m_EtCoreBinary},
    };
    auto* reply = apiRequest("POST", QStringLiteral("/v1/start"), QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || code >= 400) {
            m_EtState = QStringLiteral("error");
            m_EtError = obj.value(QStringLiteral("error")).toString();
            if (m_EtError.isEmpty()) m_EtError = reply->errorString();
            emit changed();
            return;
        }
        pollStatus();
    });
}

void TunnelManager::sendStopProvider(const QString& provider)
{
    if (!m_DaemonReady) {
        return;
    }
    const QJsonObject body{{QStringLiteral("provider"), provider}};
    auto* reply = apiRequest("POST", QStringLiteral("/v1/stop"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
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
            if (m_AttachedToExistingDaemon && ++m_PollFailures >= 3) {
                m_PollTimer.stop();
                m_AttachedToExistingDaemon = false;
                m_DaemonReady = false;
                m_DaemonPort = 0;
                emit changed();
            }
            return;
        }
        m_PollFailures = 0;
        const auto root = QJsonDocument::fromJson(reply->readAll()).object();
        applyStatusPayload(root);
    });
}

void TunnelManager::applyStatusPayload(const QJsonObject& root)
{
    const auto providers = root.value(QStringLiteral("providers")).toObject();
    const auto ts = providers.value(QStringLiteral("tailscale")).toObject();
    const auto et = providers.value(QStringLiteral("easytier")).toObject();

    const QString newTsState = ts.value(QStringLiteral("state")).toString(QStringLiteral("stopped"));
    const QString newTsVip = ts.value(QStringLiteral("virtualIp")).toString();
    const QString newTsExp = ts.value(QStringLiteral("exposeIp")).toString();
    const QString newTsAuth = ts.value(QStringLiteral("authUrl")).toString();
    const QString newTsErr = ts.value(QStringLiteral("error")).toString();
    const int newTsPeers = ts.value(QStringLiteral("peersOnline")).toInt();
    const bool newTsHostExp = ts.value(QStringLiteral("hostExposed")).toBool();

    const QString newEtState = et.value(QStringLiteral("state")).toString(QStringLiteral("stopped"));
    const QString newEtVip = et.value(QStringLiteral("virtualIp")).toString();
    const QString newEtExp = et.value(QStringLiteral("exposeIp")).toString();
    const QString newEtErr = et.value(QStringLiteral("error")).toString();
    const int newEtPeers = et.value(QStringLiteral("peersOnline")).toInt();
    const bool newEtHostExp = et.value(QStringLiteral("hostExposed")).toBool();

    const bool connChanged = (newTsState != m_TsState) || (newTsExp != m_TsExposeIp) ||
                             (newEtState != m_EtState) || (newEtExp != m_EtExposeIp);
    const bool anyChanged = connChanged || (newTsVip != m_TsVirtualIp) || (newTsAuth != m_TsAuthUrl) ||
                            (newTsErr != m_TsError) || (newTsPeers != m_TsPeersOnline) || (newTsHostExp != m_TsHostExposed) ||
                            (newEtVip != m_EtVirtualIp) || (newEtErr != m_EtError) || (newEtPeers != m_EtPeersOnline) ||
                            (newEtHostExp != m_EtHostExposed);

    m_TsState = newTsState;
    m_TsVirtualIp = newTsVip;
    m_TsExposeIp = newTsExp;
    m_TsAuthUrl = newTsAuth;
    m_TsError = newTsErr;
    m_TsPeersOnline = newTsPeers;
    m_TsHostExposed = newTsHostExp;

    m_EtState = newEtState;
    m_EtVirtualIp = newEtVip;
    m_EtExposeIp = newEtExp;
    m_EtError = newEtErr;
    m_EtPeersOnline = newEtPeers;
    m_EtHostExposed = newEtHostExp;

    if (!m_SecondaryMode && !m_AttachedToExistingDaemon && m_WantHostExpose && m_HostHttpPort > 0 &&
        ((m_TsState == QStringLiteral("connected") && !m_TsHostExposed) ||
         (m_EtState == QStringLiteral("connected") && !m_EtHostExposed))) {
        sendExposeRequest();
    }
    if (anyChanged) {
        emit changed();
    }
    if (connChanged) {
        emit connectedStateChanged();
    }
}

void TunnelManager::syncHostExposure(bool hostReady, quint16 httpPort, quint16 httpsPort)
{
    if (m_SecondaryMode || m_AttachedToExistingDaemon) {
        return;
    }
    m_WantHostExpose = hostReady;
    m_HostHttpPort = httpPort;
    m_HostHttpsPort = httpsPort;

    if (!m_DaemonReady) {
        return;
    }
    const bool anyConnected = (m_TsState == QStringLiteral("connected") || m_EtState == QStringLiteral("connected"));
    if (hostReady && httpPort > 0 && anyConnected) {
        sendExposeRequest();
    } else if (!hostReady && (m_TsHostExposed || m_EtHostExposed)) {
        auto* reply = apiRequest("POST", QStringLiteral("/v1/unexpose"));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
}

void TunnelManager::sendExposeRequest()
{
    if (!m_DaemonReady || m_HostHttpPort == 0) {
        return;
    }
    const QJsonObject ports{
        {QStringLiteral("httpPort"), m_HostHttpPort},
        {QStringLiteral("httpsPort"), m_HostHttpsPort},
    };
    auto* reply = apiRequest("POST", QStringLiteral("/v1/expose"), QJsonDocument(ports).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

void TunnelManager::ensureForwardsAsync(const QList<QPair<QString, QString>>& endpoints,
                                        quint16 httpPort,
                                        quint16 httpsPort,
                                        std::function<void(QList<QPair<QString, QString>> localForwards)> callback)
{
    if (m_ProbingExistingDaemon && !m_DaemonReady) {
        QObject::connect(this, &TunnelManager::daemonAttachFinished, this,
                         [this, endpoints, httpPort, httpsPort, cb = std::move(callback)]() mutable {
                             ensureForwardsAsync(endpoints, httpPort, httpsPort, std::move(cb));
                         },
                         Qt::SingleShotConnection);
        return;
    }
    // Filter to connected providers and sort by user's routePolicy order.
    QList<QPair<QString, QString>> matched;
    for (const auto& preferred : orderedTunnelProviders()) {
        for (const auto& ep : endpoints) {
            if (ep.first == preferred && !ep.second.isEmpty() && isProviderConnected(preferred)) {
                matched.append(ep);
                break;
            }
        }
    }
    if (!m_DaemonReady || matched.isEmpty()) {
        callback({});
        return;
    }

    struct BatchState {
        QList<QPair<QString, QString>> results; // (localHost, provider)
        int remaining = 0;
        std::function<void(QList<QPair<QString, QString>>)> done;
    };
    auto state = std::make_shared<BatchState>();
    state->results = QList<QPair<QString, QString>>(matched.size());
    state->remaining = matched.size();
    state->done = std::move(callback);

    for (int i = 0; i < matched.size(); ++i) {
        const QString provider = matched[i].first;
        const QJsonObject body{
            {QStringLiteral("provider"), provider},
            {QStringLiteral("remoteIp"), matched[i].second},
            {QStringLiteral("ports"), QJsonObject{
                {QStringLiteral("httpPort"), httpPort},
                {QStringLiteral("httpsPort"), httpsPort},
            }},
        };
        auto* reply = apiRequest("POST", QStringLiteral("/v1/forward"), QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, state, i, provider] {
            reply->deleteLater();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError && code < 400) {
                const auto obj = QJsonDocument::fromJson(reply->readAll()).object();
                const QString localHost = obj.value(QStringLiteral("localHost")).toString();
                if (!localHost.isEmpty()) {
                    state->results[i] = {localHost, provider};
                }
            }
            if (--state->remaining == 0) {
                QList<QPair<QString, QString>> compact;
                QStringList seen;
                for (const auto& entry : state->results) {
                    if (!entry.first.isEmpty() && !seen.contains(entry.first)) {
                        seen.append(entry.first);
                        compact.append(entry);
                    }
                }
                state->done(compact);
            }
        });
    }
}
