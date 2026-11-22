#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>

class TunnelManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool easytierAvailable READ easytierAvailable NOTIFY changed)

    // Summary properties (compatible with single-badge views)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString summaryBadge READ summaryBadge NOTIFY changed)
    Q_PROPERTY(QString routePolicy READ routePolicy WRITE setRoutePolicy NOTIFY changed)
    Q_PROPERTY(QString hostname READ hostname NOTIFY changed)

    // Tailscale / Headscale independent state & config
    Q_PROPERTY(QString tsState READ tsState NOTIFY changed)
    Q_PROPERTY(QString tsVirtualIp READ tsVirtualIp NOTIFY changed)
    Q_PROPERTY(QString tsExposeIp READ tsExposeIp NOTIFY changed)
    Q_PROPERTY(QString tsAuthUrl READ tsAuthUrl NOTIFY changed)
    Q_PROPERTY(QString tsError READ tsError NOTIFY changed)
    Q_PROPERTY(int tsPeersOnline READ tsPeersOnline NOTIFY changed)
    Q_PROPERTY(bool tsAutoConnect READ tsAutoConnect NOTIFY changed)
    Q_PROPERTY(QString tsControlUrl READ tsControlUrl NOTIFY changed)
    Q_PROPERTY(QString tsAuthKey READ tsAuthKey NOTIFY changed)
    Q_PROPERTY(bool tsUseWebOAuth READ tsUseWebOAuth NOTIFY changed)

    // EasyTier independent state & config
    Q_PROPERTY(QString etState READ etState NOTIFY changed)
    Q_PROPERTY(QString etVirtualIp READ etVirtualIp NOTIFY changed)
    Q_PROPERTY(QString etExposeIp READ etExposeIp NOTIFY changed)
    Q_PROPERTY(QString etError READ etError NOTIFY changed)
    Q_PROPERTY(int etPeersOnline READ etPeersOnline NOTIFY changed)
    Q_PROPERTY(bool etAutoConnect READ etAutoConnect NOTIFY changed)
    Q_PROPERTY(QString etNetworkName READ etNetworkName NOTIFY changed)
    Q_PROPERTY(QString etNetworkSecret READ etNetworkSecret NOTIFY changed)
    Q_PROPERTY(QString etPeers READ etPeers NOTIFY changed)
    Q_PROPERTY(QString etStaticIp READ etStaticIp NOTIFY changed)
    Q_PROPERTY(QString etCoreBinary READ etCoreBinary NOTIFY changed)

public:
    explicit TunnelManager(QObject* parent = nullptr);
    ~TunnelManager() override;

    bool available() const;
    bool easytierAvailable() const { return m_EasyTierAvailable; }

    QString state() const;
    QString summaryBadge() const;
    QString routePolicy() const { return m_RoutePolicy; }
    void setRoutePolicy(const QString& policy);
    QString hostname() const { return m_Hostname; }

    QString tsState() const { return m_TsState; }
    QString tsVirtualIp() const { return m_TsVirtualIp; }
    QString tsExposeIp() const { return m_TsExposeIp; }
    QString tsAuthUrl() const { return m_TsAuthUrl; }
    QString tsError() const { return m_TsError; }
    int tsPeersOnline() const { return m_TsPeersOnline; }
    bool tsAutoConnect() const { return m_TsAutoConnect; }
    QString tsControlUrl() const { return m_TsControlUrl; }
    QString tsAuthKey() const { return m_TsAuthKey; }
    bool tsUseWebOAuth() const { return m_TsUseWebOAuth; }

    QString etState() const { return m_EtState; }
    QString etVirtualIp() const { return m_EtVirtualIp; }
    QString etExposeIp() const { return m_EtExposeIp; }
    QString etError() const { return m_EtError; }
    int etPeersOnline() const { return m_EtPeersOnline; }
    bool etAutoConnect() const { return m_EtAutoConnect; }
    QString etNetworkName() const { return m_EtNetworkName; }
    QString etNetworkSecret() const { return m_EtNetworkSecret; }
    QString etPeers() const { return m_EtPeers; }
    QString etStaticIp() const { return m_EtStaticIp; }
    QString etCoreBinary() const { return m_EtCoreBinary; }

    bool preferTunnelFirst() const;
    QStringList orderedTunnelProviders() const;
    QList<QPair<QString, QString>> connectedHostTunnels() const;
    bool isProviderConnected(const QString& provider) const;

    Q_INVOKABLE void startOnLaunch(bool secondaryInstance = false);
    Q_INVOKABLE void saveAndConnectTailscale(bool autoConnect,
                                             const QString& hostname,
                                             const QString& controlUrl,
                                             const QString& authKey,
                                             bool useWebOAuth);
    Q_INVOKABLE void stopTailscale();
    Q_INVOKABLE void saveAndConnectEasyTier(bool autoConnect,
                                            const QString& hostname,
                                            const QString& networkName,
                                            const QString& networkSecret,
                                            const QString& peers,
                                            const QString& staticIp,
                                            const QString& coreBinary);
    Q_INVOKABLE void stopEasyTier();
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void openTailscaleAuthUrl() const;
    Q_INVOKABLE void applyCenterDefaults(const QVariantMap& defaults);
    Q_INVOKABLE void refreshCapabilities();

    void syncHostExposure(bool hostReady, quint16 httpPort, quint16 httpsPort);
    void ensureForwardsAsync(const QList<QPair<QString, QString>>& endpoints,
                             quint16 httpPort,
                             quint16 httpsPort,
                             std::function<void(QList<QPair<QString, QString>> localForwards)> callback);

signals:
    void changed();
    void connectedStateChanged();
    void daemonAttachFinished();

private:
    QString daemonExecutable() const;
    QString tunnelStateDir() const;
    QString daemonEndpointFile() const;
    void writeDaemonEndpointFile() const;
    void removeDaemonEndpointFileIfOwned() const;
    void loadSettings();
    void saveSettings() const;
    void ensureDaemonRunning();
    void tryAttachExistingDaemon(bool spawnFallback);
    void spawnLocalDaemon();
    void stopDaemon();
    void sendStartTailscale();
    void sendStartEasyTier();
    void sendStopProvider(const QString& provider);
    void pollStatus();
    void applyStatusPayload(const QJsonObject& root);
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
    bool m_AttachedToExistingDaemon = false;
    bool m_ProbingExistingDaemon = false;
    bool m_SecondaryMode = false;
    int m_PollFailures = 0;
    bool m_PendingStartTs = false;
    bool m_PendingStartEt = false;

    bool m_EasyTierAvailable = false;
    QString m_RoutePolicy = QStringLiteral("auto");
    QString m_Hostname;

    QString m_TsState = QStringLiteral("stopped");
    QString m_TsVirtualIp;
    QString m_TsExposeIp;
    QString m_TsAuthUrl;
    QString m_TsError;
    int m_TsPeersOnline = 0;
    bool m_TsHostExposed = false;
    bool m_TsAutoConnect = false;
    QString m_TsControlUrl;
    QString m_TsAuthKey;
    bool m_TsUseWebOAuth = true;

    QString m_EtState = QStringLiteral("stopped");
    QString m_EtVirtualIp;
    QString m_EtExposeIp;
    QString m_EtError;
    int m_EtPeersOnline = 0;
    bool m_EtHostExposed = false;
    bool m_EtAutoConnect = false;
    QString m_EtNetworkName;
    QString m_EtNetworkSecret;
    QString m_EtPeers = QStringLiteral("tcp://public.easytier.top:11010");
    QString m_EtStaticIp;
    QString m_EtCoreBinary;

    bool m_WantHostExpose = false;
    quint16 m_HostHttpPort = 0;
    quint16 m_HostHttpsPort = 0;
};
