#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QVersionNumber>

class QNetworkReply;
class PortableUpdateInstaller;

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool updateAvailable READ isUpdateAvailable NOTIFY updateStateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateDownloadUrl READ updateDownloadUrl NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateInProgress READ isUpdateInProgress NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateStatusMessage READ updateStatusMessage NOTIFY updateStateChanged)
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();
    Q_INVOKABLE bool supportsUpdateCheck() const;
    // Returns true only when a new check was started.
    Q_INVOKABLE bool checkForUpdates();
    Q_INVOKABLE bool supportsInAppUpdate() const;
    Q_INVOKABLE void installUpdate(QString url);
    Q_INVOKABLE void installAvailableUpdate();
    Q_INVOKABLE void installLocalUpdate(QString zipFilePath);

    bool isUpdateAvailable() const { return m_UpdateAvailable; }
    QString latestVersion() const { return m_LatestVersion; }
    QString updateDownloadUrl() const { return m_UpdateDownloadUrl; }
    bool isUpdateInProgress() const { return m_UpdateInProgress; }
    QString updateStatusMessage() const { return m_UpdateStatusMessage; }

signals:
    void onUpdateCheckStarted();
    void onUpdateCheckFinished(bool updateAvailable);
    void onUpdateCheckFailed();
    void onUpdateAvailable(QString newVersion, QString url);
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);
    void prepareForRestart();
    void updateStateChanged();

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);
    void handleChecksumsReplyFinished();

private:
    bool isPortableInstall() const;
    QString getExpectedAssetPrefix() const;
    QString getExpectedAssetSuffix() const;
    QString getPreferredAssetSuffix() const;
    QString getCurrentBuildArch() const;
    QString applyCustomDownloadProxy(const QString& url) const;
    void emitDiscoveredUpdate(const QString& tagName, const QString& downloadUrl, const QString& assetDigest);
    static QString extractSha256FromChecksums(const QString& text, const QString& assetName);

    QVersionNumber m_CurrentVersion;
    QNetworkAccessManager* m_Nam;
    PortableUpdateInstaller* m_PortableUpdateInstaller;
    QNetworkReply* m_ReleaseReply = nullptr;
    QNetworkReply* m_ChecksumsReply = nullptr;
    bool m_UpdateCheckInProgress = false;
    bool m_UpdateAvailable = false;
    bool m_UpdateInProgress = false;
    QString m_LatestVersion;
    QString m_UpdateDownloadUrl;
    QString m_UpdateAssetDigest;
    QString m_PendingTagName;
    QString m_PendingDownloadUrl;
    QString m_PendingAssetName;
    QString m_UpdateStatusMessage;
};
