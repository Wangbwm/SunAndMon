#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QVariantList>
#include <QWindow>

class StreamTabManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY changed)
    Q_PROPERTY(QString activeTabId READ activeTabId NOTIFY changed)
    Q_PROPERTY(bool hasTabs READ hasTabs NOTIFY changed)
    Q_PROPERTY(bool streamTabActive READ streamTabActive NOTIFY changed)
    Q_PROPERTY(QVariantMap activeTab READ activeTab NOTIFY changed)
    Q_PROPERTY(bool fullscreenStream READ fullscreenStream WRITE setFullscreenStream NOTIFY fullscreenStreamChanged)
    Q_PROPERTY(bool embedSupported READ embedSupported CONSTANT)

public:
    explicit StreamTabManager(QObject* parent = nullptr);
    ~StreamTabManager() override;

    QVariantList tabs() const;
    QString activeTabId() const { return m_ActiveTabId; }
    bool hasTabs() const { return !m_Tabs.isEmpty(); }
    bool streamTabActive() const { return !m_ActiveTabId.isEmpty(); }
    QVariantMap activeTab() const;
    bool fullscreenStream() const { return m_FullscreenStream; }
    bool embedSupported() const;

    Q_INVOKABLE void bindHostWindow(QWindow* hostWindow);
    Q_INVOKABLE void updateViewport(qreal logicalX, qreal logicalY, qreal logicalW, qreal logicalH);
    Q_INVOKABLE void openCenterTab(const QString& deviceId, const QString& deviceName);
    Q_INVOKABLE void activateTab(const QString& tabId);
    Q_INVOKABLE void closeTab(const QString& tabId);
    Q_INVOKABLE void retryTab(const QString& tabId);
    Q_INVOKABLE void closeAllTabs();
    Q_INVOKABLE void setFullscreenStream(bool enabled);
    Q_INVOKABLE void toggleActiveMenu();
    Q_INVOKABLE void notifyHostWindowMoved();
    Q_INVOKABLE void notifyHostWindowFocusChanged(bool active);

signals:
    void changed();
    void fullscreenStreamChanged();

private:
    struct TabEntry {
        QString id;
        QString deviceId;
        QString title;
        QString state; // connecting | streaming | error
        QString statusText;
        QString errorText;
        QString routeType;  // lan | easytier | tailscale | direct | tunnel
        QString routeLabel; // localized display label
        QProcess* proc = nullptr;
        QByteArray stdoutBuf;
        quintptr childWinId = 0;
        bool embedded = false;
        bool embeddedReady = false;
        bool closingByUser = false;
    };

    int indexOfTab(const QString& tabId) const;
    QRect nativeViewportRect() const;
    void sendCommand(TabEntry& tab, const QByteArray& cmd);
    void applyNativeHide(quintptr childWinId);
    void handleChildLine(const QString& tabId, const QByteArray& line);

    QPointer<QWindow> m_HostWindow;
    QList<TabEntry> m_Tabs;
    QString m_ActiveTabId;
    qreal m_LogicalX = 0;
    qreal m_LogicalY = 40;
    qreal m_LogicalW = 1280;
    qreal m_LogicalH = 600;
    QRect m_LastSentNativeRect;
    bool m_FullscreenStream = false;
    int m_NextTabSeq = 1;
};
