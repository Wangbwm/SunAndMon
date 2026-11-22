#include "centerclient.h"
#include "computermanager.h"
#include "identitymanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <openssl/rand.h>
#include <cstdio>
#include <utility>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dpapi.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#endif

namespace {
constexpr int MaxCenterResponse = 2 * 1024 * 1024;
constexpr int HostRestartExitCode = 85; // Foundation Sunshine managed restart
constexpr int HostPortsUnavailableExitCode = 86;
const QByteArray HostPortPrefix = "SUNANDMON_PORTS ";
const QByteArray HostPortConflictPrefix = "SUNANDMON_PORT_CONFLICT ";
const QString DevicesPath = QStringLiteral("/v1/devices");
const QString TunnelConfigPath = QStringLiteral("/v1/tunnel-config");
const QString AdminNodesPath = QStringLiteral("/admin/api/nodes");
const QRegularExpression CenterKeyFormat(QStringLiteral("^[a-f0-9]{64}$"));
const QRegularExpression NodeIdFormat(QStringLiteral("^[a-f0-9]{32}$"));
const auto StartHostOnLaunchKey = QStringLiteral("center/startHostOnLaunch");
constexpr int MaxServiceConnectAttempts = 12;

#ifdef Q_OS_WIN
const QString StartupRegistryPath = QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)");
const QString StartupRegistryName = QStringLiteral("SunAndMon");
const QString SavedCenterKey = QStringLiteral("center/protectedKey");
constexpr wchar_t SunshineServiceName[] = L"SunshineService";
constexpr wchar_t SunAndMonServiceName[] = L"SunAndMonHostService";
const QString SunAndMonServicePipeName = QStringLiteral("SunAndMonHostService");

QString serviceBinaryCommand(const QString& serviceExecutable)
{
    return QStringLiteral("\"%1\" --sunandmon-service").arg(QDir::toNativeSeparators(serviceExecutable));
}

bool runElevatedServiceHelper(const QString& serviceExecutable, const wchar_t* argument)
{
    const auto nativePath = QDir::toNativeSeparators(serviceExecutable).toStdWString();
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS;
    request.lpVerb = L"runas";
    request.lpFile = nativePath.c_str();
    request.lpParameters = argument;
    request.nShow = SW_HIDE;
    if (!ShellExecuteExW(&request) || request.hProcess == nullptr) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(request.hProcess, 30000);
    DWORD exitCode = 1;
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(request.hProcess, &exitCode) && exitCode == 0;
    CloseHandle(request.hProcess);
    return ok;
}

template<class Table, class ReadTable>
void addPortOwners(QSet<DWORD>& owners, quint16 port, ReadTable readTable)
{
    DWORD size = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QByteArray storage(static_cast<qsizetype>(size), Qt::Uninitialized);
        const auto result = readTable(size ? storage.data() : nullptr, &size);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result == NO_ERROR && !storage.isEmpty()) {
            const auto* table = reinterpret_cast<const Table*>(storage.constData());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const auto& row = table->table[i];
                if (ntohs(static_cast<quint16>(row.dwLocalPort)) == port) {
                    owners.insert(row.dwOwningPid);
                }
            }
        }
        return;
    }
}

QSet<DWORD> portOwners(int conflict)
{
    QSet<DWORD> owners;
    const auto port = static_cast<quint16>(qAbs(conflict));
    if (conflict > 0) {
        addPortOwners<MIB_TCPTABLE_OWNER_PID>(owners, port, [](void* buffer, DWORD* size) {
            return GetExtendedTcpTable(buffer, size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
        });
        addPortOwners<MIB_TCP6TABLE_OWNER_PID>(owners, port, [](void* buffer, DWORD* size) {
            return GetExtendedTcpTable(buffer, size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
        });
    } else {
        addPortOwners<MIB_UDPTABLE_OWNER_PID>(owners, port, [](void* buffer, DWORD* size) {
            return GetExtendedUdpTable(buffer, size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        });
        addPortOwners<MIB_UDP6TABLE_OWNER_PID>(owners, port, [](void* buffer, DWORD* size) {
            return GetExtendedUdpTable(buffer, size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
        });
    }
    return owners;
}

DWORD sunshineParent(DWORD pid)
{
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD parent = 0;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    for (auto ok = Process32FirstW(snapshot, &process); ok; ok = Process32NextW(snapshot, &process)) {
        if (process.th32ProcessID == pid && _wcsicmp(process.szExeFile, L"sunshine.exe") == 0) {
            parent = process.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}

bool stopSunshineService(SC_HANDLE manager)
{
    if (const auto service = OpenServiceW(manager, SunshineServiceName, SERVICE_STOP)) {
        SERVICE_STATUS status{};
        const bool stopped = ControlService(service, SERVICE_CONTROL_STOP, &status) ||
                             GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
        CloseServiceHandle(service);
        return stopped;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED) {
        return false;
    }

    wchar_t directory[MAX_PATH];
    const auto length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const auto command = std::wstring(directory, length) + L"\\sc.exe";
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"runas";
    request.lpFile = command.c_str();
    request.lpParameters = L"stop SunshineService";
    request.nShow = SW_HIDE;
    return ShellExecuteExW(&request);
}

bool stopPreviousSunshine(int conflict, DWORD pid)
{
    const auto parent = sunshineParent(pid);
    if (parent == 0 || !portOwners(conflict).contains(pid)) {
        return false;
    }
    if (const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        const auto service = OpenServiceW(manager, SunshineServiceName, SERVICE_QUERY_STATUS);
        const bool ownedByService = service &&
            QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) &&
            status.dwProcessId != 0 && (status.dwProcessId == pid || status.dwProcessId == parent);
        if (service) {
            CloseServiceHandle(service);
        }
        if (ownedByService) {
            const bool stopped = stopSunshineService(manager);
            CloseServiceHandle(manager);
            return stopped;
        }
        CloseServiceHandle(manager);
    }

    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        return false;
    }
    wchar_t path[32768];
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool stopped = QueryFullProcessImageNameW(process, 0, path, &length) &&
        QFileInfo(QString::fromWCharArray(path, length)).fileName().compare("sunshine.exe", Qt::CaseInsensitive) == 0 &&
        portOwners(conflict).contains(pid) && TerminateProcess(process, 0);
    CloseHandle(process);
    return stopped;
}

QString startupCommand(const QString& path = QCoreApplication::applicationFilePath())
{
    return QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(path));
}

QByteArray protectCenterKey(const QByteArray& key, const QByteArray& centerUrl, bool encrypt)
{
    if (key.isEmpty()) {
        return {};
    }
    DATA_BLOB input{static_cast<DWORD>(key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(key.constData()))};
    DATA_BLOB entropy{static_cast<DWORD>(centerUrl.size()), reinterpret_cast<BYTE*>(const_cast<char*>(centerUrl.constData()))};
    DATA_BLOB output{};
    const BOOL ok = encrypt
        ? CryptProtectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) {
        return {};
    }
    const QByteArray result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
#elif defined(Q_OS_LINUX)
QString linuxConfigDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        base = QDir::home().filePath(QStringLiteral(".config"));
    }
    const QString dirPath = QDir(base).filePath(QStringLiteral("SunAndMon"));
    QDir().mkpath(dirPath);
    QFile::setPermissions(dirPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return dirPath;
}

QString linuxAutostartFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        base = QDir::home().filePath(QStringLiteral(".config"));
    }
    return QDir(base).filePath(QStringLiteral("autostart/sunandmon.desktop"));
}

QString linuxAutostartExecLine(const QString& path = QCoreApplication::applicationFilePath())
{
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("Exec=\"%1\"").arg(escaped);
}

QByteArray loadSavedCenterKeyLinux(const QString& centerUrl)
{
    if (centerUrl.isEmpty()) {
        return {};
    }
    QFile file(QDir(linuxConfigDir()).filePath(QStringLiteral("center-session.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const auto obj = QJsonDocument::fromJson(file.read(4096)).object();
    if (obj.value(QStringLiteral("url")).toString() != centerUrl) {
        return {};
    }
    const QByteArray key = obj.value(QStringLiteral("key")).toString().toLatin1();
    return CenterKeyFormat.match(QString::fromLatin1(key)).hasMatch() ? key : QByteArray();
}

void saveCenterKeyLinux(const QByteArray& key, const QString& centerUrl)
{
    if (key.isEmpty() || centerUrl.isEmpty()) {
        return;
    }
    const QString path = QDir(linuxConfigDir()).filePath(QStringLiteral("center-session.json"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QJsonObject obj{
        {QStringLiteral("url"), centerUrl},
        {QStringLiteral("key"), QString::fromLatin1(key)}
    };
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    if (file.commit()) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
}

void clearSavedCenterKeyLinux()
{
    QFile::remove(QDir(linuxConfigDir()).filePath(QStringLiteral("center-session.json")));
}
#endif

QString resolveHostConfigDir()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    if (appDir.exists(QStringLiteral("portable.dat"))) {
        const QString portableConfig = appDir.filePath(QStringLiteral("host/config"));
        QDir().mkpath(portableConfig);
        return portableConfig;
    }
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    }
    const QString configDir = QDir(base).filePath(QStringLiteral("SunAndMon/host"));
    QDir().mkpath(configDir);
    return QDir::toNativeSeparators(configDir);
#elif defined(Q_OS_LINUX)
    QString cfgBase = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (cfgBase.isEmpty()) {
        cfgBase = QDir::home().filePath(QStringLiteral(".config"));
    }
    const QString configDir = QDir(cfgBase).filePath(QStringLiteral("sunandmon/host"));
    QDir().mkpath(configDir);
    return configDir;
#else
    return {};
#endif
}

QByteArray randomHex()
{
    QByteArray bytes(32, '\0');
    return RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()) == 1 ?
        bytes.toHex() : QByteArray();
}

QByteArray hostIdentitySeed()
{
#ifdef Q_OS_WIN
    const auto mutex = CreateMutexW(nullptr, FALSE, L"Local\\SunAndMon.Center.HostIdentity");
    if (mutex == nullptr) {
        return {};
    }
    const auto close = qScopeGuard([mutex] { CloseHandle(mutex); });
    const auto wait = WaitForSingleObject(mutex, 2000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        return {};
    }
    const auto unlock = qScopeGuard([mutex] { ReleaseMutex(mutex); });

    // Portable folders share the current Windows account's host identity.
    // Keep its secret separate from exported settings and center login data.
    QSettings settings(QStringLiteral(R"(HKEY_CURRENT_USER\Software\SunAndMon\Center)"), QSettings::NativeFormat);
    const auto stored = settings.value("hostSeed").toByteArray();
    if (stored.size() == 64) {
        return stored;
    }

    const auto seed = randomHex();
    if (seed.isEmpty()) {
        return {};
    }
    settings.setValue("hostSeed", seed);
    settings.sync();
    return settings.status() == QSettings::NoError ? seed : QByteArray();
#elif defined(Q_OS_LINUX)
    const QDir dir(linuxConfigDir());
    QLockFile lock(dir.filePath(QStringLiteral("host-seed.lock")));
    if (!lock.tryLock(2000)) {
        return {};
    }
    const QString seedPath = dir.filePath(QStringLiteral("host-seed"));
    QFile existing(seedPath);
    if (existing.open(QIODevice::ReadOnly)) {
        const QByteArray stored = existing.read(128).trimmed();
        if (stored.size() == 64 && CenterKeyFormat.match(QString::fromLatin1(stored)).hasMatch()) {
            return stored;
        }
    }
    const QByteArray seed = randomHex();
    if (seed.isEmpty()) {
        return {};
    }
    QSaveFile out(seedPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return {};
    }
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    out.write(seed);
    if (!out.commit()) {
        return {};
    }
    QFile::setPermissions(seedPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return seed;
#else
    return {};
#endif
}

QByteArray identityEndpoint(QUrl endpoint)
{
    if ((endpoint.scheme() == "https" && endpoint.port() == 443) ||
        (endpoint.scheme() == "http" && endpoint.port() == 80)) {
        endpoint.setPort(-1);
    }
    return endpoint.toEncoded();
}

bool usableAddress(const QHostAddress& ip)
{
    return !ip.isNull() && !ip.isLoopback() && !ip.isLinkLocal() && !ip.isMulticast() &&
           ip != QHostAddress(QHostAddress::AnyIPv4) && ip != QHostAddress(QHostAddress::AnyIPv6) &&
           ip != QHostAddress(QHostAddress::Broadcast) && ip.scopeId().isEmpty();
}
}

CenterClient::CenterClient(ComputerManager* computers)
    : QObject(computers), m_Computers(computers)
{
    QSettings settings;
    m_EnteredUrl = settings.value("center/url").toString();
    m_HiddenDevices = settings.value(hiddenDevicesKey()).toStringList();
    m_EnteredIp = settings.value("center/directIp").toString();
    m_AdvertisedAddress = settings.value("center/advertisedAddress").toString();
    m_Direct.setProxy(QNetworkProxy::NoProxy);
    m_Engine.setStandardErrorFile(QProcess::nullDevice());
    m_ClipboardAgent.setStandardOutputFile(QProcess::nullDevice());
    m_ClipboardAgent.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    const auto appPath = QFileInfo(QCoreApplication::applicationFilePath());
    if (appPath.fileName().compare("SunAndMon.exe", Qt::CaseInsensitive) == 0 && hostAvailable()) {
        QSettings startup(StartupRegistryPath, QSettings::NativeFormat);
        const auto oldCommand = startupCommand(appPath.dir().filePath("Moonlight.exe"));
        if (startup.value(StartupRegistryName).toString().compare(oldCommand, Qt::CaseInsensitive) == 0) {
            startup.setValue(StartupRegistryName, startupCommand());
            startup.sync();
        }
    }
    const auto hideConsole = [](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    };
    m_Engine.setCreateProcessArgumentsModifier(hideConsole);
    m_ClipboardAgent.setCreateProcessArgumentsModifier(hideConsole);
#endif
    m_Poll.setInterval(10000);
    m_Startup.setInterval(1000);
    m_Heartbeat.setInterval(12000);
    m_ConflictRetry.setInterval(250);
    m_ServiceConnectRetry.setInterval(500);
    m_ServiceConnectRetry.setSingleShot(true);
    connect(&m_Poll, &QTimer::timeout, this, &CenterClient::refresh);
    connect(&m_Startup, &QTimer::timeout, this, &CenterClient::probeHost);
    connect(&m_Heartbeat, &QTimer::timeout, this, &CenterClient::publishHost);
    connect(&m_ServiceConnectRetry, &QTimer::timeout, this, &CenterClient::connectToHostService);
    connect(&m_ConflictRetry, &QTimer::timeout, this, [this] {
#ifdef Q_OS_WIN
        if (portOwners(m_ConflictPort).contains(m_ConflictOwner)) {
            return;
        }
#endif
        m_ConflictRetry.stop();
        startHost(m_AdvertisedAddress);
    });
    connect(&m_Engine, &QProcess::stateChanged, this, &CenterClient::changed);
    connect(&m_Engine, &QProcess::readyReadStandardOutput, this, &CenterClient::readHostPorts);
    connect(&m_Engine, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_Startup.stop();
            withdrawHost();
            setHostStatus(tr("The bundled host could not start."));
        }
    });
    connect(&m_Engine, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        handleHostFinished(exitCode, exitStatus == QProcess::NormalExit);
    });
    connect(&m_ServiceSocket, &QLocalSocket::connected, this, [this] {
        m_ServiceConnectRetry.stop();
        if (!m_UsingService || m_Stopping) {
            m_ServiceSocket.disconnectFromServer();
            return;
        }
        const QByteArray keyField = m_HostKey.isEmpty() ? QByteArrayLiteral("-") : m_HostKey;
        const QByteArray allowAlt = m_AllowAlternatePorts ? QByteArrayLiteral("1") : QByteArrayLiteral("0");
        const QByteArray configDirHex = resolveHostConfigDir().toUtf8().toHex();
        QByteArray command = "START " + m_ManagedToken + " " + keyField + " " + allowAlt;
        if (!configDirHex.isEmpty()) {
            command += " " + configDirHex;
        }
        command += "\n";
        m_ServiceSocket.write(command);
        m_ServiceSocket.flush();
        m_Startup.start();
    });
    connect(&m_ServiceSocket, &QLocalSocket::readyRead, this, &CenterClient::readServiceMessages);
    connect(&m_ServiceSocket, &QLocalSocket::disconnected, this, [this] {
        if (!m_UsingService) {
            return;
        }
        m_ServiceConnectRetry.stop();
        readServiceMessages();
        m_UsingService = false;
        const bool normalExit = m_ServiceHasExitCode || m_Stopping;
        const int exitCode = m_ServiceHasExitCode ? m_ServiceExitCode : 0;
        handleHostFinished(exitCode, normalExit);
    });
    connect(&m_ServiceSocket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (!m_UsingService || m_Stopping) {
            return;
        }
        if (m_ServiceSocket.state() == QLocalSocket::UnconnectedState && m_HttpPort == 0 && !m_ServiceHasExitCode) {
            if (m_ServiceConnectAttempts < MaxServiceConnectAttempts) {
                ++m_ServiceConnectAttempts;
                m_ServiceConnectRetry.start();
                return;
            }
            m_UsingService = false;
            m_ServiceConnectRetry.stop();
            launchLocalHostProcess();
        }
    });
    connect(computers, &ComputerManager::centerConnectFinished, this, [this](const QString& uuid, const QString& error) {
        m_Busy = false;
        QString status = error;
        if (uuid.isEmpty() && !status.isEmpty() && !m_LastConnectHostTunnels.isEmpty() && m_Tunnel) {
            QStringList missing;
            for (const auto& p : m_LastConnectHostTunnels) {
                if (!m_Tunnel->isProviderConnected(p)) {
                    missing.append(p == QStringLiteral("easytier") ? QStringLiteral("EasyTier") : QStringLiteral("Tailscale"));
                }
            }
            if (!missing.isEmpty()) {
                status = tr("Could not reach the host directly. This host advertises built-in tunnel (%1) — connect it in \"Built-in tunnel\" and retry.")
                             .arg(missing.join(QStringLiteral(" / ")));
            }
        }
        m_LastConnectHostTunnels.clear();
        m_CenterStatus = status;
        emit changed();
        if (!uuid.isEmpty() && connected()) {
            emit hostConnected(uuid);
        }
    });
    m_Tunnel = new TunnelManager(this);
    connect(m_Tunnel, &TunnelManager::connectedStateChanged, this, [this] {
        if (hostReady()) {
            m_Tunnel->syncHostExposure(true, m_HttpPort, static_cast<quint16>(m_HttpsPort));
            if (connected()) {
                publishHost();
            }
        }
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &CenterClient::stopHost);
}

CenterClient::~CenterClient()
{
    m_Engine.disconnect(this);
    m_ServiceSocket.disconnect(this);
    m_ServiceConnectRetry.stop();
    stopClipboardAgent();
    if (m_UsingService) {
        m_UsingService = false;
        if (m_ServiceSocket.state() == QLocalSocket::ConnectedState) {
            m_ServiceSocket.write("STOP\n");
            m_ServiceSocket.flush();
            m_ServiceSocket.waitForBytesWritten(1000);
            m_ServiceSocket.disconnectFromServer();
            m_ServiceSocket.waitForDisconnected(3000);
        } else {
            m_ServiceSocket.abort();
        }
    }
    if (m_Engine.state() != QProcess::NotRunning) {
        m_Engine.closeWriteChannel();
        if (!m_Engine.waitForFinished(5000)) {
            m_Engine.kill();
            m_Engine.waitForFinished(1000);
        }
    }
    releasePrimaryInstanceLock();
}

QString CenterClient::hostExecutable() const
{
#ifdef Q_OS_WIN
    return QDir(QCoreApplication::applicationDirPath()).filePath("host/sunshine.exe");
#else
    return QDir(QCoreApplication::applicationDirPath()).filePath("host/sunshine");
#endif
}

QString CenterClient::hostServiceExecutable() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("host/tools/sunshinesvc.exe");
}

bool CenterClient::hostAvailable() const
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    const QFileInfo info(hostExecutable());
    return info.isFile() && info.isExecutable();
#else
    return false;
#endif
}

bool CenterClient::hostServiceAvailable() const
{
#ifdef Q_OS_WIN
    return hostAvailable() && QFileInfo(hostServiceExecutable()).isFile();
#else
    return false;
#endif
}

bool CenterClient::elevatedHostService() const
{
#ifdef Q_OS_WIN
    if (!hostServiceAvailable()) {
        return false;
    }
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    const auto service = OpenServiceW(manager, SunAndMonServiceName, SERVICE_QUERY_CONFIG);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    DWORD bytesNeeded = 0;
    QueryServiceConfigW(service, nullptr, 0, &bytesNeeded);
    bool configured = false;
    if (bytesNeeded > 0) {
        QByteArray buffer(static_cast<qsizetype>(bytesNeeded), Qt::Uninitialized);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, bytesNeeded, &bytesNeeded) &&
            config->dwStartType != SERVICE_DISABLED && config->lpBinaryPathName) {
            const auto expected = serviceBinaryCommand(hostServiceExecutable());
            configured = QString::fromWCharArray(config->lpBinaryPathName)
                .compare(expected, Qt::CaseInsensitive) == 0;
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return configured;
#else
    return false;
#endif
}

bool CenterClient::startHostOnLaunch() const
{
    return QSettings().value(StartHostOnLaunchKey, true).toBool();
}

void CenterClient::setStartHostOnLaunch(bool enabled)
{
    if (startHostOnLaunch() != enabled) {
        QSettings().setValue(StartHostOnLaunchKey, enabled);
        emit startHostOnLaunchChanged();
    }
}

bool CenterClient::openFirewallSettings() const
{
#ifdef Q_OS_WIN
    wchar_t systemDirectory[MAX_PATH];
    const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const QDir directory(QString::fromWCharArray(systemDirectory, length));
        return QProcess::startDetached(directory.filePath("control.exe"),
                                       {"/name", "Microsoft.WindowsFirewall", "/page", "pageConfigureApps"});
    }
#endif
    return false;
}

bool CenterClient::acquirePrimaryInstanceLock()
{
    if (m_PrimaryLockHandle != nullptr) {
        return true;
    }
#ifdef Q_OS_WIN
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\SunAndMon.PrimaryInstance");
    if (mutex == nullptr) {
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return false;
    }
    m_PrimaryLockHandle = mutex;
    return true;
#elif defined(Q_OS_LINUX)
    auto* lock = new QLockFile(QDir(linuxConfigDir()).filePath(QStringLiteral("primary-instance.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        delete lock;
        return false;
    }
    m_PrimaryLockHandle = lock;
    return true;
#else
    return true;
#endif
}

void CenterClient::releasePrimaryInstanceLock()
{
    if (m_PrimaryLockHandle == nullptr) {
        return;
    }
#ifdef Q_OS_WIN
    CloseHandle(static_cast<HANDLE>(m_PrimaryLockHandle));
#elif defined(Q_OS_LINUX)
    auto* lock = static_cast<QLockFile*>(m_PrimaryLockHandle);
    lock->unlock();
    delete lock;
#endif
    m_PrimaryLockHandle = nullptr;
}

void CenterClient::computeLocalHostId()
{
    if (!connected()) {
        return;
    }
    const auto seed = hostIdentitySeed();
    if (seed.isEmpty()) {
        return;
    }
    const auto hostKey = QMessageAuthenticationCode::hash(identityEndpoint(m_Endpoint.url) + "\n" + m_Key, seed,
                                                          QCryptographicHash::Sha256).toHex();
    m_HostId = QString::fromLatin1(QCryptographicHash::hash(hostKey, QCryptographicHash::Sha256).toHex().left(32));
}

void CenterClient::setSecondaryMode(bool enabled)
{
    if (m_SecondaryInstance != enabled) {
        m_SecondaryInstance = enabled;
        emit changed();
    }
}

bool CenterClient::openInNewWindow(const QString& id, const QString& action) const
{
    static const QRegularExpression idFormat("^[a-f0-9]{32}$");
    if (!idFormat.match(id).hasMatch()) {
        return false;
    }
    const QString act = (action == QStringLiteral("apps") || action == QStringLiteral("displays"))
                            ? action
                            : QStringLiteral("desktop");
    return QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                   {QStringLiteral("--center-connect"), id,
                                    QStringLiteral("--center-action"), act});
}

bool CenterClient::openNewClientWindow() const
{
    return QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
}

void CenterClient::reportEmbedLine(const QString& line) const
{
    if (line.isEmpty()) {
        return;
    }
    QByteArray utf8 = line.toUtf8();
    utf8.replace('\r', ' ');
    utf8.replace('\n', ' ');
    utf8.append('\n');
    fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stdout);
    fflush(stdout);
}

void CenterClient::startOnLaunch()
{
    if (m_LaunchHandled || !hostAvailable()) {
        return;
    }
    m_LaunchHandled = true;
    if (!m_SecondaryInstance && !acquirePrimaryInstanceLock()) {
        m_SecondaryInstance = true;
    }
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
#ifdef Q_OS_WIN
    const auto key = protectCenterKey(QSettings().value(SavedCenterKey).toByteArray(), m_EnteredUrl.toUtf8(), false);
#else
    const auto key = loadSavedCenterKeyLinux(m_EnteredUrl);
#endif
    const QUrl savedUrl(m_EnteredUrl);
    const QHostAddress savedIp(m_EnteredIp);
    const bool validRoute = m_EnteredIp.isEmpty() || (savedUrl.scheme() == "https" && !savedIp.isNull());
    if (!key.isEmpty() && validRoute) {
        m_Endpoint = {savedUrl, m_EnteredIp.isEmpty() ? QString() : savedIp.toString()};
        m_Key = key;
        computeLocalHostId();
        m_Poll.start();
    }
#endif
    if (m_Tunnel) {
        m_Tunnel->startOnLaunch(m_SecondaryInstance);
    }
    if (m_SecondaryInstance) {
        setHostStatus(tr("Managed by the main SunAndMon window"));
    } else if (startHostOnLaunch()) {
        startHost(m_AdvertisedAddress);
    }
    refresh();
    loadCenterTunnelDefaults();
}

bool CenterClient::startAtLogin() const
{
#ifdef Q_OS_WIN
    return QSettings(StartupRegistryPath, QSettings::NativeFormat).value(StartupRegistryName).toString() == startupCommand();
#elif defined(Q_OS_LINUX)
    QFile file(linuxAutostartFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    const QString content = QString::fromUtf8(file.read(4096));
    return content.contains(linuxAutostartExecLine()) &&
           !content.contains(QStringLiteral("Hidden=true"));
#else
    return false;
#endif
}

bool CenterClient::setStartAtLogin(bool enabled)
{
#ifdef Q_OS_WIN
    if (hostAvailable()) {
        QSettings settings(StartupRegistryPath, QSettings::NativeFormat);
        if (enabled) {
            settings.setValue(StartupRegistryName, startupCommand());
        } else if (settings.value(StartupRegistryName).toString() == startupCommand()) {
            settings.remove(StartupRegistryName);
        }
        settings.sync();
        emit changed();
        return settings.status() == QSettings::NoError && startAtLogin() == enabled;
    }
#elif defined(Q_OS_LINUX)
    if (hostAvailable()) {
        const QString path = linuxAutostartFilePath();
        bool ok = false;
        if (enabled) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                const QString desktop = QStringLiteral(
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Version=1.0\n"
                    "Name=SunAndMon\n"
                    "Comment=SunAndMon Remote Desktop Client and Host\n"
                    "%1\n"
                    "Icon=sunandmon\n"
                    "Terminal=false\n"
                    "X-GNOME-Autostart-enabled=true\n").arg(linuxAutostartExecLine());
                file.write(desktop.toUtf8());
                ok = file.commit();
            }
        } else {
            ok = !QFileInfo::exists(path) || QFile::remove(path);
        }
        emit changed();
        return ok && startAtLogin() == enabled;
    }
#else
    Q_UNUSED(enabled);
#endif
    return false;
}

bool CenterClient::setElevatedHostService(bool enabled)
{
#ifdef Q_OS_WIN
    if (!hostServiceAvailable()) {
        return false;
    }
    const bool wasActive = hostActive();
    const bool ok = runElevatedServiceHelper(
        hostServiceExecutable(),
        enabled ? L"--install-sunandmon-service" : L"--uninstall-sunandmon-service");
    emit changed();
    if (!ok || elevatedHostService() != enabled) {
        return false;
    }
    if (wasActive && enabled && !m_UsingService) {
        m_RestartHost = true;
        stopHostProcess();
    } else if (wasActive && !enabled) {
        if (hostActive()) {
            m_RestartHost = true;
            stopHostProcess();
        } else {
            startHost(m_AdvertisedAddress);
        }
    }
    return true;
#else
    Q_UNUSED(enabled);
    return false;
#endif
}

QUrl CenterClient::hostSettingsUrl() const
{
    if (!hostActive() || m_WebPort == 0) {
        return {};
    }
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("localhost"));
    url.setPort(m_WebPort);
    url.setPath(QStringLiteral("/"));
    return url;
}

QString CenterClient::computerName() const
{
    return QHostInfo::localHostName();
}

int CenterClient::hostPortConflict() const
{
    return hostActive() || m_ConflictRetry.isActive() ? 0 : qAbs(m_ConflictPort);
}

QNetworkReply* CenterClient::request(const CenterEndpoint& endpoint, const QByteArray& key, const QString& path,
                                    const QByteArray& method, const QByteArray& body, const QByteArray& hostKey)
{
    QUrl url(endpoint.url);
    QString prefix = url.path();
    if (prefix.endsWith('/')) {
        prefix.chop(1);
    }
    const qsizetype queryPos = path.indexOf('?');
    if (queryPos >= 0) {
        url.setPath(prefix + path.left(queryPos));
        url.setQuery(path.mid(queryPos + 1));
    } else {
        url.setPath(prefix + path);
    }
    const bool direct = !endpoint.directIp.isEmpty();
    if (direct) {
        url.setHost(endpoint.directIp);
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(5000);
    if (direct) {
        request.setPeerVerifyName(endpoint.url.host());
        request.setRawHeader("Host", endpoint.url.authority(QUrl::FullyEncoded).toLatin1());
        // Qt's connection cache uses the destination IP, which can serve several TLS names.
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setRawHeader("Connection", "close");
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
        request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, -1);
#endif
    }
    request.setRawHeader("Authorization", "Bearer " + key);
    if (!hostKey.isEmpty()) {
        request.setRawHeader("X-SunAndMon-Host-Key", hostKey);
    }
    if (path.startsWith(DevicesPath + "/") && (method == "PUT" || method == "DELETE")) {
        request.setRawHeader("X-SunAndMon-Registration-Id", m_RegistrationId);
    }
    if (!body.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }
    auto& network = direct || url.scheme() == "http" ? m_Direct : m_Network;
    auto reply = network.sendCustomRequest(request, method, body);
    reply->setReadBufferSize(MaxCenterResponse + 1);
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > MaxCenterResponse) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    return reply;
}

void CenterClient::signIn(const QString& address, const QString& key, const QString& directIp)
{
    if (connected() || m_Busy || m_DirectoryRequest) {
        return;
    }
    QUrl url(address.trimmed(), QUrl::StrictMode);
    const bool local = QHostAddress(url.host()).isLoopback() || url.host() == QStringLiteral("localhost");
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        !(url.scheme() == "https" || (url.scheme() == "http" && local)) || !CenterKeyFormat.match(key.trimmed()).hasMatch()) {
        m_CenterStatus = tr("Enter an HTTPS center URL and a 64-character lowercase hex key.");
        emit changed();
        return;
    }
    const auto ip = directIp.trimmed();
    const QHostAddress destination(ip);
    if (!ip.isEmpty() && (destination.isNull() || url.scheme() != "https")) {
        m_CenterStatus = tr("Direct connections require an HTTPS URL and a numeric IP address.");
        emit changed();
        return;
    }
    auto path = url.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setPath(path);
    loadDevices({url, ip.isEmpty() ? QString() : destination.toString()}, key.trimmed().toLatin1(), true);
}

void CenterClient::loadDevices(const CenterEndpoint& endpoint, const QByteArray& key, bool signingIn)
{
    m_Busy = signingIn;
    auto reply = m_DirectoryRequest = request(endpoint, key, DevicesPath, "GET");
    emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint, key, signingIn] {
        if (m_DirectoryRequest != reply) {
            return;
        }
        m_DirectoryRequest = nullptr;
        m_Busy = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || status != 200 || !doc.isArray()) {
            m_CenterStatus = status == 401 ? tr("The center key was rejected.") : tr("The center is unavailable.");
            m_Devices.clear();
        } else {
            if (signingIn) {
                m_Endpoint = endpoint;
                m_Key = key;
                m_EnteredUrl = endpoint.url.toString();
                m_EnteredIp = endpoint.directIp;
                computeLocalHostId();
                QSettings settings;
                m_HiddenDevices = settings.value(hiddenDevicesKey()).toStringList();
                settings.setValue("center/url", m_EnteredUrl);
                settings.setValue("center/directIp", m_EnteredIp);
#ifdef Q_OS_WIN
                if (hostAvailable()) {
                    const auto protectedKey = protectCenterKey(key, m_EnteredUrl.toUtf8(), true);
                    if (!protectedKey.isEmpty()) {
                        settings.setValue(SavedCenterKey, protectedKey);
                    }
                }
#elif defined(Q_OS_LINUX)
                if (hostAvailable()) {
                    saveCenterKeyLinux(key, m_EnteredUrl);
                }
#endif
                m_Poll.start();
                loadCenterTunnelDefaults();
                if (hostActive() && !m_Stopping) {
                    m_RestartHost = true;
                    stopHostProcess();
                }
            }
            m_ServerDeduplicatesDevices = reply->rawHeader("X-SunAndMon-Deduplicated") == "1";
            m_Devices = doc.array().toVariantList();
            for (auto it = m_Devices.begin(); it != m_Devices.end();) {
                if (it->toMap().value("id").toString() == m_HostId) {
                    it = m_Devices.erase(it);
                } else {
                    ++it;
                }
            }
            m_CenterStatus.clear();
        }
        emit changed();
    });
}

QString CenterClient::hiddenDevicesKey() const
{
    const auto endpointHash = QCryptographicHash::hash(m_EnteredUrl.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("center/hiddenDevices/") + QString::fromLatin1(endpointHash);
}

QVariantList CenterClient::devices() const
{
    QSet<QString> onlineNames;
    if (!m_ServerDeduplicatesDevices) {
        for (const auto& entry : m_Devices) {
            const auto device = entry.toMap();
            const auto id = device.value("id").toString();
            if (device.value("online").toBool() && id != m_HostId && !m_HiddenDevices.contains(id)) {
                onlineNames.insert(device.value("name").toString().toCaseFolded());
            }
        }
    }
    QVariantList visible;
    for (const auto& entry : m_Devices) {
        const auto device = entry.toMap();
        const auto id = device.value("id").toString();
        if (id != m_HostId && !m_HiddenDevices.contains(id) &&
            (device.value("online").toBool() || !onlineNames.contains(device.value("name").toString().toCaseFolded()))) {
            visible.append(entry);
        }
    }
    return visible;
}

void CenterClient::hideDevice(const QString& id)
{
    for (const auto& device : devices()) {
        if (device.toMap().value("id").toString() == id) {
            m_HiddenDevices.append(id);
            QSettings().setValue(hiddenDevicesKey(), m_HiddenDevices);
            emit changed();
            return;
        }
    }
}

void CenterClient::restoreHiddenDevices()
{
    if (!m_HiddenDevices.isEmpty()) {
        m_HiddenDevices.clear();
        QSettings().remove(hiddenDevicesKey());
        emit changed();
    }
}

void CenterClient::refresh()
{
    if (connected() && !m_Busy && !m_DirectoryRequest) {
        loadDevices(m_Endpoint, m_Key, false);
    }
}

void CenterClient::adminSignIn(const QString& key)
{
    if (!connected() || adminBusy() || adminAuthenticated()) {
        return;
    }
    const auto value = key.trimmed();
    if (!CenterKeyFormat.match(value).hasMatch()) {
        m_AdminStatus = tr("Enter a 64-character lowercase hex administrator key.");
        emit adminChanged();
        return;
    }
    m_AdminKey = value.toLatin1();
    sendAdminRequest("GET");
}

void CenterClient::adminSignOut()
{
    if (auto reply = std::exchange(m_AdminRequest, nullptr)) {
        reply->abort();
    }
    m_AdminKey.clear();
    m_AdminAuthenticated = false;
    m_AdminNodes.clear();
    m_AdminStatus.clear();
    emit adminChanged();
}

void CenterClient::refreshAdminNodes()
{
    if (adminAuthenticated()) {
        sendAdminRequest("GET");
    }
}

void CenterClient::editAdminNode(const QString& id, const QString& name, const QStringList& addresses,
                                 const QString& httpPort, const QString& httpsPort)
{
    if (!adminAuthenticated() || adminBusy() || !NodeIdFormat.match(id).hasMatch()) {
        return;
    }
    QJsonObject fields{{"name", name.trimmed()}, {"addresses", QJsonArray::fromStringList(addresses)}};
    for (const auto& port : {std::make_pair(QStringLiteral("port"), httpPort.trimmed()),
                             std::make_pair(QStringLiteral("httpsPort"), httpsPort.trimmed())}) {
        if (port.second.isEmpty()) {
            continue;
        }
        bool valid;
        const auto value = port.second.toUShort(&valid);
        if (!valid || value == 0) {
            m_AdminStatus = tr("Enter a port between 1 and 65535.");
            emit adminChanged();
            return;
        }
        fields.insert(port.first, int(value));
    }
    sendAdminRequest("PATCH", "/" + id, QJsonDocument(fields).toJson(QJsonDocument::Compact));
}

void CenterClient::deleteAdminNode(const QString& id)
{
    if (adminAuthenticated() && NodeIdFormat.match(id).hasMatch()) {
        sendAdminRequest("DELETE", "/" + id);
    }
}

void CenterClient::purgeAdminNode(const QString& id)
{
    if (adminAuthenticated() && NodeIdFormat.match(id).hasMatch()) {
        sendAdminRequest("DELETE", "/" + id + "?permanent=1");
    }
}

void CenterClient::restoreAdminNode(const QString& id)
{
    if (adminAuthenticated() && NodeIdFormat.match(id).hasMatch()) {
        sendAdminRequest("POST", "/" + id + "/restore");
    }
}

void CenterClient::sendAdminRequest(const QByteArray& method, const QString& suffix, const QByteArray& body)
{
    if (!connected() || adminBusy() || m_AdminKey.isEmpty()) {
        return;
    }
    m_AdminStatus.clear();
    auto reply = m_AdminRequest = request(m_Endpoint, m_AdminKey, AdminNodesPath + suffix, method, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, method] {
        if (m_AdminRequest != reply) {
            return;
        }
        m_AdminRequest = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError) {
            if (method == "GET" && status == 200) {
                const auto doc = QJsonDocument::fromJson(reply->readAll());
                if (doc.isArray()) {
                    m_AdminNodes = doc.array().toVariantList();
                    m_AdminAuthenticated = true;
                    emit adminChanged();
                    return;
                }
            } else if (method != "GET" && status == 204) {
                emit adminMutationSucceeded();
                refreshAdminNodes();
                refresh();
                return;
            }
        }

        if (status == 401 || !adminAuthenticated()) {
            m_AdminKey.clear();
            m_AdminAuthenticated = false;
            m_AdminNodes.clear();
        }
        switch (status) {
        case 400: m_AdminStatus = tr("Check the node name, IP addresses and ports."); break;
        case 401: m_AdminStatus = tr("The administrator key was rejected."); break;
        case 404: m_AdminStatus = tr("The node or management API is unavailable. Refresh the list and try again."); break;
        case 503: m_AdminStatus = tr("Node management is disabled on this center."); break;
        default:  m_AdminStatus = tr("Unable to contact node management."); break;
        }
        emit adminChanged();
    });
    emit adminChanged();
}

void CenterClient::signOut()
{
    if (m_Busy) {
        return;
    }
    if (hostActive() && !m_Stopping) {
        m_RestartHost = true;
        stopHostProcess();
    } else {
        withdrawHost();
    }
    if (auto reply = std::exchange(m_DirectoryRequest, nullptr)) {
        reply->abort();
    }
    adminSignOut();
    m_Poll.stop();
#ifdef Q_OS_WIN
    QSettings().remove(SavedCenterKey);
#elif defined(Q_OS_LINUX)
    clearSavedCenterKeyLinux();
#endif
    m_Key.clear();
    m_Devices.clear();
    m_CenterTunnelDefaults.clear();
    m_CenterStatus.clear();
    emit changed();
}

void CenterClient::loadCenterTunnelDefaults()
{
    if (!connected()) {
        return;
    }
    auto* reply = request(m_Endpoint, m_Key, TunnelConfigPath, "GET");
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (!connected() || reply->error() != QNetworkReply::NoError ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            return;
        }
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            return;
        }
        const QVariantMap map = doc.object().toVariantMap();
        if (map != m_CenterTunnelDefaults) {
            m_CenterTunnelDefaults = map;
            emit changed();
        }
    });
}

void CenterClient::connectDevice(const QString& id)
{
    static const QRegularExpression idFormat("^[a-f0-9]{32}$");
    if (!connected() || m_Busy || id == m_HostId || !idFormat.match(id).hasMatch()) {
        return;
    }
    if (auto refreshReply = std::exchange(m_DirectoryRequest, nullptr)) {
        refreshReply->abort();
    }
    const auto cert = IdentityManager::get()->getSslConfig().localCertificate();
    if (cert.isNull()) {
        m_CenterStatus = tr("The local client certificate is unavailable.");
        emit changed();
        return;
    }
    const QJsonObject body{{"fingerprint", QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex())}};
    m_Busy = true;
    m_CenterStatus = tr("Connecting to the host…");
    auto reply = m_DirectoryRequest = request(m_Endpoint, m_Key, DevicesPath + "/" + id + "/pair", "POST",
                                              QJsonDocument(body).toJson(QJsonDocument::Compact));
    emit changed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
        m_DirectoryRequest = nullptr;
        auto grant = QJsonDocument::fromJson(reply->readAll()).object();
        grant.remove(QStringLiteral("tunnelLocalAddress"));
        grant.remove(QStringLiteral("tunnelLocalAddresses"));
        grant.remove(QStringLiteral("preferTunnelFirst"));
        if (reply->error() != QNetworkReply::NoError ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 ||
            grant.value("id").toString() != id) {
            m_Busy = false;
            m_CenterStatus = tr("The center could not authorize this host. Refresh the device list and try again.");
            emit changed();
            return;
        }

        // Collect all tunnel endpoints advertised by the host (supports Tailscale + EasyTier simultaneously).
        QList<QPair<QString, QString>> hostTunnels;
        QStringList hostProviders;
        const auto tunnelsArr = grant.value(QStringLiteral("tunnels")).toArray();
        for (const auto& val : tunnelsArr) {
            const auto obj = val.toObject();
            const QString p = obj.value(QStringLiteral("provider")).toString().trimmed().toLower();
            const QString a = obj.value(QStringLiteral("address")).toString().trimmed();
            if (!p.isEmpty() && !a.isEmpty()) {
                hostTunnels.append({p, a});
                if (!hostProviders.contains(p)) hostProviders.append(p);
            }
        }
        if (hostTunnels.isEmpty()) {
            const QString lp = grant.value(QStringLiteral("tunnelProvider")).toString().trimmed().toLower();
            const QString la = grant.value(QStringLiteral("tunnelAddress")).toString().trimmed();
            if (!lp.isEmpty() && !la.isEmpty()) {
                hostTunnels.append({lp, la});
                hostProviders.append(lp);
            }
        }
        m_LastConnectHostTunnels = hostProviders;

        const quint16 grantPort = static_cast<quint16>(grant.value("port").toInt());
        const quint16 grantHttpsPort = static_cast<quint16>(grant.value("httpsPort").toInt());
        if (m_Tunnel && !hostTunnels.isEmpty() && grantPort > 0 && grantHttpsPort > 0) {
            m_Tunnel->ensureForwardsAsync(hostTunnels, grantPort, grantHttpsPort,
                                          [this, grant](const QStringList& localHosts) mutable {
                                              if (!localHosts.isEmpty()) {
                                                  QJsonArray arr;
                                                  for (const auto& h : localHosts) {
                                                      arr.append(h);
                                                  }
                                                  grant.insert(QStringLiteral("tunnelLocalAddresses"), arr);
                                                  grant.insert(QStringLiteral("preferTunnelFirst"), m_Tunnel->preferTunnelFirst());
                                              }
                                              m_Computers->connectFromCenter(grant);
                                          });
            return;
        }
        m_Computers->connectFromCenter(grant);
    });
}

void CenterClient::startHost(const QString& address)
{
    if (m_SecondaryInstance) {
        setHostStatus(tr("Managed by the main SunAndMon window"));
        return;
    }
    if (!hostAvailable() || hostActive() || m_ConflictRetry.isActive()) {
        return;
    }
    if (m_Registration || m_Withdrawal) {
        setHostStatus(tr("Waiting for the previous host registration to close."));
        return;
    }
    const QString overrideAddress = address.trimmed();
    if (!overrideAddress.isEmpty() && !usableAddress(QHostAddress(overrideAddress))) {
        setHostStatus(tr("Enter a usable IPv4 or IPv6 address, or leave it blank for automatic discovery."));
        return;
    }
    QSettings settings;
    m_AdvertisedAddress = overrideAddress;
    settings.setValue("center/advertisedAddress", m_AdvertisedAddress);
    m_HostKey.clear();
    m_HostId.clear();
    if (connected()) {
        const auto seed = hostIdentitySeed();
        if (m_RegistrationId.isEmpty()) {
            m_RegistrationId = randomHex();
        }
        if (seed.isEmpty() || m_RegistrationId.isEmpty()) {
            setHostStatus(tr("Unable to generate the host key."));
            return;
        }
        m_HostKey = QMessageAuthenticationCode::hash(identityEndpoint(m_Endpoint.url) + "\n" + m_Key, seed,
                                                     QCryptographicHash::Sha256).toHex();
        m_HostId = QString::fromLatin1(QCryptographicHash::hash(m_HostKey, QCryptographicHash::Sha256).toHex().left(32));
    }
    launchHost();
}

void CenterClient::launchHost()
{
    m_StartupTimedOut = false;
    m_ConflictPort = 0;
    m_ConflictOwner = 0;
    m_ServiceBuffer.clear();
    m_ServiceHasExitCode = false;
    m_ServiceExitCode = 0;
    m_ManagedToken = randomHex();
    if (m_ManagedToken.isEmpty()) {
        withdrawHost();
        setHostStatus(tr("Unable to start a secure local host session."));
        return;
    }
    setHostStatus(tr("Starting the local host…"));
    if (elevatedHostService()) {
        m_UsingService = true;
        m_ServiceConnectAttempts = 0;
        emit changed();
        connectToHostService();
    } else {
        launchLocalHostProcess();
    }
    const auto token = m_ManagedToken;
    QTimer::singleShot(25000, this, [this, token] {
        if (hostActive() && !m_Stopping && !hostReady() && m_ManagedToken == token) {
            m_StartupTimedOut = true;
            setHostStatus(tr("Sunshine started but did not respond to the readiness check. Check the host log and port settings."));
        }
    });
}

void CenterClient::launchLocalHostProcess()
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SUNANDMON_MANAGED_TOKEN", QString::fromLatin1(m_ManagedToken));
    environment.insert("SUNANDMON_HOST_KEY", QString::fromLatin1(m_HostKey));
    environment.insert("SUNANDMON_ALLOW_ALTERNATE_PORTS", m_AllowAlternatePorts ? "1" : "0");
    const QString configDir = resolveHostConfigDir();
    if (!configDir.isEmpty()) {
        environment.insert("SUNANDMON_CONFIG_DIR", configDir);
    }
    m_Engine.setProcessEnvironment(environment);
    m_Engine.setWorkingDirectory(QFileInfo(hostExecutable()).absolutePath());
    m_Startup.start();
    m_Engine.start(hostExecutable(), {});
}

void CenterClient::connectToHostService()
{
#ifdef Q_OS_WIN
    if (!m_UsingService || m_Stopping) {
        return;
    }
    m_ServiceSocket.abort();
    m_ServiceSocket.connectToServer(SunAndMonServicePipeName, QIODevice::ReadWrite);
#endif
}

void CenterClient::handleHostOutputLine(const QByteArray& line)
{
    if (line.startsWith(HostPortConflictPrefix)) {
        const int conflict = line.mid(HostPortConflictPrefix.size()).toInt();
        if (conflict >= -65535 && conflict <= 65535) {
            m_ConflictPort = conflict;
        }
        return;
    }
    if (!line.startsWith(HostPortPrefix)) {
        return;
    }
    const auto ports = line.mid(HostPortPrefix.size()).split(' ');
    if (ports.size() != 2) {
        return;
    }
    const auto httpPort = ports[0].toUShort();
    const auto webPort = ports[1].toUShort();
    if (httpPort != 0 && webPort != 0) {
        m_HttpPort = httpPort;
        m_WebPort = webPort;
        emit changed();
        probeHost();
    }
}

void CenterClient::handleHostFinished(int exitCode, bool normalExit)
{
    m_Startup.stop();
    m_Heartbeat.stop();
    stopClipboardAgent();
    m_PublicCertificate.clear();
    m_HttpPort = 0;
    m_WebPort = 0;
    if (!m_Stopping && normalExit && exitCode == HostRestartExitCode) {
        launchHost();
        return;
    }
    const bool stoppedByClient = m_Stopping;
    m_Stopping = false;
    if (!stoppedByClient && normalExit && exitCode == HostPortsUnavailableExitCode) {
#ifdef Q_OS_WIN
        if (m_ConflictPort != 0) {
            const auto owners = portOwners(m_ConflictPort);
            if (owners.size() == 1 && sunshineParent(*owners.cbegin()) != 0) {
                m_ConflictOwner = *owners.cbegin();
            }
        }
#endif
        setHostStatus(m_ConflictPort != 0
            ? tr("Local port %1 is already in use. Choose how to start hosting.").arg(qAbs(m_ConflictPort))
            : tr("No available ports were found for the local host. Check Sunshine's network settings."));
    } else {
        withdrawHost();
        m_ConflictPort = 0;
        m_ConflictOwner = 0;
        setHostStatus(stoppedByClient ? tr("Host disabled") : tr("The host exited. Check the Sunshine log and local ports."));
    }
    resumeHostIfRequested();
}

void CenterClient::readHostPorts()
{
    while (m_HttpPort == 0 && m_Engine.canReadLine()) {
        handleHostOutputLine(m_Engine.readLine().trimmed());
    }
    if (m_HttpPort != 0 || m_Engine.bytesAvailable() > 4096) {
        m_Engine.readAllStandardOutput();
    }
}

void CenterClient::readServiceMessages()
{
    m_ServiceBuffer.append(m_ServiceSocket.readAll());
    qsizetype newline = -1;
    while ((newline = m_ServiceBuffer.indexOf('\n')) >= 0) {
        const auto line = m_ServiceBuffer.left(newline).trimmed();
        m_ServiceBuffer.remove(0, newline + 1);
        if (line.startsWith("EXIT ")) {
            m_ServiceHasExitCode = true;
            m_ServiceExitCode = line.mid(5).toInt();
        } else if (line.startsWith("ERROR ")) {
            m_ServiceHasExitCode = true;
            m_ServiceExitCode = 1;
        } else if (m_HttpPort == 0) {
            handleHostOutputLine(line);
        }
    }
    if (m_ServiceBuffer.size() > 4096) {
        m_ServiceBuffer.clear();
    }
}

void CenterClient::stopHost()
{
    m_ConflictRetry.stop();
    m_RestartHost = false;
    stopHostProcess();
}

void CenterClient::startHostOnAlternatePorts()
{
    if (hostPortConflict() != 0) {
        m_AllowAlternatePorts = true;
        startHost(m_AdvertisedAddress);
    }
}

void CenterClient::stopConflictingHostAndRetry()
{
#ifdef Q_OS_WIN
    if (hostPortConflict() == 0 || !canStopConflictingHost()) {
        return;
    }
    if (!portOwners(m_ConflictPort).contains(m_ConflictOwner)) {
        startHost(m_AdvertisedAddress);
        return;
    }
    if (!stopPreviousSunshine(m_ConflictPort, m_ConflictOwner)) {
        setHostStatus(tr("Could not stop the previous Sunshine. You can start hosting on available ports."));
        return;
    }

    m_ConflictRetry.start();
    setHostStatus(tr("Waiting for the previous Sunshine to release its port…"));
    const auto token = m_ManagedToken;
    QTimer::singleShot(15000, this, [this, token] {
        if (token == m_ManagedToken && m_ConflictRetry.isActive()) {
            m_ConflictRetry.stop();
            setHostStatus(tr("The previous Sunshine is still using its port. You can start hosting on available ports."));
        }
    });
#endif
}

void CenterClient::resumeHostIfRequested()
{
    if (m_RestartHost && !hostActive() && !m_Registration && !m_Withdrawal) {
        m_RestartHost = false;
        startHost(m_AdvertisedAddress);
    }
}

void CenterClient::stopHostProcess()
{
    if (!hostActive() || m_Stopping) {
        return;
    }
    m_Stopping = true;
    m_ServiceConnectRetry.stop();
    m_Startup.stop();
    m_Heartbeat.stop();
    withdrawHost();
    setHostStatus(tr("Stopping the local host…"));
    if (m_UsingService) {
        if (m_ServiceSocket.state() == QLocalSocket::ConnectedState) {
            m_ServiceSocket.write("STOP\n");
            m_ServiceSocket.flush();
            m_ServiceSocket.disconnectFromServer();
            const auto token = m_ManagedToken;
            QTimer::singleShot(12000, this, [this, token] {
                if (m_Stopping && m_ManagedToken == token && m_UsingService) {
                    m_ServiceSocket.abort();
                }
            });
        } else {
            m_ServiceSocket.abort();
            m_UsingService = false;
            handleHostFinished(0, true);
        }
        return;
    }
    m_Engine.closeWriteChannel();
    const auto token = m_ManagedToken;
    QTimer::singleShot(12000, this, [this, token] {
        if (m_Stopping && m_ManagedToken == token && m_Engine.state() != QProcess::NotRunning) {
            m_Engine.kill();
        }
    });
}

void CenterClient::probeHost()
{
    if (!hostActive() || m_Stopping || m_StatusRequest || m_HttpPort == 0) {
        return;
    }
    QUrl endpoint(QStringLiteral("http://127.0.0.1/sunandmon-status"));
    endpoint.setPort(m_HttpPort);
    QNetworkRequest statusRequest(endpoint);
    statusRequest.setTransferTimeout(1500);
    statusRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto reply = m_StatusRequest = m_Direct.get(statusRequest);
    const auto token = m_ManagedToken;
    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        reply->deleteLater();
        m_StatusRequest = nullptr;
        const auto bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || m_Stopping || !hostActive() || token != m_ManagedToken ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || !bytes.startsWith(token + "\n")) {
            return;
        }
        const auto endPort = bytes.indexOf('\n', token.size() + 1);
        if (endPort < 0) {
            return;
        }
        const auto port = bytes.mid(token.size() + 1, endPort - token.size() - 1).toUShort();
        const QSslCertificate cert(bytes.mid(endPort + 1));
        if (port == 0 || cert.isNull()) {
            return;
        }
        m_HttpsPort = port;
        m_PublicCertificate = cert.toPem();
        m_Startup.stop();
        m_StartupTimedOut = false;
        startClipboardAgent();
        setHostStatus(tr("Ready to receive connections"));
        if (connected()) {
            publishHost();
            m_Heartbeat.start();
        }
    });
}

void CenterClient::startClipboardAgent()
{
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    if (m_ClipboardAgent.state() != QProcess::NotRunning || m_WebPort == 0) {
        return;
    }
#ifdef Q_OS_WIN
    const auto helper = QDir(QCoreApplication::applicationDirPath()).filePath("moonlight-clipboard-helper.exe");
#else
    const auto helper = QDir(QCoreApplication::applicationDirPath()).filePath("moonlight-clipboard-helper");
#endif
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SUNANDMON_CLIPBOARD_TOKEN", QString::fromLatin1(m_ManagedToken));
    environment.insert("SUNANDMON_CLIPBOARD_CERT", QString::fromLatin1(m_PublicCertificate.toBase64()));
    m_ClipboardAgent.setProcessEnvironment(environment);
    m_ClipboardAgent.start(helper, {"--host-agent", QString::number(m_WebPort)});
#endif
}

void CenterClient::stopClipboardAgent()
{
    if (m_ClipboardAgent.state() != QProcess::NotRunning) {
        m_ClipboardAgent.closeWriteChannel();
        if (!m_ClipboardAgent.waitForFinished(1500)) {
            m_ClipboardAgent.kill();
            m_ClipboardAgent.waitForFinished(500);
        }
    }
}

void CenterClient::publishHost()
{
    if (!connected() || !hostActive() || m_Stopping || m_Registration || m_PublicCertificate.isEmpty()) {
        return;
    }
    if (m_Tunnel && m_Tunnel->state() == QStringLiteral("connected")) {
        m_Tunnel->syncHostExposure(true, m_HttpPort, static_cast<quint16>(m_HttpsPort));
    }
    QJsonArray addresses;
    if (!m_AdvertisedAddress.isEmpty()) {
        addresses.append(QHostAddress(m_AdvertisedAddress).toString());
    }
    for (const auto& ip : QNetworkInterface::allAddresses()) {
        if (usableAddress(ip) && !addresses.contains(ip.toString()) && addresses.size() < 16) {
            addresses.append(ip.toString());
        }
    }
    QJsonArray tunnelsArr;
    QString primaryTunnelProvider;
    QString primaryTunnelAddress;
    if (m_Tunnel) {
        const auto activeTunnels = m_Tunnel->connectedHostTunnels();
        for (const auto& ep : activeTunnels) {
            tunnelsArr.append(QJsonObject{
                {QStringLiteral("provider"), ep.first},
                {QStringLiteral("address"), ep.second},
            });
            if (primaryTunnelProvider.isEmpty()) {
                primaryTunnelProvider = ep.first;
                primaryTunnelAddress = ep.second;
            }
            if (!addresses.contains(ep.second) && addresses.size() < 16) {
                addresses.append(ep.second);
            }
        }
    }
    if (addresses.isEmpty()) {
        setHostStatus(tr("The host has no reachable network address to publish."));
        return;
    }
    QJsonObject body{{"name", QHostInfo::localHostName().left(100)}, {"addresses", addresses},
                     {"port", m_HttpPort}, {"httpsPort", m_HttpsPort},
                     {"certificate", QString::fromLatin1(m_PublicCertificate)},
                     {"hostKey", QString::fromLatin1(m_HostKey)}};
    if (!tunnelsArr.isEmpty()) {
        body.insert(QStringLiteral("tunnels"), tunnelsArr);
        body.insert(QStringLiteral("tunnelProvider"), primaryTunnelProvider);
        body.insert(QStringLiteral("tunnelAddress"), primaryTunnelAddress);
    }
    const auto token = m_ManagedToken;
    auto reply = m_Registration = request(m_Endpoint, m_Key, DevicesPath + "/" + m_HostId, "PUT",
                                         QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, token] {
        m_Registration = nullptr;
        if (m_Stopping || !hostActive() || token != m_ManagedToken) {
            return;
        }
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 409) {
            setHostStatus(tr("Another copy is already registered. Waiting for it to disconnect…"));
        } else if (reply->error() == QNetworkReply::NoError && status == 204) {
            const bool firstRegistration = m_HostStatus != tr("Available through the center");
            setHostStatus(tr("Available through the center"));
            if (firstRegistration) {
                refresh();
            }
        } else {
            setHostStatus(status > 0 ? tr("The center rejected this registration (HTTP %1). Retrying…").arg(status)
                                     : tr("The center could not register this host. Retrying…"));
        }
    });
}

void CenterClient::withdrawHost()
{
    if (m_Tunnel) {
        m_Tunnel->syncHostExposure(false, 0, 0);
    }
    if (!connected() || m_HostId.isEmpty() || m_HostKey.isEmpty()) {
        return;
    }
    const auto withdraw = [this, endpoint = m_Endpoint, key = m_Key, id = m_HostId, hostKey = m_HostKey] {
        auto reply = m_Withdrawal = request(endpoint, key, DevicesPath + "/" + id, "DELETE", {}, hostKey);
        connect(reply, &QNetworkReply::finished, this, [this] {
            m_Withdrawal = nullptr;
            resumeHostIfRequested();
        });
    };
    if (m_Registration) {
        connect(m_Registration, &QNetworkReply::finished, this, withdraw);
    } else {
        withdraw();
    }
    m_HostKey.clear();
}

void CenterClient::setHostStatus(const QString& status)
{
    m_HostStatus = status;
    emit changed();
}
