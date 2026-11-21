#include "overlaymenubutton.h"

#include <QGuiApplication>
#include <QScreen>
#include <QPainterPath>
#include <QStyleHints>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QInputDevice>
#endif

#ifdef Q_OS_DARWIN
#include "overlayeventmonitor_mac.h"
#endif

#ifdef HAVE_LINUX_DISPLAY_EVENT_MONITOR
#include "overlayeventmonitor_linux.h"
#endif

#ifdef Q_OS_WIN32
#include <windows.h>
#include <commctrl.h>
#endif

namespace {
QPoint interactionPosition(QMouseEvent* event, const QWindow* window, OverlayWindowMode windowMode)
{
    if (OverlayWindowPolicy::usesParentCoordinates(windowMode)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return window->position() + event->position().toPoint();
#else
        return window->position() + event->pos();
#endif
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

bool mouseEventComesFromTouch(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->pointingDevice() &&
           event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen;
#else
    return event->source() != Qt::MouseEventNotSynthesized;
#endif
}
}

#ifdef Q_OS_WIN32
struct OverlayMenuButton::NativeEventMonitor
{
    using SetWindowSubclassFn = BOOL (WINAPI *)(HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR);
    using RemoveWindowSubclassFn = BOOL (WINAPI *)(HWND, SUBCLASSPROC, UINT_PTR);
    using DefSubclassProcFn = LRESULT (WINAPI *)(HWND, UINT, WPARAM, LPARAM);

    struct SubclassApis
    {
        SubclassApis()
        {
            module = LoadLibraryW(L"comctl32.dll");
            if (!module) {
                return;
            }
            setWindowSubclass = reinterpret_cast<SetWindowSubclassFn>(
                    GetProcAddress(module, "SetWindowSubclass"));
            removeWindowSubclass = reinterpret_cast<RemoveWindowSubclassFn>(
                    GetProcAddress(module, "RemoveWindowSubclass"));
            defSubclassProc = reinterpret_cast<DefSubclassProcFn>(
                    GetProcAddress(module, "DefSubclassProc"));
        }

        bool available() const
        {
            return setWindowSubclass && removeWindowSubclass && defSubclassProc;
        }

        HMODULE module = nullptr;
        SetWindowSubclassFn setWindowSubclass = nullptr;
        RemoveWindowSubclassFn removeWindowSubclass = nullptr;
        DefSubclassProcFn defSubclassProc = nullptr;
    };

    explicit NativeEventMonitor(OverlayMenuButton* owner) : owner(owner) {}

    ~NativeEventMonitor()
    {
        detach();
    }

    static SubclassApis& apis()
    {
        static SubclassApis value;
        return value;
    }

    static bool shouldWakeForMessage(UINT message)
    {
        if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
                (message >= 0x0241 && message <= 0x0253)) {
            return true;
        }

        switch (message) {
        case WM_MOUSEHOVER:
        case WM_MOUSELEAVE:
        case WM_NCMOUSEMOVE:
        case WM_NCMOUSEHOVER:
        case WM_NCMOUSELEAVE:
        case WM_CAPTURECHANGED:
        case WM_TOUCH:
        case WM_GESTURE:
        case WM_GESTURENOTIFY:
        case WM_PAINT:
        case WM_SHOWWINDOW:
        case WM_WINDOWPOSCHANGED:
        case WM_DPICHANGED:
            return true;
        default:
            return false;
        }
    }

    static LRESULT CALLBACK subclassProc(HWND hwnd, UINT message,
                                         WPARAM wParam, LPARAM lParam,
                                         UINT_PTR, DWORD_PTR refData)
    {
        auto* monitor = reinterpret_cast<NativeEventMonitor*>(refData);
        if (monitor && monitor->owner && shouldWakeForMessage(message)) {
            monitor->owner->requestEventProcessing();
        }

        if (message == WM_NCDESTROY && monitor) {
            monitor->hwnd = nullptr;
        }

        return apis().defSubclassProc(hwnd, message, wParam, lParam);
    }

    bool attach(HWND newHwnd)
    {
        if (hwnd == newHwnd && hwnd != nullptr) {
            return true;
        }

        detach();
        if (!newHwnd || !apis().available()) {
            return false;
        }

        if (!apis().setWindowSubclass(newHwnd, subclassProc, kSubclassId,
                                      reinterpret_cast<DWORD_PTR>(this))) {
            return false;
        }

        hwnd = newHwnd;
        return true;
    }

    void detach()
    {
        if (hwnd && apis().available()) {
            apis().removeWindowSubclass(hwnd, subclassProc, kSubclassId);
        }
        hwnd = nullptr;
    }

    bool isAttached() const { return hwnd != nullptr; }

    static constexpr UINT_PTR kSubclassId = 0x4D4C4F42; // "MLOB"
    OverlayMenuButton* owner;
    HWND hwnd = nullptr;
};
#endif

OverlayMenuButton::OverlayMenuButton(QWindow* parent, OverlayWindowMode windowMode)
    : OverlayRasterWindow(parent, windowMode), m_Hovered(false), m_ButtonVisible(false),
      m_Dragging(false), m_InputSource(InputSource::None), m_TouchPointId(-1),
      m_NormalizedPosition(m_PositionStore.load()), m_WindowMode(windowMode)
{
    Q_ASSERT(m_WindowMode != OverlayWindowMode::WaylandSubsurface || parent != nullptr);
    setFlags(OverlayWindowPolicy::flags(m_WindowMode));
    setTitle(tr("Stream controls"));

    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    setFormat(fmt);

    setOpacity(m_Compact ? 0.40 : 0.86);
}

OverlayMenuButton::~OverlayMenuButton()
{
#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || \
        defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
    m_NativeEventMonitor.reset();
#endif
}

void OverlayMenuButton::setEventWakeCallback(EventWakeCallback cb)
{
    std::lock_guard<std::mutex> lock(m_EventWakeCallbackMutex);
    m_EventWakeCallback = std::move(cb);
}

bool OverlayMenuButton::needsEventProcessing() const
{
    if (!m_ButtonVisible.load(std::memory_order_acquire)) {
        return false;
    }

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || \
        defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
    // Keep pumping throughout a gesture even if the native monitor misses the
    // next motion/release. Only an idle button can rely on native wakeups.
    // If native monitoring is unavailable, retain continuous processing.
    return m_InputSource != InputSource::None || !m_NativeEventMonitor ||
           !m_NativeEventMonitor->isAttached() || m_EventWakeState.isPending();
#else
    return true;
#endif
}

void OverlayMenuButton::beginEventProcessing()
{
    m_EventWakeState.take();
}

void OverlayMenuButton::finishEventProcessing()
{
#ifdef HAVE_LINUX_DISPLAY_EVENT_MONITOR
    if (m_NativeEventMonitor) {
        m_NativeEventMonitor->finishEventProcessing();
    }
#endif
}

void OverlayMenuButton::requestEventProcessing()
{
    if (!m_ButtonVisible.load(std::memory_order_acquire)) {
        return;
    }

    if (m_EventWakeState.request()) {
        std::lock_guard<std::mutex> lock(m_EventWakeCallbackMutex);
        if (m_ButtonVisible.load(std::memory_order_acquire) && m_EventWakeCallback) {
            m_EventWakeCallback();
        }
    }
}

void OverlayMenuButton::requestButtonUpdate()
{
    requestEventProcessing();
    requestUpdate();
}

#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || \
        defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
void OverlayMenuButton::ensureNativeEventMonitor()
{
    if (!m_NativeEventMonitor) {
#if defined(Q_OS_WIN32)
        m_NativeEventMonitor = std::make_unique<NativeEventMonitor>(this);
#elif defined(Q_OS_DARWIN)
        m_NativeEventMonitor = std::make_unique<MacOverlayEventMonitor>([this]() {
            requestEventProcessing();
        });
#elif defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
        m_NativeEventMonitor = std::make_unique<LinuxDisplayEventMonitor>([this]() {
            requestEventProcessing();
        });
#endif
    }

#ifdef Q_OS_WIN32
    const auto nativeWindow = reinterpret_cast<HWND>(winId());
#elif defined(Q_OS_DARWIN)
    void* nativeWindow = reinterpret_cast<void*>(winId());
#elif defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
    const auto nativeWindow = static_cast<std::uintptr_t>(winId());
#endif
    if (!m_NativeEventMonitor->attach(nativeWindow)) {
        qWarning("Failed to monitor native overlay button events; using continuous Qt event processing");
    }
}

void OverlayMenuButton::detachNativeEventMonitor()
{
    if (m_NativeEventMonitor) {
        m_NativeEventMonitor->detach();
    }
}
#endif

void OverlayMenuButton::repositionTo(int parentX, int parentY, int parentW, int parentH)
{
    const QRect newParentGeometry(parentX, parentY, parentW, parentH);
    const QRect previousParentGeometry = m_ParentGeometry;
    const QPoint previousPosition = position();
    m_ParentGeometry = newParentGeometry;

    QPoint newPosition;
    if (m_InputSource != InputSource::None) {
        const QPoint parentTranslation = previousParentGeometry.isValid()
                ? m_ParentGeometry.topLeft() - previousParentGeometry.topLeft()
                : QPoint();
        newPosition = OverlayButtonPlacement::clamp(
                previousPosition + parentTranslation,
                m_ParentGeometry,
                buttonSize(),
                kMargin);
        // Keep the active drag origin aligned with any parent movement or
        // resize so the next pointer event cannot jump back to stale bounds.
        m_WindowPositionAtPress += newPosition - previousPosition;
    }
    else if (m_NormalizedPosition) {
        newPosition = OverlayButtonPlacement::snapToEdge(
                OverlayButtonPlacement::resolve(*m_NormalizedPosition, m_ParentGeometry, buttonSize(), kMargin),
                m_ParentGeometry, buttonSize(), kMargin);
        m_NormalizedPosition = OverlayButtonPlacement::normalize(newPosition, m_ParentGeometry, buttonSize(), kMargin);
    }
    else {
        newPosition = OverlayButtonPlacement::defaultPosition(
                m_ParentGeometry, buttonSize(), kMargin);
    }

    setGeometry(QRect(clampToParent(newPosition), buttonSize()));
    if (m_ButtonVisible.load(std::memory_order_acquire)) {
        requestEventProcessing();
    }
}

void OverlayMenuButton::showButton(int parentX, int parentY, int parentW, int parentH)
{
    repositionTo(parentX, parentY, parentW, parentH);
    m_ButtonVisible.store(true, std::memory_order_release);
    show();
#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || \
        defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
    ensureNativeEventMonitor();
#endif
    raise();
    requestButtonUpdate();
}

void OverlayMenuButton::hideButton()
{
    cancelInteraction();
    m_ButtonVisible.store(false, std::memory_order_release);
#if defined(Q_OS_WIN32) || defined(Q_OS_DARWIN) || \
        defined(HAVE_LINUX_DISPLAY_EVENT_MONITOR)
    detachNativeEventMonitor();
#endif
    m_EventWakeState.clear();
    unsetCursor();
    hide();
}

bool OverlayMenuButton::handleExternalMousePress(const QPoint& globalPosition)
{
    if (!m_ButtonVisible.load(std::memory_order_acquire) || !geometry().contains(globalPosition)) {
        return false;
    }

    if (m_InputSource == InputSource::None) {
        beginInteraction(InputSource::Mouse, globalPosition);
    }
    return m_InputSource == InputSource::Mouse;
}

bool OverlayMenuButton::handleExternalMouseMove(const QPoint& globalPosition, bool leftButtonDown)
{
    if (m_InputSource != InputSource::Mouse) {
        return false;
    }

    if (leftButtonDown) {
        updateInteraction(globalPosition);
    } else {
        cancelInteraction();
    }
    return true;
}

bool OverlayMenuButton::handleExternalMouseRelease(const QPoint& globalPosition)
{
    if (m_InputSource != InputSource::Mouse) {
        return false;
    }

    finishInteraction(globalPosition);
    return true;
}

bool OverlayMenuButton::handleExternalTouchPress(qint64 touchId, const QPoint& globalPosition)
{
    if (!m_ButtonVisible.load(std::memory_order_acquire) || !geometry().contains(globalPosition) ||
        m_InputSource != InputSource::None) {
        return false;
    }

    m_ExternalTouchId = touchId;
    beginInteraction(InputSource::Touch, globalPosition);
    return true;
}

bool OverlayMenuButton::handleExternalTouchMove(qint64 touchId, const QPoint& globalPosition)
{
    if (m_InputSource != InputSource::Touch || m_ExternalTouchId != touchId) {
        return false;
    }

    updateInteraction(globalPosition);
    return true;
}

bool OverlayMenuButton::handleExternalTouchRelease(qint64 touchId, const QPoint& globalPosition)
{
    if (m_InputSource != InputSource::Touch || m_ExternalTouchId != touchId) {
        return false;
    }

    finishInteraction(globalPosition);
    return true;
}

QPoint OverlayMenuButton::clampToParent(const QPoint& position) const
{
    return OverlayButtonPlacement::clamp(
            position, m_ParentGeometry, buttonSize(), kMargin);
}

void OverlayMenuButton::beginInteraction(InputSource source, const QPoint& globalPosition)
{
    if (m_InputSource != InputSource::None) {
        return;
    }

    m_InputSource = source;
    m_Dragging = false;
    m_PressGlobalPosition = globalPosition;
    m_WindowPositionAtPress = position();
}

void OverlayMenuButton::updateInteraction(const QPoint& globalPosition)
{
    if (m_InputSource == InputSource::None) {
        return;
    }

    const QPoint delta = globalPosition - m_PressGlobalPosition;
    if (!m_Dragging &&
            delta.manhattanLength() >= QGuiApplication::styleHints()->startDragDistance()) {
        m_Dragging = true;
        setCursor(Qt::ClosedHandCursor);
    }

    if (m_Dragging) {
        const QPoint targetPosition = clampToParent(m_WindowPositionAtPress + delta);
        if (targetPosition != position()) {
            setPosition(targetPosition);
        }
    }
}

void OverlayMenuButton::finishInteraction(const QPoint& globalPosition)
{
    if (m_InputSource == InputSource::None) {
        return;
    }

    const InputSource source = m_InputSource;
    const bool dragged = m_Dragging;
    const bool activate = !m_Dragging && geometry().contains(globalPosition);

    if (dragged && m_ParentGeometry.isValid()) {
        const auto snapped = OverlayButtonPlacement::snapToEdge(position(), m_ParentGeometry, buttonSize(), kMargin);
        setPosition(snapped);
        m_NormalizedPosition = OverlayButtonPlacement::normalize(
                snapped, m_ParentGeometry, buttonSize(), kMargin);
        if (!m_PositionStore.save(*m_NormalizedPosition)) {
            qWarning("Failed to save the device-local overlay button position");
        }
    }

    cancelInteraction();

    if (activate && m_ClickCallback) {
        // Touch input has no reliable QCursor position, so it must not use
        // mouse-hover auto-close checks after opening the menu.
        m_ClickCallback(globalPosition, source == InputSource::Mouse);
    }
}

void OverlayMenuButton::cancelInteraction()
{
    const bool wasTouch = m_InputSource == InputSource::Touch;
    m_InputSource = InputSource::None;
    m_TouchPointId = -1;
    m_ExternalTouchId = -1;
    m_Dragging = false;

    if (wasTouch) {
        // Touch input has no hover state after the finger is released.
        m_Hovered = false;
        setOpacity(m_Compact ? 0.40 : 0.86);
        requestButtonUpdate();
    }

    if (m_Hovered) {
        setCursor(Qt::OpenHandCursor);
    }
    else {
        unsetCursor();
    }
}

void OverlayMenuButton::setCompact(bool compact)
{
    if (m_Compact != compact) {
        m_Compact = compact;
        setOpacity(m_Hovered ? 1.0 : m_Compact ? 0.40 : 0.86);
    }
}

QSize OverlayMenuButton::buttonSize() const
{
    QFont labelFont;
    labelFont.setPixelSize(13);
    labelFont.setWeight(QFont::DemiBold);
    return QSize(m_Compact ? kButtonSize
                           : 46 + QFontMetrics(labelFont).horizontalAdvance(tr("Stream controls")),
                 kButtonSize);
}

void OverlayMenuButton::paintOverlay(QPainter& painter)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(0, 0, width(), height(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const QRectF surface = QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1);
    painter.setBrush(m_Hovered ? QColor("#243c58") : QColor("#162438"));
    painter.setPen(QPen(QColor("#597999"), 1));
    painter.drawRoundedRect(surface, 9, 9);

    const QPointF icon(kButtonSize / 2.0, height() / 2.0);
    QColor ink("#e8f2ff");
    painter.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap));
    for (const qreal offset : {-5.0, 0.0, 5.0}) {
        painter.drawLine(icon + QPointF(-7, offset), icon + QPointF(7, offset));
        painter.drawLine(icon + QPointF(offset / 2, offset - 1.5), icon + QPointF(offset / 2, offset + 1.5));
    }
    if (!m_Compact) {
        QFont labelFont;
        labelFont.setPixelSize(13);
        labelFont.setWeight(QFont::DemiBold);
        painter.setFont(labelFont);
        painter.drawText(QRect(kButtonSize, 0, width() - kButtonSize - 10, height()),
                         Qt::AlignLeft | Qt::AlignVCenter, tr("Stream controls"));
    }
}

void OverlayMenuButton::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_InputSource == InputSource::None) {
        beginInteraction(mouseEventComesFromTouch(event) ? InputSource::Touch : InputSource::Mouse,
                         interactionPosition(event, this, m_WindowMode));
        event->accept();
    }
}

void OverlayMenuButton::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_Hovered) {
        m_Hovered = true;
        setOpacity(0.95);
        requestButtonUpdate();
    }

    if (m_InputSource == InputSource::None) {
        setCursor(Qt::OpenHandCursor);
        return;
    }

    if (!(event->buttons() & Qt::LeftButton)) {
        // A cancelled touch sequence may not produce a synthesized mouse
        // release. Clear the state as soon as the button mask says it ended.
        cancelInteraction();
        return;
    }

    updateInteraction(interactionPosition(event, this, m_WindowMode));
    event->accept();
}

void OverlayMenuButton::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || m_InputSource == InputSource::None) {
        return;
    }

    finishInteraction(interactionPosition(event, this, m_WindowMode));
    event->accept();
}

void OverlayMenuButton::touchEvent(QTouchEvent* event)
{
    if (event->type() == QEvent::TouchCancel) {
        cancelInteraction();
        event->accept();
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto points = event->points();
    if (points.isEmpty()) {
        if (event->type() == QEvent::TouchEnd && m_InputSource == InputSource::Touch) {
            cancelInteraction();
        }
        event->accept();
        return;
    }

    if (event->type() == QEvent::TouchBegin && m_InputSource == InputSource::None) {
        m_TouchPointId = points.first().id();
        const QPoint position = OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)
                                    ? this->position() + points.first().position().toPoint()
                                    : points.first().globalPosition().toPoint();
        beginInteraction(InputSource::Touch, position);
    }

    bool foundTrackedPoint = false;
    for (const auto& point : points) {
        if (point.id() != m_TouchPointId) {
            continue;
        }

        foundTrackedPoint = true;
        const QPoint globalPosition = OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)
                                          ? position() + point.position().toPoint()
                                          : point.globalPosition().toPoint();
        if (point.state() == QEventPoint::Released) {
            finishInteraction(globalPosition);
        }
        else if (event->type() == QEvent::TouchUpdate) {
            updateInteraction(globalPosition);
        }
        else if (event->type() == QEvent::TouchEnd) {
            finishInteraction(globalPosition);
        }
        break;
    }
    if (event->type() == QEvent::TouchEnd &&
            !foundTrackedPoint && m_InputSource == InputSource::Touch) {
        cancelInteraction();
    }
#else
    const auto points = event->touchPoints();
    if (points.isEmpty()) {
        if (event->type() == QEvent::TouchEnd && m_InputSource == InputSource::Touch) {
            cancelInteraction();
        }
        event->accept();
        return;
    }

    if (event->type() == QEvent::TouchBegin && m_InputSource == InputSource::None) {
        m_TouchPointId = points.first().id();
        const QPoint position = OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)
                                    ? this->position() + points.first().pos().toPoint()
                                    : points.first().screenPos().toPoint();
        beginInteraction(InputSource::Touch, position);
    }

    bool foundTrackedPoint = false;
    for (const auto& point : points) {
        if (point.id() != m_TouchPointId) {
            continue;
        }

        foundTrackedPoint = true;
        const QPoint globalPosition = OverlayWindowPolicy::usesParentCoordinates(m_WindowMode)
                                          ? position() + point.pos().toPoint()
                                          : point.screenPos().toPoint();
        if (point.state() == Qt::TouchPointReleased) {
            finishInteraction(globalPosition);
        }
        else if (event->type() == QEvent::TouchUpdate) {
            updateInteraction(globalPosition);
        }
        else if (event->type() == QEvent::TouchEnd) {
            finishInteraction(globalPosition);
        }
        break;
    }
    if (event->type() == QEvent::TouchEnd &&
            !foundTrackedPoint && m_InputSource == InputSource::Touch) {
        cancelInteraction();
    }
#endif

    // Accepting the native touch sequence prevents Qt from also synthesizing
    // a duplicate mouse interaction for the same finger.
    event->accept();
}

bool OverlayMenuButton::event(QEvent* ev)
{
    if (ev->type() == QEvent::Leave) {
        m_Hovered = false;
        setOpacity(m_Compact ? 0.40 : 0.86);
        requestButtonUpdate();
    }
    else if (ev->type() == QEvent::UngrabMouse && m_InputSource == InputSource::Mouse) {
        cancelInteraction();
    }
    return QRasterWindow::event(ev);
}
