#include "autoupdatechecker.h"
#include "portableupdateinstaller.h"
#include "path.h"
#include "settings/streamingpreferences.h"
#include "versionutils.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#ifdef SUNANDMON_APP
#define GITHUB_OWNER "Wangbwm"
#define GITHUB_REPO  "SunAndMon"
#define APP_UA_PREFIX "SunAndMon"
#else
#define GITHUB_OWNER "qiin2333"
#define GITHUB_REPO  "moonlight-qt"
#define APP_UA_PREFIX "Moonlight"
#endif

namespace {

// main.cpp globally disables system proxies so streaming/LAN never hit a proxy.
// Use a per-NAM proxy factory here so GitHub update checks honor system/Clash proxy.
class SystemUpdateProxyFactory : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query = QNetworkProxyQuery()) override
    {
        const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (!proxies.isEmpty()) {
            return proxies;
        }
        return {QNetworkProxy(QNetworkProxy::NoProxy)};
    }
};

} // namespace

AutoUpdateChecker::AutoUpdateChecker(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);
    m_Nam->setProxyFactory(new SystemUpdateProxyFactory());
    m_PortableUpdateInstaller = new PortableUpdateInstaller(this);

    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    QString currentVersion(VERSION_STR);
    qDebug() << "Current" << APP_UA_PREFIX << "version:" << currentVersion;
    m_CurrentVersion = VersionUtils::parseRelease(currentVersion);
    Q_ASSERT(m_CurrentVersion.segmentCount() > 1);

    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateStatusChanged,
            this, [this](const QString& message) {
                m_UpdateInProgress = true;
                m_UpdateStatusMessage = message;
                emit updateStateChanged();
                emit onPortableUpdateStatusChanged(message);
            });
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::onPortableUpdateFailed,
            this, [this](const QString& message) {
                m_UpdateInProgress = false;
                m_UpdateStatusMessage = message;
                emit updateStateChanged();
                emit onPortableUpdateFailed(message);
            });
    connect(m_PortableUpdateInstaller, &PortableUpdateInstaller::prepareForRestart,
            this, &AutoUpdateChecker::prepareForRestart);
}

bool AutoUpdateChecker::supportsInAppUpdate() const
{
    return m_PortableUpdateInstaller->supportsInAppUpdate();
}

bool AutoUpdateChecker::supportsUpdateCheck() const
{
#if defined(SUNANDMON_APP) && (defined(Q_OS_WIN32) || defined(Q_OS_LINUX) || defined(Q_OS_DARWIN))
    return true;
#elif defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
    return true;
#else
    return false;
#endif
}

void AutoUpdateChecker::installUpdate(QString url)
{
    const QString expectedDigest = url == m_UpdateDownloadUrl ? m_UpdateAssetDigest : QString();
    m_PortableUpdateInstaller->installUpdate(url, expectedDigest);
}

void AutoUpdateChecker::installAvailableUpdate()
{
    if (m_UpdateDownloadUrl.isEmpty()) {
        return;
    }

    if (supportsInAppUpdate() &&
            m_UpdateDownloadUrl.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        installUpdate(m_UpdateDownloadUrl);
    }
    else {
        QDesktopServices::openUrl(QUrl(m_UpdateDownloadUrl));
    }
}

void AutoUpdateChecker::installLocalUpdate(QString zipFilePath)
{
    QString localPath = zipFilePath.trimmed();
    QUrl asUrl(localPath);
    if (asUrl.isValid() && asUrl.isLocalFile()) {
        localPath = asUrl.toLocalFile();
    }

    if (localPath.isEmpty() || !QFileInfo::exists(localPath)) {
        const QString errMsg = tr("The selected update archive does not exist.");
        m_UpdateInProgress = false;
        m_UpdateStatusMessage = errMsg;
        emit updateStateChanged();
        emit onPortableUpdateFailed(errMsg);
        return;
    }

    m_PortableUpdateInstaller->installLocalArchive(localPath);
}

void AutoUpdateChecker::start()
{
    checkForUpdates();
}

QString AutoUpdateChecker::applyCustomDownloadProxy(const QString& url) const
{
#ifdef SUNANDMON_APP
    StreamingPreferences* prefs = StreamingPreferences::get();
    if (prefs == nullptr) {
        return url;
    }

    const QString proxy = prefs->customUpdateUrl.trimmed();
    if (proxy.isEmpty()) {
        return url;
    }

    if (proxy.contains(QStringLiteral("{url}"))) {
        QString replaced = proxy;
        return replaced.replace(QStringLiteral("{url}"), url);
    }

    if (proxy.endsWith(QLatin1Char('/'))) {
        return proxy + url;
    }
    return proxy + QLatin1Char('/') + url;
#else
    return url;
#endif
}

bool AutoUpdateChecker::checkForUpdates()
{
    if (!supportsUpdateCheck()) {
        return false;
    }

    if (m_UpdateCheckInProgress) {
        return false;
    }

    m_UpdateCheckInProgress = true;
    if (!m_UpdateInProgress && !m_UpdateStatusMessage.isEmpty()) {
        m_UpdateStatusMessage.clear();
        emit updateStateChanged();
    }
    emit onUpdateCheckStarted();

#if defined(SUNANDMON_APP) || defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || defined(STEAM_LINK) || defined(APP_IMAGE)
    bool includePreReleases = false;
#ifdef SUNANDMON_APP
    if (StreamingPreferences* prefs = StreamingPreferences::get()) {
        includePreReleases = prefs->includePreReleaseUpdates;
    }
#endif

    const QString apiUrl = includePreReleases
            ? QStringLiteral("https://api.github.com/repos/%1/%2/releases?per_page=10")
                  .arg(QStringLiteral(GITHUB_OWNER), QStringLiteral(GITHUB_REPO))
            : QStringLiteral("https://api.github.com/repos/%1/%2/releases/latest")
                  .arg(QStringLiteral(GITHUB_OWNER), QStringLiteral(GITHUB_REPO));

    QNetworkRequest request{QUrl(apiUrl)};
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("%1/%2").arg(QStringLiteral(APP_UA_PREFIX), QStringLiteral(VERSION_STR)));
    request.setRawHeader("Accept", "application/vnd.github+json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    QNetworkReply* reply = m_Nam->get(request);
    m_ReleaseReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_ReleaseReply == reply) {
            m_ReleaseReply = nullptr;
        }
        handleUpdateCheckRequestFinished(reply);
    });

    QTimer* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(reply, &QNetworkReply::finished, timeout, &QTimer::stop);
    connect(timeout, &QTimer::timeout, this, [this, reply]() {
        if (!reply->isRunning()) {
            return;
        }

        qWarning() << "Update check timed out";
        reply->abort();
        if (m_UpdateCheckInProgress) {
            m_UpdateCheckInProgress = false;
            emit onUpdateCheckFailed();
        }
    });
    timeout->start(15000);
#endif

    return true;
}

QString AutoUpdateChecker::getPreferredAssetSuffix() const
{
#if defined(SUNANDMON_APP)
#if defined(Q_OS_WIN32)
    return QStringLiteral("-x64.zip");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("_amd64.deb");
#else
    return QString();
#endif
#elif defined(Q_OS_DARWIN)
    return QStringLiteral("-") + QSysInfo::buildCpuArchitecture() + QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    QString architecture = QSysInfo::buildCpuArchitecture().toLower();
    if (architecture == QStringLiteral("arm64")) {
        architecture = QStringLiteral("aarch64");
    }
    return QStringLiteral("-") + architecture + QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getExpectedAssetSuffix() const
{
#if defined(SUNANDMON_APP)
#if defined(Q_OS_WIN32)
    return QStringLiteral(".zip");
#elif defined(Q_OS_LINUX)
    return QStringLiteral(".deb");
#else
    return QString();
#endif
#elif defined(Q_OS_WIN32)
    return isPortableInstall() ? QStringLiteral(".zip") : QStringLiteral(".exe");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral(".dmg");
#elif defined(APP_IMAGE)
    return QStringLiteral(".AppImage");
#else
    return QString();
#endif
}

bool AutoUpdateChecker::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir(Path::getPortableRootDir()).filePath("portable.dat"));
#else
    return false;
#endif
}

QString AutoUpdateChecker::getExpectedAssetPrefix() const
{
#if defined(SUNANDMON_APP)
#if defined(Q_OS_WIN32)
    return QStringLiteral("SunAndMon-Windows-");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("sunandmon_");
#else
    return QString();
#endif
#elif defined(Q_OS_WIN32)
    if (isPortableInstall()) {
        return QStringLiteral("Moonlight-VPlus-Portable-%1-").arg(getCurrentBuildArch());
    }
    return QStringLiteral("Moonlight-VPlus-Setup-");
#else
    return QString();
#endif
}

QString AutoUpdateChecker::getCurrentBuildArch() const
{
    QString buildArch = QSysInfo::buildCpuArchitecture();
    if (buildArch == "x86_64") {
        return QStringLiteral("x64");
    }
    else if (buildArch == "i386") {
        return QStringLiteral("x86");
    }
    return buildArch.toLower();
}

QString AutoUpdateChecker::extractSha256FromChecksums(const QString& text, const QString& assetName)
{
    if (assetName.isEmpty()) {
        return QString();
    }

    static const QRegularExpression linePattern(QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(.+)$"));
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        const QRegularExpressionMatch match = linePattern.match(rawLine.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        const QString digest = match.captured(1).toLower();
        const QString fileName = QFileInfo(match.captured(2).trimmed()).fileName();
        if (fileName.compare(assetName, Qt::CaseInsensitive) == 0) {
            return QStringLiteral("sha256:") + digest;
        }
    }
    return QString();
}

void AutoUpdateChecker::emitDiscoveredUpdate(const QString& tagName,
                                             const QString& downloadUrl,
                                             const QString& assetDigest)
{
    m_UpdateAvailable = true;
    m_LatestVersion = tagName;
    m_UpdateDownloadUrl = downloadUrl;
    m_UpdateAssetDigest = assetDigest;
    m_UpdateCheckInProgress = false;

    emit updateStateChanged();
    emit onUpdateAvailable(tagName, downloadUrl);
    emit onUpdateCheckFinished(true);
}

void AutoUpdateChecker::handleChecksumsReplyFinished()
{
    QNetworkReply* reply = m_ChecksumsReply;
    m_ChecksumsReply = nullptr;
    if (reply == nullptr) {
        return;
    }

    QString digest;
    if (reply->error() == QNetworkReply::NoError) {
        digest = extractSha256FromChecksums(QString::fromUtf8(reply->readAll()), m_PendingAssetName);
    }
    else {
        qWarning() << "Failed to fetch SHA256SUMS.txt:" << reply->errorString();
    }
    reply->deleteLater();

    const QString tagName = m_PendingTagName;
    const QString downloadUrl = m_PendingDownloadUrl;
    m_PendingTagName.clear();
    m_PendingDownloadUrl.clear();
    m_PendingAssetName.clear();

    emitDiscoveredUpdate(tagName, downloadUrl, digest);
}

void AutoUpdateChecker::handleUpdateCheckRequestFinished(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    auto finish = [this](bool updateAvailable) {
        m_UpdateCheckInProgress = false;
        if (!updateAvailable) {
            m_UpdateAvailable = false;
            emit updateStateChanged();
        }
        emit onUpdateCheckFinished(updateAvailable);
    };
    auto fail = [this]() {
        m_UpdateCheckInProgress = false;
        emit onUpdateCheckFailed();
    };

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Update checking failed:" << reply->error() << reply->errorString();
        reply->deleteLater();
        fail();
        return;
    }

    QTextStream stream(reply);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif
    QString jsonString = stream.readAll();
    reply->deleteLater();

    QJsonParseError error;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonString.toUtf8(), &error);
    if (jsonDoc.isNull()) {
        qWarning() << "GitHub release response malformed:" << error.errorString();
        fail();
        return;
    }

    bool includePreReleases = false;
#ifdef SUNANDMON_APP
    if (StreamingPreferences* prefs = StreamingPreferences::get()) {
        includePreReleases = prefs->includePreReleaseUpdates;
    }
#endif

    QJsonObject releaseObj;
    if (jsonDoc.isObject()) {
        releaseObj = jsonDoc.object();
    }
    else if (jsonDoc.isArray()) {
        const QJsonArray releases = jsonDoc.array();
        for (const auto& entry : releases) {
            if (!entry.isObject()) {
                continue;
            }
            const QJsonObject candidate = entry.toObject();
            if (candidate["draft"].toBool(false)) {
                continue;
            }
            if (!includePreReleases && candidate["prerelease"].toBool(false)) {
                continue;
            }
            releaseObj = candidate;
            break;
        }
        if (releaseObj.isEmpty()) {
            finish(false);
            return;
        }
    }
    else {
        fail();
        return;
    }

    if (releaseObj["draft"].toBool(false) ||
            (!includePreReleases && releaseObj["prerelease"].toBool(false))) {
        finish(false);
        return;
    }

    if (!releaseObj.contains("tag_name") || !releaseObj["tag_name"].isString()) {
        fail();
        return;
    }

    QString tagName = releaseObj["tag_name"].toString();
    qDebug() << "Latest GitHub release tag:" << tagName;

    const QVersionNumber latestVersion = VersionUtils::parseRelease(tagName);
    if (m_CurrentVersion.segmentCount() < 2 || latestVersion.segmentCount() < 2) {
        qWarning() << "Invalid release version:" << QStringLiteral(VERSION_STR) << tagName;
        fail();
        return;
    }

    int res = VersionUtils::compare(m_CurrentVersion, latestVersion);
    if (res >= 0) {
        finish(false);
        return;
    }

    qDebug() << "Update available:" << tagName;

    QString downloadUrl;
    QString matchedAssetName;
    QString assetDigest;
    QString checksumsUrl;

    const QString expectedPrefix = getExpectedAssetPrefix();
    const QString expectedSuffix = getExpectedAssetSuffix();
    const QString preferredSuffix = getPreferredAssetSuffix();

    if (!expectedSuffix.isEmpty() && releaseObj.contains("assets") && releaseObj["assets"].isArray()) {
        QJsonArray assets = releaseObj["assets"].toArray();
        QString fallbackUrl;
        QString fallbackName;
        QString fallbackDigest;

#if defined(SUNANDMON_APP) && defined(Q_OS_WIN32)
        QString setupFallbackUrl;
        QString setupFallbackName;
        QString setupFallbackDigest;
#endif

        for (const auto& asset : std::as_const(assets)) {
            if (!asset.isObject()) {
                continue;
            }
            QJsonObject assetObj = asset.toObject();
            QString assetName = assetObj["name"].toString();

            if (assetName.compare(QStringLiteral("SHA256SUMS.txt"), Qt::CaseInsensitive) == 0) {
                checksumsUrl = assetObj["browser_download_url"].toString();
                continue;
            }

#if defined(SUNANDMON_APP) && defined(Q_OS_WIN32)
            if (assetName.startsWith(QStringLiteral("SunAndMon-Setup-"), Qt::CaseInsensitive) &&
                    assetName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) &&
                    setupFallbackUrl.isEmpty()) {
                setupFallbackUrl = assetObj["browser_download_url"].toString();
                setupFallbackName = assetName;
                setupFallbackDigest = assetObj["digest"].toString();
            }
#endif

            bool prefixMatches = expectedPrefix.isEmpty() ||
                                 assetName.startsWith(expectedPrefix, Qt::CaseInsensitive);
            bool suffixMatches = assetName.endsWith(expectedSuffix, Qt::CaseInsensitive);
            if (!prefixMatches || !suffixMatches) {
                continue;
            }

            if (!preferredSuffix.isEmpty() &&
                    assetName.endsWith(preferredSuffix, Qt::CaseInsensitive)) {
                downloadUrl = assetObj["browser_download_url"].toString();
                matchedAssetName = assetName;
                assetDigest = assetObj["digest"].toString();
                break;
            }

            bool isOtherArchAsset =
                    assetName.endsWith(QStringLiteral("-arm64.dmg"), Qt::CaseInsensitive) ||
                    assetName.endsWith(QStringLiteral("-x86_64.dmg"), Qt::CaseInsensitive) ||
                    (!preferredSuffix.isEmpty() &&
                     (assetName.endsWith(QStringLiteral("-aarch64.AppImage"), Qt::CaseInsensitive) ||
                      assetName.endsWith(QStringLiteral("-x86_64.AppImage"), Qt::CaseInsensitive)));

            if (fallbackUrl.isEmpty() && !isOtherArchAsset) {
                fallbackUrl = assetObj["browser_download_url"].toString();
                fallbackName = assetName;
                fallbackDigest = assetObj["digest"].toString();
            }
        }

        if (downloadUrl.isEmpty() && !fallbackUrl.isEmpty()) {
            downloadUrl = fallbackUrl;
            matchedAssetName = fallbackName;
            assetDigest = fallbackDigest;
        }
#if defined(SUNANDMON_APP) && defined(Q_OS_WIN32)
        if (downloadUrl.isEmpty() && !setupFallbackUrl.isEmpty()) {
            downloadUrl = setupFallbackUrl;
            matchedAssetName = setupFallbackName;
            assetDigest = setupFallbackDigest;
        }
#endif
    }

    if (downloadUrl.isEmpty()) {
        downloadUrl = releaseObj["html_url"].toString();
    }
    else {
        downloadUrl = applyCustomDownloadProxy(downloadUrl);
    }

    if (assetDigest.isEmpty() && !checksumsUrl.isEmpty() && !matchedAssetName.isEmpty()) {
        m_PendingTagName = tagName;
        m_PendingDownloadUrl = downloadUrl;
        m_PendingAssetName = matchedAssetName;

        QNetworkRequest sumReq{QUrl(applyCustomDownloadProxy(checksumsUrl))};
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        sumReq.setTransferTimeout(10000);
#endif
        sumReq.setHeader(QNetworkRequest::UserAgentHeader,
                         QStringLiteral("%1/%2").arg(QStringLiteral(APP_UA_PREFIX), QStringLiteral(VERSION_STR)));
        m_ChecksumsReply = m_Nam->get(sumReq);
        connect(m_ChecksumsReply, &QNetworkReply::finished,
                this, &AutoUpdateChecker::handleChecksumsReplyFinished);
        return;
    }

    emitDiscoveredUpdate(tagName, downloadUrl, assetDigest);
}
