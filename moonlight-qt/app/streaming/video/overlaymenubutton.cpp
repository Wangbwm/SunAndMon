#include "overlaymenubutton.h"
#include "uifont.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QLocale>
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

    setOpacity(0.50);
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
    QSize targetSize;
    if (m_InputSource != InputSource::None) {
        targetSize = buttonSize();
        const QPoint parentTranslation = previousParentGeometry.isValid()
                ? m_ParentGeometry.topLeft() - previousParentGeometry.topLeft()
                : QPoint();
        newPosition = OverlayButtonPlacement::clamp(
                previousPosition + parentTranslation,
                m_ParentGeometry,
                targetSize,
                kMargin);
        m_DockEdge = detectDockEdge(newPosition, targetSize);
        m_WindowPositionAtPress += newPosition - previousPosition;
    }
    else if (m_NormalizedPosition) {
        // Resolve the remembered normalized anchor using a reference top-edge
        // size first to determine which screen edge it belongs to, then apply
        // the exact idle/expanded dimensions flush to that edge.
        const QSize probeSize(kIdleLength, kIdleThickness);
        const QPoint probePos = OverlayButtonPlacement::snapToEdge(
                OverlayButtonPlacement::resolve(*m_NormalizedPosition, m_ParentGeometry, probeSize, kMargin),
                m_ParentGeometry, probeSize, kMargin);
        m_DockEdge = detectDockEdge(probePos, probeSize);
        targetSize = buttonSize();
        newPosition = OverlayButtonPlacement::snapToEdge(
                OverlayButtonPlacement::resolve(*m_NormalizedPosition, m_ParentGeometry, targetSize, kMargin),
                m_ParentGeometry, targetSize, kMargin);
        m_NormalizedPosition = OverlayButtonPlacement::normalize(newPosition, m_ParentGeometry, targetSize, kMargin);
    }
    else {
        m_DockEdge = DockEdge::Top;
        targetSize = buttonSize();
        newPosition = OverlayButtonPlacement::defaultPosition(
                m_ParentGeometry, targetSize, kMargin);
    }

    setGeometry(QRect(clampToParent(newPosition), targetSize));
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
    m_Hovered = false;
    setOpacity(0.50);
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
        requestButtonUpdate();
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
        const QSize currentSize = buttonSize();
        const auto snapped = OverlayButtonPlacement::snapToEdge(position(), m_ParentGeometry, currentSize, kMargin);
        m_DockEdge = detectDockEdge(snapped, currentSize);
        setPosition(snapped);
        m_NormalizedPosition = OverlayButtonPlacement::normalize(
                snapped, m_ParentGeometry, currentSize, kMargin);
        if (!m_PositionStore.save(*m_NormalizedPosition)) {
            qWarning("Failed to save the device-local overlay button position");
        }
    }

    cancelInteraction();
    if (dragged) {
        requestButtonUpdate();
    }

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
        setOpacity(0.50);
        updateAnchoredGeometry(false);
        requestButtonUpdate();
    }

    if (m_Hovered) {
        setCursor(Qt::PointingHandCursor);
    }
    else {
        unsetCursor();
    }
}

void OverlayMenuButton::setCompact(bool compact)
{
    if (m_Compact != compact) {
        m_Compact = compact;
        setOpacity((m_Hovered || m_Dragging) ? 0.96 : 0.50);
    }
}

QString OverlayMenuButton::controlCenterLabel() const
{
    const QString lang = QLocale().name().toLower();
    if (lang.startsWith(QStringLiteral("zh"))) {
        return QStringLiteral("控制中心");
    }
    return tr("Control Center");
}

QSize OverlayMenuButton::idleSizeForEdge(DockEdge edge) const
{
    if (edge == DockEdge::Left || edge == DockEdge::Right) {
        return QSize(kIdleThickness, kSideIdleHeight);
    }
    return QSize(kIdleLength, kIdleThickness);
}

QSize OverlayMenuButton::expandedSizeForEdge(DockEdge edge) const
{
    QFont labelFont;
    labelFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    labelFont.setPixelSize(12);
    labelFont.setWeight(QFont::DemiBold);
    const int textW = QFontMetrics(labelFont).horizontalAdvance(controlCenterLabel());
    const int w = qMax(104, 34 + textW + 14);
    const int h = (edge == DockEdge::Left || edge == DockEdge::Right)
            ? kSideIdleHeight
            : kExpandedHeight;
    return QSize(w, h);
}

QSize OverlayMenuButton::buttonSize() const
{
    return (m_Hovered || m_Dragging)
            ? expandedSizeForEdge(m_DockEdge)
            : idleSizeForEdge(m_DockEdge);
}

OverlayMenuButton::DockEdge OverlayMenuButton::detectDockEdge(const QPoint& pos, const QSize& sz) const
{
    if (!m_ParentGeometry.isValid()) {
        return DockEdge::Top;
    }
    const int leftDist = qAbs(pos.x() - m_ParentGeometry.x());
    const int rightDist = qAbs((m_ParentGeometry.x() + m_ParentGeometry.width()) - (pos.x() + sz.width()));
    const int topDist = qAbs(pos.y() - m_ParentGeometry.y());
    const int bottomDist = qAbs((m_ParentGeometry.y() + m_ParentGeometry.height()) - (pos.y() + sz.height()));

    const int minHoriz = qMin(leftDist, rightDist);
    const int minVert = qMin(topDist, bottomDist);
    if (minVert <= minHoriz) {
        return topDist <= bottomDist ? DockEdge::Top : DockEdge::Bottom;
    }
    return leftDist <= rightDist ? DockEdge::Left : DockEdge::Right;
}

void OverlayMenuButton::updateAnchoredGeometry(bool expanded)
{
    if (!m_ParentGeometry.isValid()) {
        return;
    }
    const QRect cur = geometry();
    const QSize targetSize = expanded
            ? expandedSizeForEdge(m_DockEdge)
            : idleSizeForEdge(m_DockEdge);
    if (cur.size() == targetSize) {
        return;
    }

    QPoint newPos = cur.topLeft();
    switch (m_DockEdge) {
    case DockEdge::Top:
        newPos.setX(cur.center().x() - targetSize.width() / 2);
        newPos.setY(m_ParentGeometry.y());
        break;
    case DockEdge::Bottom:
        newPos.setX(cur.center().x() - targetSize.width() / 2);
        newPos.setY(m_ParentGeometry.y() + m_ParentGeometry.height() - targetSize.height());
        break;
    case DockEdge::Left:
        newPos.setX(m_ParentGeometry.x());
        newPos.setY(cur.center().y() - targetSize.height() / 2);
        break;
    case DockEdge::Right:
        newPos.setX(m_ParentGeometry.x() + m_ParentGeometry.width() - targetSize.width());
        newPos.setY(cur.center().y() - targetSize.height() / 2);
        break;
    }

    const QPoint clamped = OverlayButtonPlacement::clamp(newPos, m_ParentGeometry, targetSize, kMargin);
    setGeometry(QRect(clamped, targetSize));
}

void OverlayMenuButton::paintOverlay(QPainter& painter)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(0, 0, width(), height(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const bool expanded = m_Hovered || m_Dragging;
    const qreal w = static_cast<qreal>(width());
    const qreal h = static_cast<qreal>(height());
    const qreal r = m_Dragging ? 11.0 : (expanded ? 10.0 : 7.0);

    // Build an edge-docked glass path (flat against the docked screen border,
    // softly rounded on the interior corners) or a floating pill while dragging.
    QPainterPath path;
    if (m_Dragging) {
        path.addRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), r, r);
    }
    else if (m_DockEdge == DockEdge::Top) {
        const qreal x0 = 0.5, y0 = 0.0, x1 = w - 0.5, y1 = h - 0.5;
        path.moveTo(x0, y0);
        path.lineTo(x1, y0);
        path.lineTo(x1, y1 - r);
        path.quadTo(x1, y1, x1 - r, y1);
        path.lineTo(x0 + r, y1);
        path.quadTo(x0, y1, x0, y1 - r);
        path.closeSubpath();
    }
    else if (m_DockEdge == DockEdge::Bottom) {
        const qreal x0 = 0.5, y0 = 0.5, x1 = w - 0.5, y1 = h;
        path.moveTo(x0, y1);
        path.lineTo(x0, y0 + r);
        path.quadTo(x0, y0, x0 + r, y0);
        path.lineTo(x1 - r, y0);
        path.quadTo(x1, y0, x1, y0 + r);
        path.lineTo(x1, y1);
        path.closeSubpath();
    }
    else if (m_DockEdge == DockEdge::Left) {
        const qreal x0 = 0.0, y0 = 0.5, x1 = w - 0.5, y1 = h - 0.5;
        path.moveTo(x0, y0);
        path.lineTo(x1 - r, y0);
        path.quadTo(x1, y0, x1, y0 + r);
        path.lineTo(x1, y1 - r);
        path.quadTo(x1, y1, x1 - r, y1);
        path.lineTo(x0, y1);
        path.closeSubpath();
    }
    else { // Right
        const qreal x0 = 0.5, y0 = 0.5, x1 = w, y1 = h - 0.5;
        path.moveTo(x1, y0);
        path.lineTo(x0 + r, y0);
        path.quadTo(x0, y0, x0, y0 + r);
        path.lineTo(x0, y1 - r);
        path.quadTo(x0, y1, x0 + r, y1);
        path.lineTo(x1, y1);
        path.closeSubpath();
    }

    QLinearGradient fill(0, 0, 0, h);
    if (expanded) {
        fill.setColorAt(0.0, QColor(27, 45, 71, 242));
        fill.setColorAt(1.0, QColor(15, 25, 40, 246));
    }
    else {
        fill.setColorAt(0.0, QColor(16, 27, 44, 215));
        fill.setColorAt(1.0, QColor(11, 19, 32, 225));
    }

    painter.setBrush(fill);
    painter.setPen(QPen(expanded ? QColor(149, 191, 255, 135) : QColor(149, 191, 255, 85), 1.0));
    painter.drawPath(path);

    if (!expanded) {
        // Minimalist edge grab pill when idle
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(210, 230, 255, 200));
        if (m_DockEdge == DockEdge::Left || m_DockEdge == DockEdge::Right) {
            const qreal pillW = 3.0;
            const qreal pillH = 16.0;
            painter.drawRoundedRect(QRectF((w - pillW) / 2.0, (h - pillH) / 2.0, pillW, pillH), 1.5, 1.5);
        }
        else {
            const qreal pillW = 28.0;
            const qreal pillH = 3.0;
            painter.drawRoundedRect(QRectF((w - pillW) / 2.0, (h - pillH) / 2.0, pillW, pillH), 1.5, 1.5);
        }
        return;
    }

    // Expanded glass capsule: refined slider icon + "控制中心" label
    const qreal cy = h / 2.0;
    const qreal iconCx = 18.0;
    painter.setPen(QPen(QColor("#DCEBFF"), 1.4, Qt::SolidLine, Qt::RoundCap));
    const qreal rows[3] = { -4.0, 0.0, 4.0 };
    const qreal knobs[3] = { -2.5, 2.5, -1.0 };
    for (int i = 0; i < 3; ++i) {
        const qreal ry = cy + rows[i];
        painter.drawLine(QPointF(iconCx - 6.0, ry), QPointF(iconCx + 6.0, ry));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#95BFFF"));
    for (int i = 0; i < 3; ++i) {
        painter.drawEllipse(QPointF(iconCx + knobs[i], cy + rows[i]), 2.0, 2.0);
    }

    QFont labelFont;
    labelFont.setFamilies(UiFont::familyChain(QStringLiteral("Manrope")));
    labelFont.setPixelSize(12);
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);
    painter.setPen(QColor("#EFF5FF"));
    painter.drawText(QRectF(31.0, 0.0, w - 41.0, h),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     controlCenterLabel());
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
        setOpacity(0.96);
        updateAnchoredGeometry(true);
        requestButtonUpdate();
    }

    if (m_InputSource == InputSource::None) {
        setCursor(Qt::PointingHandCursor);
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
    if (ev->type() == QEvent::Enter) {
        if (!m_Hovered) {
            m_Hovered = true;
            setOpacity(0.96);
            updateAnchoredGeometry(true);
            requestButtonUpdate();
        }
    }
    else if (ev->type() == QEvent::Leave) {
        if (!m_Dragging) {
            m_Hovered = false;
            setOpacity(0.50);
            updateAnchoredGeometry(false);
            requestButtonUpdate();
        }
    }
    else if (ev->type() == QEvent::UngrabMouse && m_InputSource == InputSource::Mouse) {
        cancelInteraction();
    }
    return QRasterWindow::event(ev);
}
