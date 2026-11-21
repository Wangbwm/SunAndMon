#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QWindow>
#include <memory>

class QMenu;
class QSystemTrayIcon;

class WindowsWindowChrome : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
    Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QQuickItem* titleBar READ titleBar WRITE setTitleBar NOTIFY titleBarChanged)
    Q_PROPERTY(bool maximized READ isMaximized NOTIFY maximizedChanged)
    Q_PROPERTY(bool minimizeToTray MEMBER m_MinimizeToTray)

public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using NativeEventResult = qintptr;
#else
    using NativeEventResult = long;
#endif

    explicit WindowsWindowChrome(QObject* parent = nullptr);
    ~WindowsWindowChrome() override;

    QWindow* window() const;
    void setWindow(QWindow* window);

    QQuickItem* titleBar() const;
    void setTitleBar(QQuickItem* titleBar);

    bool isMaximized() const;

    Q_INVOKABLE void activate();
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void toggleMaximized();
    Q_INVOKABLE void close();
    Q_INVOKABLE bool hideToTray();
    Q_INVOKABLE bool clientAreaAnimationsEnabled() const;

    bool nativeEventFilter(const QByteArray& eventType, void* message, NativeEventResult* result) override;

signals:
    void windowChanged();
    void titleBarChanged();
    void maximizedChanged();
    void exitRequested();

private:
    void postSystemCommand(unsigned int command, const QString& action);
    void logWindowState(const QString& action, const char* phase) const;
    void scheduleNativeStateUpdate();
    void restoreFromTray();

    QPointer<QWindow> m_Window;
    QPointer<QQuickItem> m_TitleBar;
    WId m_WindowId = 0;
    bool m_Installed = false;
    bool m_Maximized = false;
    bool m_StateUpdatePending = false;
    bool m_MinimizeToTray = false;
    QWindow::Visibility m_TrayRestoreVisibility = QWindow::Windowed;
    std::unique_ptr<QMenu> m_TrayMenu;
    std::unique_ptr<QSystemTrayIcon> m_TrayIcon;
};
