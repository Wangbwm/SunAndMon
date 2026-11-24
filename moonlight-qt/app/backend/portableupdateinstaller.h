#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class QFile;
class QNetworkReply;

class PortableUpdateInstaller : public QObject
{
    Q_OBJECT
public:
    explicit PortableUpdateInstaller(QObject *parent = nullptr);

    bool supportsInAppUpdate() const;
    void installUpdate(const QString& url, const QString& expectedDigest = QString());
    void installLocalArchive(const QString& localZipPath, const QString& expectedDigest = QString());

signals:
    void onPortableUpdateStatusChanged(QString message);
    void onPortableUpdateFailed(QString message);
    void prepareForRestart();

private slots:
    void handlePortableUpdateMetaDataChanged();
    void handlePortableUpdateDownloadReadyRead();
    void handlePortableUpdateDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void handlePortableUpdateDownloadFinished();

private:
    bool isPortableInstall() const;
    bool isBundleInstall() const;
    QString getInstalledBundlePath() const;
    QString getUpdateArchiveName() const;
    QString getUpdateArchiveSuffix() const;
    QString getUpdateStorageProbePath() const;
    QString getInstallRootDir() const;
    QString getPortableUpdaterExecutable() const;
    bool ensureWritableInstallDir(QString& errorMessage) const;
    bool isInstallDirWritable() const;
    bool isOwnedHostServiceRunning() const;
    QString createPortableUpdateWorkspace() const;
    QString materializePortableUpdateScript(const QString& workspace) const;
    bool launchUpdaterAndQuit(const QString& archivePath, QString& errorMessage);
    bool stageMacUpdateBundle(const QString& archivePath,
                              QString& stagedBundlePath,
                              QString& errorMessage);
    bool runTool(const QString& program, const QStringList& arguments, int timeoutMs) const;
    bool ensureSufficientDiskSpace(qint64 requiredBytes, QString& errorMessage) const;
    bool verifyUpdateArchive(const QString& archivePath, QString& errorMessage) const;
    qint64 estimateRequiredWorkspaceBytes(qint64 archiveBytes) const;
    void resetPortableUpdateState(bool removeWorkspace);

    QNetworkAccessManager* m_UpdateNam;
    QNetworkReply* m_UpdateReply;
    QFile* m_UpdateFile;
    QString m_PortableUpdateWorkspace;
    QString m_PortableUpdateError;
    QByteArray m_ExpectedSha256;
    bool m_MetadataChecked;
};
