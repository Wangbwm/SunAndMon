#pragma once

#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>
#include <functional>

class TunnelManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool easytierAvailable READ easytierAvailable NOTIFY changed)
    Q_PROPERTY(QString provider READ provider NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString virtualIp READ virtualIp NOTIFY changed)
    Q_PROPERTY(QString exposeIp READ exposeIp NOTIFY changed)
    Q_PROPERTY(QString authUrl READ authUrl NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)
    Q_PROPERTY(int peersOnline READ peersOnline NOTIFY changed)
    Q_PROPERTY(bool hostExposed READ hostExposed NOTIFY changed)
    Q_PROPERTY(bool autoConnect READ autoConnect NOTIFY changed)
    Q_PROPERTY(QString hostname READ hostname NOTIFY changed)
    Q_PROPERTY(QString tsControlUrl READ tsControlUrl NOTIFY changed)
    Q_PROPERTY(QString tsAuthKey READ tsAuthKey NOTIFY changed)
    Q_PROPERTY(bool tsUseWebOAuth READ tsUseWebOAuth NOTIFY changed)
    Q_PROPERTY(QString etNetworkName READ etNetworkName NOTIFY changed)
    Q_PROPERTY(QString etNetworkSecret READ etNetworkSecret NOTIFY changed)
    Q_PROPERTY(QString etPeers READ etPeers NOTIFY changed)
    Q_PROPERTY(QString etVirtualIp READ etVirtualIp NOTIFY changed)
    Q_PROPERTY(QString etCoreBinary READ etCoreBinary NOTIFY changed)

public:
    explicit TunnelManager(QObject* parent = nullptr);
    ~TunnelManager() override;

    bool available() const;
    bool easytierAvailable() const { return m_EasyTierAvailable; }
    QString provider() const { return m_Provider; }
    QString state() const { return m_State; }
    QString virtualIp() const { return m_VirtualIp; }
    QString exposeIp() const { return m_ExposeIp; }
    QString authUrl() const { return m_AuthUrl; }
    QString errorText() const { return m_ErrorText; }
    int peersOnline() const { return m_PeersOnline; }
    bool hostExposed() const { return m_HostExposed; }
    bool autoConnect() const { return m_AutoConnect; }
    QString hostname() const { return m_Hostname; }
    QString tsControlUrl() const { return m_TsControlUrl; }
    QString tsAuthKey() const { return m_TsAuthKey; }
    bool tsUseWebOAuth() const { return m_TsUseWebOAuth; }
    QString etNetworkName() const { return m_EtNetworkName; }
    QString etNetworkSecret() const { return m_EtNetworkSecret; }
    QString etPeers() const { return m_EtPeers; }
    QString etVirtualIp() const { return m_EtVirtualIp; }
    QString etCoreBinary() const { return m_EtCoreBinary; }

    Q_INVOKABLE void startOnLaunch();
    Q_INVOKABLE void saveAndConnect(const QString& provider,
                                    bool autoConnect,
                                    const QString& hostname,
                                    const QString& tsControlUrl,
                                    const QString& tsAuthKey,
                                    bool tsUseWebOAuth,
                                    const QString& etNetworkName,
                                    const QString& etNetworkSecret,
                                    const QString& etPeers,
                                    const QString& etVirtualIp,
                                    const QString& etCoreBinary);
    Q_INVOKABLE void stopTunnel();
    Q_INVOKABLE void openAuthUrl() const;
    Q_INVOKABLE void applyCenterDefaults(const QVariantMap& defaults);
    Q_INVOKABLE void refreshCapabilities();

    void syncHostExposure(bool hostReady, quint16 httpPort, quint16 httpsPort);
    void ensureForwardAsync(const QString& remoteIp, quint16 httpPort, quint16 httpsPort,
                            std::function<void(QString localHost, QString error)> callback);

signals:
    void changed();
    void connectedStateChanged();

private:
    QString daemonExecutable() const;
    QString tunnelStateDir() const;
    void loadSettings();
    void saveSettings() const;
    void ensureDaemonRunning();
    void stopDaemon();
    void sendStartRequest();
    void pollStatus();
    void queryCapabilities();
    void sendExposeRequest();
    QNetworkReply* apiRequest(const QByteArray& method, const QString& path, const QByteArray& body = {});

    QNetworkAccessManager m_Network;
    QProcess m_Daemon;
    QTimer m_PollTimer;
    QByteArray m_StdoutBuf;
    QByteArray m_Token;
    quint16 m_DaemonPort = 0;
    bool m_DaemonReady = false;
    bool m_PendingStart = false;

    bool m_EasyTierAvailable = false;
    QString m_Provider = QStringLiteral("none");
    QString m_State = QStringLiteral("stopped");
    QString m_VirtualIp;
    QString m_ExposeIp;
    QString m_AuthUrl;
    QString m_ErrorText;
    int m_PeersOnline = 0;
    bool m_HostExposed = false;

    bool m_AutoConnect = false;
    QString m_Hostname;
    QString m_TsControlUrl;
    QString m_TsAuthKey;
    bool m_TsUseWebOAuth = true;
    QString m_EtNetworkName;
    QString m_EtNetworkSecret;
    QString m_EtPeers = QStringLiteral("tcp://public.easytier.top:11010");
    QString m_EtVirtualIp;
    QString m_EtCoreBinary;

    bool m_WantHostExpose = false;
    quint16 m_HostHttpPort = 0;
    quint16 m_HostHttpsPort = 0;
};
