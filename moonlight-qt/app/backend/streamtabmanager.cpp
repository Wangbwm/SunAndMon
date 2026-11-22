#include "streamtabmanager.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QtMath>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

StreamTabManager::StreamTabManager(QObject* parent)
    : QObject(parent)
{
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, &StreamTabManager::closeAllTabs);
}

StreamTabManager::~StreamTabManager()
{
    closeAllTabs();
}

bool StreamTabManager::embedSupported() const
{
#ifdef Q_OS_WIN32
    return true;
#elif defined(Q_OS_LINUX)
    return QGuiApplication::platformName() == QStringLiteral("xcb");
#else
    return false;
#endif
}

QVariantList StreamTabManager::tabs() const
{
    QVariantList out;
    out.reserve(m_Tabs.size());
    for (const auto& tab : m_Tabs) {
        out.append(QVariantMap{
            {QStringLiteral("id"), tab.id},
            {QStringLiteral("deviceId"), tab.deviceId},
            {QStringLiteral("title"), tab.title},
            {QStringLiteral("state"), tab.state},
            {QStringLiteral("statusText"), tab.statusText},
            {QStringLiteral("errorText"), tab.errorText},
            {QStringLiteral("routeType"), tab.routeType},
            {QStringLiteral("routeLabel"), tab.routeLabel},
            {QStringLiteral("embedded"), tab.embedded},
        });
    }
    return out;
}

QVariantMap StreamTabManager::activeTab() const
{
    const int idx = indexOfTab(m_ActiveTabId);
    if (idx < 0) {
        return {};
    }
    const auto& tab = m_Tabs.at(idx);
    return QVariantMap{
        {QStringLiteral("id"), tab.id},
        {QStringLiteral("deviceId"), tab.deviceId},
        {QStringLiteral("title"), tab.title},
        {QStringLiteral("state"), tab.state},
        {QStringLiteral("statusText"), tab.statusText},
        {QStringLiteral("errorText"), tab.errorText},
        {QStringLiteral("routeType"), tab.routeType},
        {QStringLiteral("routeLabel"), tab.routeLabel},
        {QStringLiteral("embedded"), tab.embedded},
    };
}

int StreamTabManager::indexOfTab(const QString& tabId) const
{
    if (tabId.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_Tabs.size(); ++i) {
        if (m_Tabs.at(i).id == tabId) {
            return i;
        }
    }
    return -1;
}

void StreamTabManager::bindHostWindow(QWindow* hostWindow)
{
    m_HostWindow = hostWindow;
}

QRect StreamTabManager::nativeViewportRect() const
{
    const qreal dpr = m_HostWindow ? qMax<qreal>(1.0, m_HostWindow->devicePixelRatio()) : 1.0;
    const int x = qRound(qMax<qreal>(0.0, m_LogicalX) * dpr);
    const int y = qRound(qMax<qreal>(0.0, m_LogicalY) * dpr);
#ifdef Q_OS_WIN32
    if (m_HostWindow && m_HostWindow->winId() != 0) {
        HWND parentHwnd = reinterpret_cast<HWND>(m_HostWindow->winId());
        RECT rc {};
        if (IsWindow(parentHwnd) && GetClientRect(parentHwnd, &rc)) {
            const int clientW = static_cast<int>(rc.right - rc.left);
            const int clientH = static_cast<int>(rc.bottom - rc.top);
            if (clientW > 0 && clientH > 0) {
                const int w = qMax(160, clientW - x);
                const int h = qMax(120, clientH - y);
                return QRect(x, y, w, h);
            }
        }
    }
#endif
    const int w = qMax(160, qRound(m_LogicalW * dpr));
    const int h = qMax(120, qRound(m_LogicalH * dpr));
    return QRect(x, y, w, h);
}

void StreamTabManager::updateViewport(qreal logicalX, qreal logicalY, qreal logicalW, qreal logicalH)
{
    m_LogicalX = logicalX;
    m_LogicalY = logicalY;
    m_LogicalW = logicalW;
    m_LogicalH = logicalH;

    const QRect nr = nativeViewportRect();
    if (nr == m_LastSentNativeRect) {
        return;
    }
    m_LastSentNativeRect = nr;

    const QByteArray cmd = QByteArray("RESIZE ")
        + QByteArray::number(nr.x()) + " "
        + QByteArray::number(nr.y()) + " "
        + QByteArray::number(nr.width()) + " "
        + QByteArray::number(nr.height()) + "\n";

    const int activeIdx = indexOfTab(m_ActiveTabId);
    if (activeIdx >= 0) {
        auto& tab = m_Tabs[activeIdx];
        if (tab.embedded && tab.embeddedReady) {
            sendCommand(tab, cmd);
        }
    }
}

void StreamTabManager::applyNativeHide(quintptr childWinId)
{
#ifdef Q_OS_WIN32
    if (childWinId != 0) {
        HWND hwnd = reinterpret_cast<HWND>(childWinId);
        if (IsWindow(hwnd)) {
            ShowWindowAsync(hwnd, SW_HIDE);
        }
    }
#else
    Q_UNUSED(childWinId);
#endif
}

void StreamTabManager::sendCommand(TabEntry& tab, const QByteArray& cmd)
{
    if (tab.proc && tab.proc->state() == QProcess::Running) {
        tab.proc->write(cmd);
    }
}

void StreamTabManager::openCenterTab(const QString& deviceId, const QString& deviceName)
{
    static const QRegularExpression idFormat(QStringLiteral("^[a-f0-9]{32}$"));
    if (!idFormat.match(deviceId).hasMatch()) {
        return;
    }

    // If a live tab for this device already exists, focus it instead of duplicating.
    for (int i = 0; i < m_Tabs.size(); ++i) {
        if (m_Tabs.at(i).deviceId == deviceId) {
            if (m_Tabs.at(i).state == QStringLiteral("error")) {
                closeTab(m_Tabs.at(i).id);
                break;
            }
            activateTab(m_Tabs.at(i).id);
            return;
        }
    }

    // Hide currently active embedded stream tab before switching to the new tab.
    const int prevIdx = indexOfTab(m_ActiveTabId);
    if (prevIdx >= 0 && m_Tabs[prevIdx].embedded) {
        applyNativeHide(m_Tabs[prevIdx].childWinId);
        sendCommand(m_Tabs[prevIdx], QByteArrayLiteral("HIDE\n"));
    }

    const QString tabId = QStringLiteral("tab-%1").arg(m_NextTabSeq++);
    const bool canEmbed = embedSupported() && m_HostWindow && m_HostWindow->winId() != 0;
    const QRect nr = nativeViewportRect();

    TabEntry entry;
    entry.id = tabId;
    entry.deviceId = deviceId;
    entry.title = deviceName.trimmed().isEmpty() ? deviceId.left(8) : deviceName.trimmed();
    entry.state = QStringLiteral("connecting");
    entry.statusText = tr("Connecting to %1…").arg(entry.title);
    entry.embedded = canEmbed;

    auto* proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::SeparateChannels);
    entry.proc = proc;

    QStringList args{
        QStringLiteral("--center-connect"), deviceId,
        QStringLiteral("--center-action"), QStringLiteral("desktop"),
    };
    if (canEmbed) {
        args << QStringLiteral("--embed-parent")
             << QString::number(static_cast<qulonglong>(m_HostWindow->winId()))
             << QStringLiteral("--embed-rect")
             << QStringLiteral("%1,%2,%3,%4").arg(nr.x()).arg(nr.y()).arg(nr.width()).arg(nr.height());
    }

    m_Tabs.append(entry);
    m_ActiveTabId = tabId;
    emit changed();

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, tabId, proc] {
        const int idx = indexOfTab(tabId);
        if (idx < 0) {
            return;
        }
        m_Tabs[idx].stdoutBuf += proc->readAllStandardOutput();
        while (true) {
            const int nl = m_Tabs[idx].stdoutBuf.indexOf('\n');
            if (nl < 0) {
                break;
            }
            const QByteArray line = m_Tabs[idx].stdoutBuf.left(nl).trimmed();
            m_Tabs[idx].stdoutBuf.remove(0, nl + 1);
            if (!line.isEmpty()) {
                handleChildLine(tabId, line);
                // handleChildLine may have removed/mutated m_Tabs; re-resolve idx
                if (indexOfTab(tabId) < 0) {
                    return;
                }
            }
        }
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, tabId, proc](int exitCode, QProcess::ExitStatus exitStatus) {
        proc->deleteLater();
        const int idx = indexOfTab(tabId);
        if (idx < 0) {
            return;
        }
        m_Tabs[idx].proc = nullptr;
        const bool wasUserClosing = m_Tabs[idx].closingByUser;
        const bool hadError = m_Tabs[idx].state == QStringLiteral("error");
        const bool wasStreaming = m_Tabs[idx].state == QStringLiteral("streaming");

        if (wasUserClosing || wasStreaming || (exitStatus == QProcess::NormalExit && exitCode == 0 && !hadError)) {
            const bool wasActive = (m_ActiveTabId == tabId);
            const quintptr hwnd = m_Tabs[idx].childWinId;
            applyNativeHide(hwnd);
            m_Tabs.removeAt(idx);
            if (wasActive) {
                if (m_FullscreenStream) {
                    setFullscreenStream(false);
                }
                if (!m_Tabs.isEmpty()) {
                    const int nextIdx = qMin(idx, m_Tabs.size() - 1);
                    m_ActiveTabId.clear();
                    activateTab(m_Tabs.at(nextIdx).id);
                    return;
                }
                m_ActiveTabId.clear();
            }
            emit changed();
            return;
        }

        if (!hadError) {
            m_Tabs[idx].state = QStringLiteral("error");
            m_Tabs[idx].errorText = tr("The stream session ended unexpectedly.");
        }
        m_Tabs[idx].embeddedReady = false;
        emit changed();
    });

    connect(proc, &QProcess::errorOccurred, this, [this, tabId](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            const int idx = indexOfTab(tabId);
            if (idx >= 0) {
                m_Tabs[idx].state = QStringLiteral("error");
                m_Tabs[idx].errorText = tr("Unable to start the stream process.");
                emit changed();
            }
        }
    });

    proc->start(QCoreApplication::applicationFilePath(), args);
}

void StreamTabManager::handleChildLine(const QString& tabId, const QByteArray& line)
{
    const int idx = indexOfTab(tabId);
    if (idx < 0) {
        return;
    }
    auto& tab = m_Tabs[idx];

    if (line.startsWith("TAB_HWND ")) {
        bool ok = false;
        const quintptr wid = static_cast<quintptr>(line.mid(9).trimmed().toULongLong(&ok));
        if (ok && wid != 0) {
            tab.childWinId = wid;
        }
        return;
    }
    if (line.startsWith("TAB_STAGE ")) {
        tab.statusText = QString::fromUtf8(line.mid(10).trimmed());
        emit changed();
        return;
    }
    if (line.startsWith("TAB_ROUTE ")) {
        const QByteArray payload = line.mid(10).trimmed();
        const int sp = payload.indexOf(' ');
        if (sp > 0) {
            tab.routeType = QString::fromUtf8(payload.left(sp)).trimmed();
            tab.routeLabel = QString::fromUtf8(payload.mid(sp + 1)).trimmed();
        } else {
            tab.routeType = QString::fromUtf8(payload);
            tab.routeLabel = tab.routeType;
        }
        emit changed();
        return;
    }
    if (line == "TAB_READY") {
        tab.state = QStringLiteral("streaming");
        tab.embeddedReady = true;
        tab.statusText.clear();
        tab.errorText.clear();
        const QRect nr = nativeViewportRect();
        m_LastSentNativeRect = nr;
        const QByteArray resizeCmd = QByteArray("RESIZE ")
            + QByteArray::number(nr.x()) + " "
            + QByteArray::number(nr.y()) + " "
            + QByteArray::number(nr.width()) + " "
            + QByteArray::number(nr.height()) + "\n";
        sendCommand(tab, resizeCmd);
        if (m_ActiveTabId == tabId) {
            sendCommand(tab, QByteArrayLiteral("SHOW\n"));
        } else {
            applyNativeHide(tab.childWinId);
            sendCommand(tab, QByteArrayLiteral("HIDE\n"));
        }
        emit changed();
        return;
    }
    if (line.startsWith("TAB_ERROR ")) {
        tab.state = QStringLiteral("error");
        tab.errorText = QString::fromUtf8(line.mid(10).trimmed());
        tab.embeddedReady = false;
        applyNativeHide(tab.childWinId);
        emit changed();
        return;
    }
    if (line == "TAB_TOGGLE_FULLSCREEN") {
        if (m_ActiveTabId == tabId) {
            setFullscreenStream(!m_FullscreenStream);
        }
        return;
    }
    if (line == "TAB_OPEN_DEVICES") {
        activateTab(QString());
        return;
    }
}

void StreamTabManager::activateTab(const QString& tabId)
{
    if (tabId == m_ActiveTabId) {
        // Re-assert visibility/focus if clicking an already active stream tab
        const int idx = indexOfTab(tabId);
        if (idx >= 0 && m_Tabs[idx].embedded && m_Tabs[idx].embeddedReady) {
            const QRect nr = nativeViewportRect();
            sendCommand(m_Tabs[idx], QByteArray("RESIZE ")
                + QByteArray::number(nr.x()) + " " + QByteArray::number(nr.y()) + " "
                + QByteArray::number(nr.width()) + " " + QByteArray::number(nr.height()) + "\n");
            sendCommand(m_Tabs[idx], QByteArrayLiteral("SHOW\n"));
        }
        return;
    }

    const int prevIdx = indexOfTab(m_ActiveTabId);
    if (prevIdx >= 0 && m_Tabs[prevIdx].embedded) {
        applyNativeHide(m_Tabs[prevIdx].childWinId);
        sendCommand(m_Tabs[prevIdx], QByteArrayLiteral("HIDE\n"));
    }

    if (tabId.isEmpty()) {
        if (m_FullscreenStream) {
            setFullscreenStream(false);
        }
        m_ActiveTabId.clear();
        emit changed();
        return;
    }

    const int nextIdx = indexOfTab(tabId);
    if (nextIdx < 0) {
        return;
    }
    m_ActiveTabId = tabId;
    emit changed();

    if (m_Tabs[nextIdx].embedded && m_Tabs[nextIdx].embeddedReady) {
        const QRect nr = nativeViewportRect();
        m_LastSentNativeRect = nr;
        sendCommand(m_Tabs[nextIdx], QByteArray("RESIZE ")
            + QByteArray::number(nr.x()) + " " + QByteArray::number(nr.y()) + " "
            + QByteArray::number(nr.width()) + " " + QByteArray::number(nr.height()) + "\n");
        sendCommand(m_Tabs[nextIdx], QByteArrayLiteral("SHOW\n"));
    }
}

void StreamTabManager::closeTab(const QString& tabId)
{
    const int idx = indexOfTab(tabId);
    if (idx < 0) {
        return;
    }

    const bool wasActive = (m_ActiveTabId == tabId);
    QProcess* proc = m_Tabs[idx].proc;
    const quintptr childHwnd = m_Tabs[idx].childWinId;
    m_Tabs[idx].closingByUser = true;
    m_Tabs[idx].proc = nullptr;

    applyNativeHide(childHwnd);
    if (proc && proc->state() != QProcess::NotRunning) {
        proc->write("QUIT\n");
        proc->closeWriteChannel();
        QPointer<QProcess> guard(proc);
        QTimer::singleShot(3000, this, [guard] {
            if (guard && guard->state() != QProcess::NotRunning) {
                guard->kill();
            }
        });
    } else if (proc) {
        proc->deleteLater();
    }

    m_Tabs.removeAt(idx);

    if (wasActive) {
        if (m_FullscreenStream) {
            setFullscreenStream(false);
        }
        if (!m_Tabs.isEmpty()) {
            const int nextIdx = qMin(idx, m_Tabs.size() - 1);
            m_ActiveTabId.clear();
            activateTab(m_Tabs.at(nextIdx).id);
            return;
        }
        m_ActiveTabId.clear();
    }
    emit changed();
}

void StreamTabManager::retryTab(const QString& tabId)
{
    const int idx = indexOfTab(tabId);
    if (idx < 0) {
        return;
    }
    const QString devId = m_Tabs.at(idx).deviceId;
    const QString title = m_Tabs.at(idx).title;
    closeTab(tabId);
    openCenterTab(devId, title);
}

void StreamTabManager::closeAllTabs()
{
    if (m_Tabs.isEmpty()) {
        return;
    }
    auto tabsCopy = m_Tabs;
    m_Tabs.clear();
    m_ActiveTabId.clear();
    m_FullscreenStream = false;

    for (auto& tab : tabsCopy) {
        applyNativeHide(tab.childWinId);
        if (tab.proc && tab.proc->state() != QProcess::NotRunning) {
            tab.proc->disconnect(this);
            tab.proc->write("QUIT\n");
            tab.proc->closeWriteChannel();
            if (!tab.proc->waitForFinished(1500)) {
                tab.proc->kill();
                tab.proc->waitForFinished(500);
            }
            tab.proc->deleteLater();
        } else if (tab.proc) {
            tab.proc->deleteLater();
        }
    }
    emit changed();
    emit fullscreenStreamChanged();
}

void StreamTabManager::setFullscreenStream(bool enabled)
{
    if (m_FullscreenStream != enabled) {
        m_FullscreenStream = enabled;
        m_LastSentNativeRect = QRect();
        emit fullscreenStreamChanged();
    }
}

void StreamTabManager::toggleActiveMenu()
{
    const int idx = indexOfTab(m_ActiveTabId);
    if (idx >= 0 && m_Tabs[idx].embedded && m_Tabs[idx].embeddedReady) {
        sendCommand(m_Tabs[idx], QByteArrayLiteral("TOGGLE_MENU\n"));
    }
}

void StreamTabManager::notifyHostWindowMoved()
{
    const int idx = indexOfTab(m_ActiveTabId);
    if (idx >= 0 && m_Tabs[idx].embedded && m_Tabs[idx].embeddedReady) {
        sendCommand(m_Tabs[idx], QByteArrayLiteral("SYNC_OVERLAY\n"));
    }
}

void StreamTabManager::notifyHostWindowFocusChanged(bool active)
{
    const QString tabId = m_ActiveTabId;
    if (tabId.isEmpty()) {
        return;
    }
    if (active) {
        QTimer::singleShot(15, this, [this, tabId] {
            if (m_ActiveTabId != tabId) {
                return;
            }
            const int idx = indexOfTab(tabId);
            if (idx >= 0 && m_Tabs[idx].embedded && m_Tabs[idx].embeddedReady) {
                sendCommand(m_Tabs[idx], QByteArrayLiteral("FOCUS\n"));
            }
        });
    } else {
        QTimer::singleShot(60, this, [this, tabId] {
            if (m_ActiveTabId != tabId) {
                return;
            }
            const int idx = indexOfTab(tabId);
            if (idx >= 0 && m_Tabs[idx].embedded && m_Tabs[idx].embeddedReady) {
                sendCommand(m_Tabs[idx], QByteArrayLiteral("SYNC_OVERLAY\n"));
            }
        });
    }
}
