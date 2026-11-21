#pragma once

#include <QLocalSocket>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class ComputerManager;
class QNetworkReply;

class CenterClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString centerStatus READ centerStatus NOTIFY changed)
    Q_PROPERTY(QString centerUrl READ centerUrl NOTIFY changed)
    Q_PROPERTY(QString centerIp READ centerIp NOTIFY changed)
    Q_PROPERTY(bool adminAuthenticated READ adminAuthenticated NOTIFY adminChanged)
    Q_PROPERTY(bool adminBusy READ adminBusy NOTIFY adminChanged)
    Q_PROPERTY(QString adminStatus READ adminStatus NOTIFY adminChanged)
    Q_PROPERTY(QVariantList adminNodes READ adminNodes NOTIFY adminChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(int hiddenDeviceCount READ hiddenDeviceCount NOTIFY changed)
    Q_PROPERTY(bool hostAvailable READ hostAvailable CONSTANT)
    Q_PROPERTY(bool hostServiceAvailable READ hostServiceAvailable CONSTANT)
    Q_PROPERTY(bool elevatedHostService READ elevatedHostService NOTIFY changed)
    Q_PROPERTY(bool startHostOnLaunch READ startHostOnLaunch WRITE setStartHostOnLaunch NOTIFY startHostOnLaunchChanged)
    Q_PROPERTY(bool hostActive READ hostActive NOTIFY changed)
    Q_PROPERTY(bool hostReady READ hostReady NOTIFY changed)
    Q_PROPERTY(bool hostStopping READ hostStopping NOTIFY changed)
    Q_PROPERTY(bool hostStartupTimedOut READ hostStartupTimedOut NOTIFY changed)
    Q_PROPERTY(int hostPortConflict READ hostPortConflict NOTIFY changed)
    Q_PROPERTY(bool canStopConflictingHost READ canStopConflictingHost NOTIFY changed)
    Q_PROPERTY(QString computerName READ computerName CONSTANT)
    Q_PROPERTY(QString hostStatus READ hostStatus NOTIFY changed)
    Q_PROPERTY(QUrl hostSettingsUrl READ hostSettingsUrl NOTIFY changed)
    Q_PROPERTY(bool startAtLogin READ startAtLogin NOTIFY changed)
    Q_PROPERTY(QString advertisedAddress READ advertisedAddress NOTIFY changed)

public:
    explicit CenterClient(ComputerManager* computers);
    ~CenterClient() override;

    bool connected() const { return !m_Key.isEmpty(); }
    bool busy() const { return m_Busy; }
    QString centerStatus() const { return m_CenterStatus; }
    QString centerUrl() const { return m_EnteredUrl; }
    QString centerIp() const { return m_EnteredIp; }
    bool adminAuthenticated() const { return m_AdminAuthenticated; }
    bool adminBusy() const { return m_AdminRequest != nullptr; }
    QString adminStatus() const { return m_AdminStatus; }
    QVariantList adminNodes() const { return m_AdminNodes; }
    QVariantList devices() const;
    int hiddenDeviceCount() const { return m_HiddenDevices.size(); }
    bool hostAvailable() const;
    bool hostServiceAvailable() const;
    bool elevatedHostService() const;
    bool startHostOnLaunch() const;
    void setStartHostOnLaunch(bool enabled);
    bool hostActive() const { return m_Engine.state() != QProcess::NotRunning || m_UsingService; }
    bool hostReady() const { return hostActive() && !m_Stopping && !m_PublicCertificate.isEmpty(); }
    bool hostStopping() const { return m_Stopping; }
    bool hostStartupTimedOut() const { return hostActive() && m_StartupTimedOut; }
    int hostPortConflict() const;
    bool canStopConflictingHost() const { return m_ConflictOwner != 0 && !m_ConflictRetry.isActive(); }
    QString computerName() const;
    QString hostStatus() const { return m_HostStatus; }
    QUrl hostSettingsUrl() const;
    bool startAtLogin() const;
    QString advertisedAddress() const { return m_AdvertisedAddress; }

    Q_INVOKABLE void signIn(const QString& url, const QString& key, const QString& directIp = {});
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void adminSignIn(const QString& key);
    Q_INVOKABLE void adminSignOut();
    Q_INVOKABLE void refreshAdminNodes();
    Q_INVOKABLE void editAdminNode(const QString& id, const QString& name, const QStringList& addresses,
                                  const QString& httpPort, const QString& httpsPort);
    Q_INVOKABLE void deleteAdminNode(const QString& id);
    Q_INVOKABLE void restoreAdminNode(const QString& id);
    Q_INVOKABLE void hideDevice(const QString& id);
    Q_INVOKABLE void restoreHiddenDevices();
    Q_INVOKABLE void connectDevice(const QString& id);
    Q_INVOKABLE void startOnLaunch();
    Q_INVOKABLE bool openFirewallSettings() const;
    Q_INVOKABLE bool setStartAtLogin(bool enabled);
    Q_INVOKABLE bool setElevatedHostService(bool enabled);
    Q_INVOKABLE void startHost(const QString& advertisedAddress);
    Q_INVOKABLE void stopHost();
    Q_INVOKABLE void startHostOnAlternatePorts();
    Q_INVOKABLE void stopConflictingHostAndRetry();

signals:
    void changed();
    void adminChanged();
    void adminMutationSucceeded();
    void startHostOnLaunchChanged();
    void hostConnected(QString uuid);

private:
    struct CenterEndpoint {
        QUrl url;
        QString directIp;
    };

    QString hostExecutable() const;
    QString hostServiceExecutable() const;
    QString hiddenDevicesKey() const;
    QNetworkReply* request(const CenterEndpoint& endpoint, const QByteArray& key, const QString& path,
                           const QByteArray& method, const QByteArray& body = {},
                           const QByteArray& hostKey = {});
    void loadDevices(const CenterEndpoint& endpoint, const QByteArray& key, bool signingIn);
    void sendAdminRequest(const QByteArray& method, const QString& suffix = {}, const QByteArray& body = {});
    void launchHost();
    void launchLocalHostProcess();
    void connectToHostService();
    void stopHostProcess();
    void resumeHostIfRequested();
    void handleHostOutputLine(const QByteArray& line);
    void handleHostFinished(int exitCode, bool normalExit);
    void readHostPorts();
    void readServiceMessages();
    void probeHost();
    void startClipboardAgent();
    void stopClipboardAgent();
    void publishHost();
    void withdrawHost();
    void setHostStatus(const QString& status);

    ComputerManager* m_Computers;
    QNetworkAccessManager m_Network;
    QNetworkAccessManager m_Direct;
    QProcess m_Engine;
    QLocalSocket m_ServiceSocket;
    QTimer m_ServiceConnectRetry;
    QByteArray m_ServiceBuffer;
    QProcess m_ClipboardAgent;
    QTimer m_Poll;
    QTimer m_Startup;
    QTimer m_Heartbeat;
    QTimer m_ConflictRetry;
    CenterEndpoint m_Endpoint;
    QByteArray m_Key;
    QByteArray m_HostKey;
    QByteArray m_RegistrationId;
    QByteArray m_ManagedToken;
    QByteArray m_PublicCertificate;
    QString m_HostId;
    quint16 m_HttpPort = 0;
    quint16 m_WebPort = 0;
    int m_HttpsPort = 0;
    QString m_EnteredUrl;
    QString m_EnteredIp;
    QString m_AdvertisedAddress;
    QString m_CenterStatus;
    QString m_HostStatus;
    QVariantList m_Devices;
    QStringList m_HiddenDevices;
    bool m_Busy = false;
    bool m_ServerDeduplicatesDevices = false;
    bool m_Stopping = false;
    bool m_UsingService = false;
    int m_ServiceConnectAttempts = 0;
    bool m_ServiceHasExitCode = false;
    int m_ServiceExitCode = 0;
    bool m_LaunchHandled = false;
    bool m_RestartHost = false;
    bool m_StartupTimedOut = false;
    bool m_AllowAlternatePorts = false;
    int m_ConflictPort = 0;
    quint32 m_ConflictOwner = 0;
    QNetworkReply* m_DirectoryRequest = nullptr;
    QNetworkReply* m_AdminRequest = nullptr;
    QByteArray m_AdminKey;
    QVariantList m_AdminNodes;
    QString m_AdminStatus;
    bool m_AdminAuthenticated = false;
    QNetworkReply* m_StatusRequest = nullptr;
    QNetworkReply* m_Registration = nullptr;
    QNetworkReply* m_Withdrawal = nullptr;
};
