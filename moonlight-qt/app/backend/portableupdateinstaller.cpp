#include "portableupdateinstaller.h"
#include "path.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <vector>
#endif

namespace {

class SystemDownloadProxyFactory : public QNetworkProxyFactory
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

#ifdef Q_OS_WIN32
QString quoteWindowsArg(const QString& arg)
{
    if (!arg.isEmpty() && !arg.contains(QLatin1Char(' ')) &&
            !arg.contains(QLatin1Char('\t')) && !arg.contains(QLatin1Char('"'))) {
        return arg;
    }

    QString out;
    out.append(QLatin1Char('"'));
    int backslashes = 0;
    for (QChar ch : arg) {
        if (ch == QLatin1Char('\\')) {
            backslashes++;
        }
        else if (ch == QLatin1Char('"')) {
            out.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            out.append(QLatin1Char('"'));
            backslashes = 0;
        }
        else {
            if (backslashes > 0) {
                out.append(QString(backslashes, QLatin1Char('\\')));
                backslashes = 0;
            }
            out.append(ch);
        }
    }
    if (backslashes > 0) {
        out.append(QString(backslashes * 2, QLatin1Char('\\')));
    }
    out.append(QLatin1Char('"'));
    return out;
}

QString joinWindowsArgs(const QStringList& args)
{
    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString& arg : args) {
        quoted.append(quoteWindowsArg(arg));
    }
    return quoted.join(QLatin1Char(' '));
}

QString extractFirstCommandToken(QString cmd)
{
    cmd = cmd.trimmed();
    if (cmd.startsWith(QLatin1Char('"'))) {
        const int endQuote = cmd.indexOf(QLatin1Char('"'), 1);
        if (endQuote > 1) {
            return cmd.mid(1, endQuote - 1);
        }
    }
    const int space = cmd.indexOf(QLatin1Char(' '));
    return space > 0 ? cmd.left(space) : cmd;
}
#endif

} // namespace

PortableUpdateInstaller::PortableUpdateInstaller(QObject *parent) :
    QObject(parent),
    m_UpdateNam(nullptr),
    m_UpdateReply(nullptr),
    m_UpdateFile(nullptr),
    m_MetadataChecked(false)
{
}

bool PortableUpdateInstaller::supportsInAppUpdate() const
{
#if defined(Q_OS_WIN32)
#ifdef SUNANDMON_APP
    return !getPortableUpdaterExecutable().isEmpty();
#else
    return isPortableInstall() && !getPortableUpdaterExecutable().isEmpty();
#endif
#elif defined(Q_OS_DARWIN)
    QString errorMessage;
    return isBundleInstall() &&
            !getPortableUpdaterExecutable().isEmpty() &&
            ensureWritableInstallDir(errorMessage);
#else
    return false;
#endif
}

void PortableUpdateInstaller::installUpdate(const QString& url, const QString& expectedDigest)
{
    if (!supportsInAppUpdate()) {
        emit onPortableUpdateFailed(tr("In-app update is not supported for this installation."));
        return;
    }

    if (m_UpdateReply != nullptr) {
        emit onPortableUpdateStatusChanged(tr("An update is already in progress."));
        return;
    }

    QUrl downloadUrl = QUrl::fromUserInput(url.trimmed());
    if (!downloadUrl.isValid() || downloadUrl.scheme().isEmpty()) {
        emit onPortableUpdateFailed(tr("The update URL is invalid."));
        return;
    }

    if (!downloadUrl.path().endsWith(getUpdateArchiveSuffix(), Qt::CaseInsensitive)) {
        emit onPortableUpdateFailed(tr("The update package was not found for this release."));
        return;
    }

    QString normalizedDigest = expectedDigest.trimmed();
    if (normalizedDigest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
        normalizedDigest.remove(0, 7);
    }

    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
#if defined(Q_OS_DARWIN)
    if (!sha256Pattern.match(normalizedDigest).hasMatch()) {
        emit onPortableUpdateFailed(tr("The update package has no valid integrity information. Please install the update manually."));
        return;
    }
#else
    if (!normalizedDigest.isEmpty() && !sha256Pattern.match(normalizedDigest).hasMatch()) {
        emit onPortableUpdateFailed(tr("The update package has invalid integrity information."));
        return;
    }
#endif

#if !defined(SUNANDMON_APP) || !defined(Q_OS_WIN32)
    QString errorMessage;
    if (!ensureWritableInstallDir(errorMessage)) {
        emit onPortableUpdateFailed(errorMessage);
        return;
    }
#endif

    QString workspace = createPortableUpdateWorkspace();
    if (workspace.isEmpty()) {
        emit onPortableUpdateFailed(tr("Unable to create a temporary folder for the update."));
        return;
    }

    resetPortableUpdateState(true);
    m_PortableUpdateWorkspace = workspace;
    m_PortableUpdateError.clear();
    m_ExpectedSha256 = normalizedDigest.toLatin1().toLower();
    m_MetadataChecked = false;

    if (m_UpdateNam == nullptr) {
        m_UpdateNam = new QNetworkAccessManager(this);
        m_UpdateNam->setProxyFactory(new SystemDownloadProxyFactory());
        m_UpdateNam->setStrictTransportSecurityEnabled(true);
        m_UpdateNam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    }

    QString archivePath = QDir(workspace).filePath(getUpdateArchiveName());
    m_UpdateFile = new QFile(archivePath, this);
    if (!m_UpdateFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_PortableUpdateError = tr("Unable to create the update package on disk.");
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(m_PortableUpdateError);
        return;
    }

    QNetworkRequest request(downloadUrl);
#ifdef SUNANDMON_APP
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SunAndMon/%1").arg(QStringLiteral(VERSION_STR)));
#else
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Moonlight/%1").arg(QStringLiteral(VERSION_STR)));
#endif
    request.setRawHeader("Accept", "application/octet-stream");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    m_UpdateReply = m_UpdateNam->get(request);
    connect(m_UpdateReply, &QNetworkReply::metaDataChanged,
            this, &PortableUpdateInstaller::handlePortableUpdateMetaDataChanged);
    connect(m_UpdateReply, &QNetworkReply::readyRead,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadReadyRead);
    connect(m_UpdateReply, &QNetworkReply::downloadProgress,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadProgress);
    connect(m_UpdateReply, &QNetworkReply::finished,
            this, &PortableUpdateInstaller::handlePortableUpdateDownloadFinished);

    emit onPortableUpdateStatusChanged(tr("Downloading update..."));
}

void PortableUpdateInstaller::installLocalArchive(const QString& localZipPath, const QString& expectedDigest)
{
    if (!supportsInAppUpdate()) {
        emit onPortableUpdateFailed(tr("In-app update is not supported for this installation."));
        return;
    }

    if (m_UpdateReply != nullptr) {
        emit onPortableUpdateStatusChanged(tr("An update is already in progress."));
        return;
    }

    QFileInfo sourceInfo(localZipPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile() ||
            !sourceInfo.fileName().endsWith(getUpdateArchiveSuffix(), Qt::CaseInsensitive)) {
        emit onPortableUpdateFailed(tr("Please select a valid .zip update archive."));
        return;
    }

    QString normalizedDigest = expectedDigest.trimmed();
    if (normalizedDigest.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive)) {
        normalizedDigest.remove(0, 7);
    }
    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    if (!normalizedDigest.isEmpty() && !sha256Pattern.match(normalizedDigest).hasMatch()) {
        emit onPortableUpdateFailed(tr("The update package has invalid integrity information."));
        return;
    }

    QString spaceError;
    if (!ensureSufficientDiskSpace(estimateRequiredWorkspaceBytes(sourceInfo.size()), spaceError)) {
        emit onPortableUpdateFailed(spaceError);
        return;
    }

    QString workspace = createPortableUpdateWorkspace();
    if (workspace.isEmpty()) {
        emit onPortableUpdateFailed(tr("Unable to create a temporary folder for the update."));
        return;
    }

    resetPortableUpdateState(true);
    m_PortableUpdateWorkspace = workspace;
    m_PortableUpdateError.clear();
    m_ExpectedSha256 = normalizedDigest.toLatin1().toLower();

    emit onPortableUpdateStatusChanged(tr("Preparing local update archive..."));

    QString stagedArchive = QDir(workspace).filePath(getUpdateArchiveName());
    if (!QFile::copy(sourceInfo.absoluteFilePath(), stagedArchive)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(tr("Unable to copy the selected update archive to workspace."));
        return;
    }

    emit onPortableUpdateStatusChanged(tr("Verifying update..."));
    QString verificationError;
    if (!verifyUpdateArchive(stagedArchive, verificationError)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(verificationError);
        return;
    }

    QString launchError;
    if (!launchUpdaterAndQuit(stagedArchive, launchError)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(launchError);
        return;
    }
}

bool PortableUpdateInstaller::isPortableInstall() const
{
#if defined(Q_OS_WIN32)
    return QFile::exists(QDir(Path::getPortableRootDir()).filePath("portable.dat"));
#else
    return false;
#endif
}

bool PortableUpdateInstaller::isBundleInstall() const
{
    return !getInstalledBundlePath().isEmpty();
}

QString PortableUpdateInstaller::getInstalledBundlePath() const
{
#if defined(Q_OS_DARWIN)
    QDir dir(QCoreApplication::applicationDirPath());
    if (!dir.cdUp() || !dir.cdUp()) {
        return QString();
    }
    QString path = dir.absolutePath();
    return path.endsWith(QLatin1String(".app")) ? path : QString();
#else
    return QString();
#endif
}

QString PortableUpdateInstaller::getUpdateArchiveSuffix() const
{
#if defined(Q_OS_DARWIN)
    return QStringLiteral(".dmg");
#else
    return QStringLiteral(".zip");
#endif
}

QString PortableUpdateInstaller::getUpdateArchiveName() const
{
#if defined(Q_OS_DARWIN)
    return QStringLiteral("MoonlightUpdate.dmg");
#elif defined(SUNANDMON_APP)
    return QStringLiteral("SunAndMonUpdate.zip");
#else
    return QStringLiteral("MoonlightPortableUpdate.zip");
#endif
}

QString PortableUpdateInstaller::getInstallRootDir() const
{
#if defined(Q_OS_WIN32)
    const QString appDir = QCoreApplication::applicationDirPath();
    return appDir.isEmpty() ? Path::getPortableRootDir() : appDir;
#else
    return Path::getPortableRootDir();
#endif
}

QString PortableUpdateInstaller::getUpdateStorageProbePath() const
{
#if defined(Q_OS_DARWIN)
    QString bundlePath = getInstalledBundlePath();
    if (!bundlePath.isEmpty()) {
        return QFileInfo(bundlePath).absolutePath();
    }
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
#elif defined(SUNANDMON_APP) && defined(Q_OS_WIN32)
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appData.isEmpty()) {
        QDir().mkpath(appData);
        return appData;
    }
    return QDir::tempPath();
#else
    return Path::getPortableRootDir();
#endif
}

QString PortableUpdateInstaller::getPortableUpdaterExecutable() const
{
#if defined(Q_OS_WIN32)
    QString executable = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!executable.isEmpty()) {
        return executable;
    }
    return QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
#elif defined(Q_OS_DARWIN)
    return QFile::exists(QStringLiteral("/bin/sh")) ? QStringLiteral("/bin/sh") : QString();
#else
    return QString();
#endif
}

bool PortableUpdateInstaller::isInstallDirWritable() const
{
#if defined(Q_OS_WIN32)
    QTemporaryFile probeFile(QDir(getInstallRootDir()).filePath("SunAndMonUpdateWriteProbe-XXXXXX.tmp"));
    probeFile.setAutoRemove(true);
    if (!probeFile.open()) {
        return false;
    }
    probeFile.close();
    return true;
#else
    return false;
#endif
}

bool PortableUpdateInstaller::isOwnedHostServiceRunning() const
{
#if defined(Q_OS_WIN32) && defined(SUNANDMON_APP)
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        return false;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"SunAndMonHostService", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    bool ownedAndRunning = false;
    if (svc != nullptr) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status) &&
                (status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_START_PENDING)) {
            DWORD needed = 0;
            QueryServiceConfigW(svc, nullptr, 0, &needed);
            if (needed > 0) {
                std::vector<BYTE> buf(needed);
                auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
                if (QueryServiceConfigW(svc, cfg, needed, &needed) && cfg->lpBinaryPathName != nullptr) {
                    const QString svcBin = QDir::cleanPath(
                        QDir::fromNativeSeparators(extractFirstCommandToken(QString::fromWCharArray(cfg->lpBinaryPathName))));
                    QString rootPrefix = QDir::cleanPath(QDir::fromNativeSeparators(getInstallRootDir()));
                    if (!rootPrefix.endsWith(QLatin1Char('/'))) {
                        rootPrefix += QLatin1Char('/');
                    }
                    ownedAndRunning = svcBin.startsWith(rootPrefix, Qt::CaseInsensitive);
                }
            }
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ownedAndRunning;
#else
    return false;
#endif
}

bool PortableUpdateInstaller::ensureWritableInstallDir(QString& errorMessage) const
{
#if defined(Q_OS_WIN32)
    if (!isInstallDirWritable()) {
        errorMessage = tr("The current installation folder is not writable. Run with sufficient permissions or install the update manually.");
        return false;
    }
    return true;
#elif defined(Q_OS_DARWIN)
    QString bundlePath = getInstalledBundlePath();
    if (bundlePath.isEmpty()) {
        errorMessage = tr("In-app update requires running Moonlight from an app bundle.");
        return false;
    }
    QFileInfo bundleInfo(bundlePath);
    QTemporaryFile probeFile(QDir(bundleInfo.absolutePath())
                             .filePath(QStringLiteral(".MoonlightUpdateWriteProbe-XXXXXX")));
    probeFile.setAutoRemove(true);
    if (!probeFile.open()) {
        errorMessage = tr("Moonlight is installed in a folder you can't write to. Move it to your "
                          "Applications folder, or download and install the update manually.");
        return false;
    }
    probeFile.close();
    return true;
#else
    Q_UNUSED(errorMessage);
    return false;
#endif
}

QString PortableUpdateInstaller::createPortableUpdateWorkspace() const
{
#if defined(Q_OS_DARWIN)
    static const QLatin1String kWorkspacePrefix(".MoonlightUpdate-");
    QString baseDir = getUpdateStorageProbePath();
#elif defined(SUNANDMON_APP) && defined(Q_OS_WIN32)
    static const QLatin1String kWorkspacePrefix("hotupdate-");
    QString baseDir = QDir(getUpdateStorageProbePath()).filePath(QStringLiteral("updates"));
#else
    static const QLatin1String kWorkspacePrefix("MoonlightPortableUpdate-");
    QString baseDir = getUpdateStorageProbePath();
#endif

    QString workspace = QDir(baseDir).filePath(
                kWorkspacePrefix + QString("%1-%2")
                .arg(QCoreApplication::applicationPid())
                .arg(QDateTime::currentMSecsSinceEpoch()));

    if (!QDir().mkpath(workspace)) {
        return QString();
    }
    return workspace;
}

QString PortableUpdateInstaller::materializePortableUpdateScript(const QString& workspace) const
{
#if defined(Q_OS_DARWIN)
    static const QLatin1String kScriptName("install-dmg-update.sh");
#else
    static const QLatin1String kScriptName("install-portable-update.ps1");
#endif

    QFile resourceFile(QStringLiteral(":/data/") + kScriptName);
    if (!resourceFile.open(QIODevice::ReadOnly)) {
        return QString();
    }

    QString scriptPath = QDir(workspace).filePath(kScriptName);
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }

    QByteArray scriptContents = resourceFile.readAll();
    if (scriptFile.write(scriptContents) != scriptContents.size()) {
        return QString();
    }

    scriptFile.close();
    return scriptPath;
}

bool PortableUpdateInstaller::ensureSufficientDiskSpace(qint64 requiredBytes, QString& errorMessage) const
{
    if (requiredBytes <= 0) {
        return true;
    }

    QStorageInfo storage(getUpdateStorageProbePath());
    storage.refresh();
    if (!storage.isValid() || !storage.isReady()) {
        errorMessage = tr("Unable to determine free disk space for the update.");
        return false;
    }
    if (storage.bytesAvailable() < requiredBytes) {
        errorMessage = tr("Not enough free disk space for the update. Need about %1 MB free.")
                .arg(QString::number(requiredBytes / (1024.0 * 1024.0), 'f', 0));
        return false;
    }
    return true;
}

qint64 PortableUpdateInstaller::estimateRequiredWorkspaceBytes(qint64 archiveBytes) const
{
    if (archiveBytes <= 0) {
        return 0;
    }
    static const qint64 kSafetyMarginBytes = 64LL * 1024 * 1024;
    return archiveBytes * 3 + kSafetyMarginBytes;
}

bool PortableUpdateInstaller::verifyUpdateArchive(const QString& archivePath,
                                                   QString& errorMessage) const
{
    if (m_ExpectedSha256.isEmpty()) {
        return true;
    }

    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly)) {
        errorMessage = tr("Unable to verify the downloaded update package.");
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!archive.atEnd()) {
        QByteArray chunk = archive.read(1024 * 1024);
        if (chunk.isEmpty() && archive.error() != QFile::NoError) {
            errorMessage = tr("Unable to verify the downloaded update package.");
            return false;
        }
        hash.addData(chunk);
    }

    if (hash.result().toHex() != m_ExpectedSha256) {
        errorMessage = tr("The downloaded update package failed its integrity check.");
        return false;
    }
    return true;
}

bool PortableUpdateInstaller::runTool(const QString& program,
                                     const QStringList& arguments,
                                     int timeoutMs) const
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(10000)) {
        qWarning() << "Failed to start" << program << process.errorString();
        return false;
    }

    if (process.state() != QProcess::NotRunning) {
        QEventLoop loop;
        QTimer timeoutTimer;
        bool timedOut = false;

        QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&loop, &timedOut]() {
            timedOut = true;
            loop.quit();
        });

        timeoutTimer.setSingleShot(true);
        timeoutTimer.start(timeoutMs);
        loop.exec(QEventLoop::ExcludeUserInputEvents);

        if (timedOut) {
            process.kill();
            process.waitForFinished(5000);
            return false;
        }
    }

    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool PortableUpdateInstaller::stageMacUpdateBundle(const QString& archivePath,
                                                   QString& stagedBundlePath,
                                                   QString& errorMessage)
{
#if defined(Q_OS_DARWIN)
    QString mountPoint = QDir(m_PortableUpdateWorkspace).filePath(QStringLiteral("mnt"));
    if (!QDir().mkpath(mountPoint)) {
        errorMessage = tr("Unable to create a temporary folder for the update.");
        return false;
    }

    if (!runTool(QStringLiteral("/usr/bin/hdiutil"),
                 { QStringLiteral("attach"), archivePath,
                   QStringLiteral("-nobrowse"), QStringLiteral("-noautoopen"),
                   QStringLiteral("-readonly"),
                   QStringLiteral("-mountpoint"), mountPoint },
                 120000)) {
        errorMessage = tr("Unable to open the downloaded disk image.");
        return false;
    }

    bool staged = false;
    QString bundleInImage = QDir(mountPoint).filePath(QStringLiteral("Moonlight.app"));
    QString stagedBundle = QDir(m_PortableUpdateWorkspace).filePath(QStringLiteral("Moonlight.app"));

    if (!QFileInfo(bundleInImage).isDir()) {
        errorMessage = tr("The downloaded disk image does not contain Moonlight.");
    }
    else if (!runTool(QStringLiteral("/usr/bin/ditto"), { bundleInImage, stagedBundle }, 300000)) {
        errorMessage = tr("Unable to extract the update.");
    }
    else {
        staged = true;
    }

    runTool(QStringLiteral("/usr/bin/hdiutil"),
            { QStringLiteral("detach"), mountPoint, QStringLiteral("-quiet") }, 60000);

    if (!staged) {
        return false;
    }

    runTool(QStringLiteral("/usr/bin/xattr"),
            { QStringLiteral("-dr"), QStringLiteral("com.apple.quarantine"), stagedBundle }, 60000);

    stagedBundlePath = stagedBundle;
    return true;
#else
    Q_UNUSED(archivePath);
    Q_UNUSED(stagedBundlePath);
    Q_UNUSED(errorMessage);
    return false;
#endif
}

bool PortableUpdateInstaller::launchUpdaterAndQuit(const QString& archivePath, QString& errorMessage)
{
#if defined(Q_OS_DARWIN)
    QString stagedBundle;
    if (!stageMacUpdateBundle(archivePath, stagedBundle, errorMessage)) {
        return false;
    }
#endif

    QString scriptPath = materializePortableUpdateScript(m_PortableUpdateWorkspace);
    if (scriptPath.isEmpty()) {
        errorMessage = tr("Unable to prepare the update installer.");
        return false;
    }

    QString updaterExecutable = getPortableUpdaterExecutable();
    if (updaterExecutable.isEmpty()) {
        errorMessage = tr("Unable to launch the updater.");
        return false;
    }

    QStringList arguments;
    QString workingDirectory;

#if defined(Q_OS_DARWIN)
    arguments << scriptPath
              << m_PortableUpdateWorkspace
              << getInstalledBundlePath()
              << stagedBundle
              << QString::number(QCoreApplication::applicationPid());
    workingDirectory = QDir::homePath();

    if (!QProcess::startDetached(updaterExecutable, arguments, workingDirectory)) {
        errorMessage = tr("Unable to launch the updater.");
        return false;
    }
#elif defined(Q_OS_WIN32)
    const QString installDir = QDir::toNativeSeparators(getInstallRootDir());
    const QString logDir = QDir::toNativeSeparators(getUpdateStorageProbePath());
    const bool serviceRunning = isOwnedHostServiceRunning();
    const bool needElevation = serviceRunning || !isInstallDirWritable();

    arguments << "-NoProfile"
              << "-ExecutionPolicy" << "Bypass"
              << "-WindowStyle" << "Hidden"
              << "-File" << QDir::toNativeSeparators(scriptPath)
              << "-WorkspaceDir" << QDir::toNativeSeparators(m_PortableUpdateWorkspace)
              << "-InstallDir" << installDir
              << "-ZipPath" << QDir::toNativeSeparators(archivePath)
              << "-ExePath" << QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
              << "-ParentPid" << QString::number(QCoreApplication::applicationPid())
              << "-LogDir" << logDir;

    if (serviceRunning) {
        arguments << "-RestartService";
    }
    if (needElevation) {
        arguments << "-RelaunchNonElevated";
    }

    workingDirectory = QDir::toNativeSeparators(m_PortableUpdateWorkspace);

    if (needElevation) {
        const QString params = joinWindowsArgs(arguments);
        const std::wstring exeW = QDir::toNativeSeparators(updaterExecutable).toStdWString();
        const std::wstring paramsW = params.toStdWString();
        const std::wstring dirW = workingDirectory.toStdWString();
        HINSTANCE rc = ShellExecuteW(nullptr, L"runas", exeW.c_str(), paramsW.c_str(), dirW.c_str(), SW_HIDE);
        if (reinterpret_cast<intptr_t>(rc) <= 32) {
            errorMessage = tr("Administrator permission is required to complete this hot update.");
            return false;
        }
    }
    else {
        if (!QProcess::startDetached(updaterExecutable, arguments, workingDirectory)) {
            errorMessage = tr("Unable to launch the updater.");
            return false;
        }
    }
#else
    errorMessage = tr("In-app update is not supported on this platform.");
    return false;
#endif

#ifdef SUNANDMON_APP
    emit onPortableUpdateStatusChanged(tr("Applying hot update and restarting SunAndMon..."));
#else
    emit onPortableUpdateStatusChanged(tr("Installing update and restarting Moonlight..."));
#endif

    if (m_UpdateReply != nullptr) {
        m_UpdateReply->deleteLater();
        m_UpdateReply = nullptr;
    }
    if (m_UpdateFile != nullptr) {
        if (m_UpdateFile->isOpen()) {
            m_UpdateFile->close();
        }
        m_UpdateFile->deleteLater();
        m_UpdateFile = nullptr;
    }

    emit prepareForRestart();
    QTimer::singleShot(250, qApp, &QCoreApplication::quit);
    return true;
}

void PortableUpdateInstaller::resetPortableUpdateState(bool removeWorkspace)
{
    if (m_UpdateReply != nullptr) {
        m_UpdateReply->deleteLater();
        m_UpdateReply = nullptr;
    }

    if (m_UpdateFile != nullptr) {
        if (m_UpdateFile->isOpen()) {
            m_UpdateFile->close();
        }
        m_UpdateFile->deleteLater();
        m_UpdateFile = nullptr;
    }

    if (removeWorkspace && !m_PortableUpdateWorkspace.isEmpty()) {
        QDir(m_PortableUpdateWorkspace).removeRecursively();
    }

    if (removeWorkspace) {
        m_PortableUpdateWorkspace.clear();
        m_ExpectedSha256.clear();
    }

    m_MetadataChecked = false;
}

void PortableUpdateInstaller::handlePortableUpdateMetaDataChanged()
{
    if (m_UpdateReply == nullptr || m_MetadataChecked) {
        return;
    }
    m_MetadataChecked = true;

    bool ok = false;
    qint64 archiveBytes = m_UpdateReply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
    if (!ok || archiveBytes <= 0) {
        return;
    }

    QString errorMessage;
    if (!ensureSufficientDiskSpace(estimateRequiredWorkspaceBytes(archiveBytes), errorMessage)) {
        m_PortableUpdateError = errorMessage;
        m_UpdateReply->abort();
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadReadyRead()
{
    if (m_UpdateReply == nullptr || m_UpdateFile == nullptr) {
        return;
    }

    QByteArray data = m_UpdateReply->readAll();
    if (data.isEmpty()) {
        return;
    }

    if (m_UpdateFile->write(data) != data.size()) {
        m_PortableUpdateError = tr("Failed while writing the update package to disk.");
        m_UpdateReply->abort();
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    if (bytesTotal > 0) {
        int progress = static_cast<int>((bytesReceived * 100) / bytesTotal);
        emit onPortableUpdateStatusChanged(tr("Downloading update... %1%").arg(progress));
    }
    else {
        emit onPortableUpdateStatusChanged(
                    tr("Downloading update... %1 MB")
                    .arg(QString::number(bytesReceived / (1024.0 * 1024.0), 'f', 1)));
    }
}

void PortableUpdateInstaller::handlePortableUpdateDownloadFinished()
{
    handlePortableUpdateDownloadReadyRead();

    QString failureMessage = m_PortableUpdateError;
    if (m_UpdateReply != nullptr && m_UpdateReply->error() != QNetworkReply::NoError) {
        if (failureMessage.isEmpty()) {
            failureMessage = tr("Failed to download the update: %1").arg(m_UpdateReply->errorString());
        }
    }

    if (m_UpdateFile != nullptr) {
        m_UpdateFile->flush();
        m_UpdateFile->close();
    }

    if (!failureMessage.isEmpty()) {
        m_PortableUpdateError.clear();
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(failureMessage);
        return;
    }

    QString archivePath = QDir(m_PortableUpdateWorkspace).filePath(getUpdateArchiveName());

    emit onPortableUpdateStatusChanged(tr("Verifying update..."));
    QString verificationError;
    if (!verifyUpdateArchive(archivePath, verificationError)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(verificationError);
        return;
    }

    QString launchError;
    if (!launchUpdaterAndQuit(archivePath, launchError)) {
        resetPortableUpdateState(true);
        emit onPortableUpdateFailed(launchError);
        return;
    }
}
