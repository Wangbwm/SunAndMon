#include "clipboardsync.h"
#include "clipboardlogging.h"
#include "backend/pairedcertificate.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QNetworkProxy>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QThread>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <cmath>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef Q_OS_MACOS
extern "C" int ClipboardHelperPasteboardChangeCount();
extern "C" bool ClipboardHelperWritePasteboardCompound(const char* utf8Text,
                                                       const unsigned char* pngBytes,
                                                       int pngLength);
extern "C" bool ClipboardHelperIsFinderPasteboard();
#endif

namespace {
bool isImageLikeMimeFormat(const QString& format);
bool isFileReferenceFormat(const QString& format);
bool shouldCopyClipboardFormat(const QString& format);
}

// Releases a clipboard snapshot that was deep-copied on the GUI thread by
// ClipboardBridge::cloneClipboard(). deleteLater() is safe from any thread:
// the deferred delete is posted to the object's own (GUI) event loop.
namespace {

struct OwnedClipboardSnapshot
{
    QMimeData* data = nullptr;

    ~OwnedClipboardSnapshot()
    {
        if (data != nullptr) {
            data->deleteLater();
        }
    }
};

}

namespace {

const QString kFileClipboardMime = QStringLiteral("application/x-sunandmon-files");
constexpr int kMaxClipboardFiles = 256;
constexpr quint32 kMaxClipboardMetadataBytes = 262144;

#ifdef Q_OS_WIN
std::unique_ptr<QMimeData> readWindowsClipboard(bool fileClipboard, bool& retry)
{
    if (!OpenClipboard(nullptr)) {
        retry = true;
        return {};
    }
    const auto close = qScopeGuard([] { CloseClipboard(); });
    auto snapshot = std::make_unique<QMimeData>();
    if (fileClipboard && IsClipboardFormatAvailable(CF_HDROP)) {
        const auto files = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (!files) {
            retry = true;
            return {};
        }
        const auto count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
        if (count == 0 || count > static_cast<UINT>(kMaxClipboardFiles)) {
            return snapshot;
        }
        QList<QUrl> urls;
        for (UINT index = 0; index < count; ++index) {
            const auto length = DragQueryFileW(files, index, nullptr, 0);
            if (length == 0 || length >= 32768) {
                return snapshot;
            }
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(files, index, path.data(), length + 1) != length) {
                retry = true;
                return {};
            }
            urls.append(QUrl::fromLocalFile(QString::fromWCharArray(path.data(), length)));
        }
        snapshot->setUrls(urls);
        return snapshot;
    }

    static const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5) ||
        IsClipboardFormatAvailable(CF_BITMAP) || (pngFormat && IsClipboardFormatAvailable(pngFormat)) ||
        IsClipboardFormatAvailable(CF_HDROP)) {
        return {}; // Let Qt read image and compound formats without the Win32 lock.
    }
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        const auto handle = GetClipboardData(CF_UNICODETEXT);
        const auto* text = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
        if (!text) {
            retry = true;
            return {};
        }
        const auto unlock = qScopeGuard([handle] { GlobalUnlock(handle); });
        const auto length = GlobalSize(handle) / sizeof(wchar_t);
        if (const auto* end = std::wmemchr(text, L'\0', length)) {
            snapshot->setText(QString::fromWCharArray(text, end - text));
        }
        return snapshot;
    }
    return {}; // Preserve Qt's fallback for other clipboard formats.
}
#endif

bool validClipboardFilename(const QString& name)
{
    static const QRegularExpression invalid(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    static const QRegularExpression reserved(
        QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])(?:\.|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return !name.isEmpty() && name.size() <= 240 && name == name.trimmed() &&
           !name.endsWith('.') && !invalid.match(name).hasMatch() && !reserved.match(name).hasMatch();
}

bool inspectClipboardFiles(const QList<QUrl>& urls,
                           qint64 limit,
                           QByteArray& outHeaderAndMeta,
                           QStringList& outFilePaths,
                           QVector<qint64>& outFileSizes,
                           qint64& outTotalBytes)
{
    outHeaderAndMeta.clear();
    outFilePaths.clear();
    outFileSizes.clear();
    outTotalBytes = 0;

    if (urls.isEmpty() || urls.size() > kMaxClipboardFiles) {
        return false;
    }
    QJsonArray entries;
    QSet<QString> names;
    qint64 totalDataBytes = 0;
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            return false;
        }
        const QFileInfo info(url.toLocalFile());
        const auto name = info.fileName();
        const auto size = info.size();
        const auto key = name.toCaseFolded();
        if (!info.isFile() || info.isSymLink() || !validClipboardFilename(name) ||
            names.contains(key) || size < 0 || size > limit - totalDataBytes) {
            return false;
        }
        entries.append(QJsonObject{{"name", name}, {"size", size}});
        outFilePaths.append(info.filePath());
        outFileSizes.append(size);
        totalDataBytes += size;
        names.insert(key);
    }
    const auto metadata = QJsonDocument(entries).toJson(QJsonDocument::Compact);
    if (metadata.size() > static_cast<qsizetype>(kMaxClipboardMetadataBytes) ||
        4 + static_cast<qint64>(metadata.size()) > limit - totalDataBytes) {
        return false;
    }
    outHeaderAndMeta.resize(4);
    qToBigEndian<quint32>(static_cast<quint32>(metadata.size()), outHeaderAndMeta.data());
    outHeaderAndMeta.append(metadata);
    outTotalBytes = static_cast<qint64>(outHeaderAndMeta.size()) + totalDataBytes;
    return true;
}

QByteArray encodeClipboardFiles(const QList<QUrl>& urls, qint64 limit)
{
    QByteArray headerAndMeta;
    QStringList filePaths;
    QVector<qint64> fileSizes;
    qint64 totalBytes = 0;
    if (!inspectClipboardFiles(urls, limit, headerAndMeta, filePaths, fileSizes, totalBytes)) {
        return {};
    }
    QByteArray result;
    result.reserve(static_cast<qsizetype>(totalBytes));
    result.append(headerAndMeta);
    for (int i = 0; i < filePaths.size(); ++i) {
        QFile file(filePaths[i]);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        const auto size = fileSizes[i];
        const auto data = file.read(size + 1);
        if (data.size() != size || file.error() != QFileDevice::NoError) {
            return {};
        }
        result.append(data);
    }
    return result;
}

QString clipboardTransferRoot()
{
    const QDir applicationDir(QCoreApplication::applicationDirPath());
    if (applicationDir.exists("portable.dat")) {
        return applicationDir.filePath("cache/clipboard-files");
    }
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return cache.isEmpty() ? QString() : QDir(cache).filePath("received-files");
}

void pruneOldClipboardTransfers(const QString& transferRoot, const QString& keepPath)
{
    const auto older = QDir(transferRoot).entryInfoList({"paste-*"},
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time);
    for (qsizetype index = 8; index < older.size(); ++index) {
        if (older[index].absoluteFilePath() != keepPath) {
            QDir(older[index].absoluteFilePath()).removeRecursively();
        }
    }
}

QList<QUrl> extractClipboardFiles(const QByteArray& payload)
{
    if (payload.size() < 4) {
        return {};
    }
    const auto metadataSize = qFromBigEndian<quint32>(payload.constData());
    if (metadataSize > kMaxClipboardMetadataBytes || metadataSize > payload.size() - 4) {
        return {};
    }
    const auto entries = QJsonDocument::fromJson(payload.mid(4, metadataSize)).array();
    if (entries.isEmpty() || entries.size() > kMaxClipboardFiles) {
        return {};
    }
    const auto transferRoot = clipboardTransferRoot();
    if (transferRoot.isEmpty() || !QDir().mkpath(transferRoot)) {
        return {};
    }
    QTemporaryDir transfer(QDir(transferRoot).filePath("paste-XXXXXX"));
    if (!transfer.isValid()) {
        return {};
    }
    QList<QUrl> urls;
    QSet<QString> names;
    qsizetype offset = 4 + metadataSize;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto name = entry.value("name").toString();
        const auto size = entry.value("size").toInteger(-1);
        const auto key = name.toCaseFolded();
        if (!validClipboardFilename(name) || names.contains(key) || size < 0 ||
            size > payload.size() - offset) {
            return {};
        }
        QFile file(transfer.filePath(name));
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            file.write(payload.constData() + offset, size) != size || !file.flush()) {
            return {};
        }
        urls.append(QUrl::fromLocalFile(file.fileName()));
        offset += size;
        names.insert(key);
    }
    if (offset != payload.size()) {
        return {};
    }
    transfer.setAutoRemove(false);
    pruneOldClipboardTransfers(transferRoot, transfer.path());
    return urls;
}

bool isImageLikeMimeFormat(const QString& format)
{
    if (format.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive)
            || format.compare(QStringLiteral("application/x-qt-image"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    // Chromium/Edge/Firefox register Windows clipboard formats like
    // "PNG" / "image/png" / "DeviceIndependentBitmap" directly; Qt
    // surfaces those mangled as application/x-qt-windows-mime;value="...".
    // Recognize the common image-bearing variants so we still treat them
    // as candidates for extractClipboardPng().
    if (format.startsWith(QStringLiteral("application/x-qt-windows-mime;value=\""),
                          Qt::CaseInsensitive)) {
        // Extract inner value name.
        const int prefixLen = QStringLiteral("application/x-qt-windows-mime;value=\"").size();
        const int endQuote = format.indexOf(QLatin1Char('"'), prefixLen);
        if (endQuote > prefixLen) {
            const QString inner = format.mid(prefixLen, endQuote - prefixLen);
            if (inner.compare(QStringLiteral("PNG"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/jpeg"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/bmp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/webp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmap"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmapV5"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("JFIF"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("GIF"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    return false;
}

bool isFileReferenceFormat(const QString& format)
{
    // Some native and promised-file providers expose only a platform format,
    // without a URL that QMimeData can normalize. Match the identifying part
    // so this also covers Qt's application/x-qt-*-mime wrappers.
    static const QStringList fileFormatMarkers{
        QStringLiteral("public.file-url"),     QStringLiteral("NSFilenamesPboardType"),
        QStringLiteral("promised-file"),       QStringLiteral("FileNameW"),
        QStringLiteral("FileGroupDescriptor"), QStringLiteral("FileContents"),
        QStringLiteral("Shell IDList Array")
    };

    for (const QString& marker : fileFormatMarkers) {
        if (format.contains(marker, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool shouldCopyClipboardFormat(const QString& format)
{
    const QString lowerFormat = format.toLower();
    return lowerFormat.startsWith(QStringLiteral("text/plain")) ||
           lowerFormat.startsWith(QStringLiteral("text/html")) ||
           lowerFormat == QStringLiteral("text/uri-list") || isImageLikeMimeFormat(format) ||
           isFileReferenceFormat(format);
}

// PNG ::= 89 50 4E 47 0D 0A 1A 0A (RFC 2083 §3.1). Some Windows apps
// (older Office, IM clients) advertise "image/png" on the clipboard but
// stuff a DIB/BMP payload inside; loadFromData still decodes those by
// sniffing the actual magic, so we cannot trust the mime name alone.
// Only treat the bytes as wire-ready PNG when the magic matches.
bool looksLikePngBytes(const QByteArray& bytes)
{
    static const unsigned char kMagic[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return bytes.size() >= 8
            && memcmp(bytes.constData(), kMagic, sizeof(kMagic)) == 0;
}
}

ClipboardSync::ClipboardSync(const ClipboardSyncHostContext& hostContext, QObject* parent)
    : QObject(parent),
      m_HostContext(hostContext)
{
    qRegisterMetaType<QByteArray>("QByteArray");
#ifndef Q_OS_MACOS
    // The helper constructs the engine on its GUI thread (before
    // moveToThread), so the bridge inherits GUI-thread affinity and owns
    // every QClipboard access on platforms that require it.
    m_Bridge = new ClipboardBridge();
#endif
    m_Clock.start();
    // Tokens are shared with peers. Avoid every helper restart beginning at 1.
    m_NextToken = QRandomGenerator::global()->generate();
    if (m_NextToken == 0) {
        m_NextToken = 1;
    }
}

ClipboardSync::~ClipboardSync()
{
    // During normal helper shutdown the GUI event loop has already returned
    // before the worker is joined. Do not block this worker waiting for a
    // GUI queued call that can no longer be dispatched.
    stop(false);
#ifndef Q_OS_MACOS
    // The bridge lives on the helper GUI thread; hand its deletion back to
    // that thread. If the GUI loop is already gone (process exit) this is
    // a harmless no-op.
    if (m_Bridge != nullptr) {
        m_Bridge->deleteLater();
        m_Bridge = nullptr;
    }
#endif
}

#ifndef Q_OS_MACOS
ClipboardBridge::ClipboardBridge(QObject* parent) : QObject(parent) {}

void ClipboardBridge::attachForwarding()
{
    if (m_Forwarding) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        ClipboardLog::warn("ClipboardBridge: no QClipboard available; change forwarding disabled");
        return;
    }

    connect(cb, &QClipboard::dataChanged, this, &ClipboardBridge::clipboardDataChanged);
    m_Forwarding = true;
}

void ClipboardBridge::detachForwarding()
{
    if (!m_Forwarding) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb != nullptr) {
        disconnect(cb, &QClipboard::dataChanged, this, &ClipboardBridge::clipboardDataChanged);
    }
    m_Forwarding = false;
}

QMimeData* ClipboardBridge::cloneClipboard()
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return nullptr;
    }

    const QMimeData* original = cb->mimeData();
    if (original == nullptr) {
        return nullptr;
    }

    // Deep-copy the clipboard flavors consumed by the worker so it can
    // inspect a stable read-only snapshot without touching the live
    // clipboard. Keep provider-backed formats out unless the worker has an
    // extraction path that consumes them; reading every advertised format
    // can synchronously render virtual files or large custom payloads on the
    // GUI thread.
    QMimeData* clone = new QMimeData();
    const QStringList formats = original->formats();
    for (const QString& format : formats) {
        if (!shouldCopyClipboardFormat(format)) {
            continue;
        }

        // File-reference markers are needed by name only. Avoid asking a
        // native provider to render their data while preserving the format
        // in the detached snapshot for hasFileReferences().
        if (isFileReferenceFormat(format)) {
            clone->setData(format, QByteArray());
        } else {
            clone->setData(format, original->data(format));
        }
    }
    if (original->hasUrls()) {
        clone->setUrls(original->urls());
    }
    if (original->hasImage()) {
        clone->setImageData(original->imageData());
    }
    return clone;
}

bool ClipboardBridge::hasFileReferences()
{
    QClipboard* cb = QGuiApplication::clipboard();
    return cb != nullptr && ClipboardSync::hasFileReferences(cb->mimeData());
}

static bool clipboardWriteSucceeded(QClipboard* clipboard)
{
#ifdef Q_OS_WIN
    return QGuiApplication::platformName() != "windows" || clipboard->ownsClipboard();
#else
    Q_UNUSED(clipboard)
    return true;
#endif
}

bool ClipboardBridge::writeClipboard(const QString& text, const QImage& image,
                                     const QByteArray& pngBytes, bool preserveFiles)
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return false;
    }

    // The earlier worker-side guard can race with a user copying a file while
    // the decoded inbound image is waiting for this GUI-thread invocation.
    // Recheck immediately before replacing the native clipboard.
    if (preserveFiles && ClipboardSync::hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug(
            "ClipboardSync: preserving local file clipboard; inbound write ignored");
        return true;
    }

    if (image.isNull()) {
        cb->setText(text);
        return clipboardWriteSucceeded(cb);
    }

    QMimeData* mime = new QMimeData();
    if (!text.isEmpty()) {
        mime->setText(text);
    }
    mime->setImageData(image);
    // Provide raw PNG bytes too so apps that prefer image/png over CF_DIB
    // (browsers, modern image editors) get the lossless copy verbatim.
    // Intentionally NOT setting HTML: pasting a giant base64 data: URI into
    // CF_HTML adds nothing useful (Office takes the bitmap path anyway) and
    // some Windows clipboard hooks reject oversize CF_HTML, dropping the
    // entire mime data set on the floor.
    if (!pngBytes.isEmpty()) {
        mime->setData(QStringLiteral("image/png"), pngBytes);
    }
    cb->setMimeData(mime);
    return clipboardWriteSucceeded(cb);
}

bool ClipboardSync::clipboardBridgeNeeded() const
{
    return m_Bridge != nullptr && QThread::currentThread() != m_Bridge->thread();
}

QMimeData* ClipboardSync::clipboardBridgeClone()
{
    if (m_Bridge == nullptr) {
        return nullptr;
    }

    QMimeData* snapshot = nullptr;
    QMetaObject::invokeMethod(
        m_Bridge,
        [bridge = QPointer<ClipboardBridge>(m_Bridge)]() -> QMimeData* {
            return bridge != nullptr ? bridge->cloneClipboard() : nullptr;
        },
        Qt::BlockingQueuedConnection, &snapshot);
    return snapshot;
}

bool ClipboardSync::clipboardBridgeFileGuard(const char* context)
{
    if (m_Bridge == nullptr || m_HostContext.fileClipboard) {
        return false;
    }

    bool fileRefs = false;
    QMetaObject::invokeMethod(
        m_Bridge,
        [bridge = QPointer<ClipboardBridge>(m_Bridge), &fileRefs]() {
            if (bridge != nullptr) {
                fileRefs = bridge->hasFileReferences();
            }
        },
        clipboardBridgeNeeded() ? Qt::BlockingQueuedConnection : Qt::DirectConnection);
    if (fileRefs) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; %s ignored", context);
    }
    return fileRefs;
}

void ClipboardSync::clipboardBridgeWrite(const QString& text, const QImage& image,
                                         const QByteArray& pngBytes, int retriesRemaining)
{
    if (m_Bridge == nullptr) {
        ClipboardLog::warn("ClipboardSync: no clipboard bridge; clipboard write skipped");
        return;
    }

    bool ok = false;
#ifdef Q_OS_WIN
    const bool native = m_WindowsClipboardPoll && m_WindowsClipboardPoll->isActive();
    const auto sequence = m_WindowsClipboardSequence;
    auto sequenceAfterWrite = sequence;
#else
    Q_UNUSED(retriesRemaining)
#endif
    QMetaObject::invokeMethod(
        m_Bridge,
        [&, bridge = QPointer<ClipboardBridge>(m_Bridge)] {
#ifdef Q_OS_WIN
            if (native && sequence != GetClipboardSequenceNumber()) {
                return;
            }
#endif
            if (bridge != nullptr) {
                ok = bridge->writeClipboard(text, image, pngBytes, !m_HostContext.fileClipboard);
            }
#ifdef Q_OS_WIN
            if (ok && native) {
                sequenceAfterWrite = GetClipboardSequenceNumber();
            }
#endif
        },
        clipboardBridgeNeeded() ? Qt::BlockingQueuedConnection : Qt::DirectConnection);
#ifdef Q_OS_WIN
    if (native && sequence != sequenceAfterWrite) {
        m_WindowsClipboardSequence = sequenceAfterWrite;
    }
#endif
    if (!ok) {
#ifdef Q_OS_WIN
        if (native && sequence != GetClipboardSequenceNumber()) {
            return;
        }
        if (native && retriesRemaining > 0) {
            const auto revision = m_Revision;
            QTimer::singleShot(100, this, [this, text, image, pngBytes, sequence, revision, retriesRemaining] {
                if (m_Active && revision == m_Revision && sequence == GetClipboardSequenceNumber()) {
                    clipboardBridgeWrite(text, image, pngBytes, retriesRemaining - 1);
                }
            });
            return;
        }
#endif
        ClipboardLog::warn("ClipboardSync: GUI-thread clipboard write failed");
    }
}
#endif // !Q_OS_MACOS

bool ClipboardSync::hasFileReferences(const QMimeData* mime)
{
    if (mime == nullptr) {
        return false;
    }

    // Qt normalizes ordinary Finder and Explorer file copies to local URLs.
    // Do not treat remote web URLs as file clipboard data: browsers commonly
    // attach those to otherwise valid copied images.
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                return true;
            }
        }
    }

    const QStringList formats = mime->formats();
    for (const QString& format : formats) {
        if (isFileReferenceFormat(format)) {
            return true;
        }
    }

    return false;
}

void ClipboardSync::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    beginClipboardChange();
    m_RetiredTokens.clear();
    m_HaveClipboardSnapshot = false;
    if (m_HostContext.address == hostContext.address &&
        m_HostContext.httpsPort == hostContext.httpsPort &&
        m_HostContext.serverCertificate == hostContext.serverCertificate &&
        m_HostContext.clientCertificate == hostContext.clientCertificate &&
        m_HostContext.clientPrivateKey == hostContext.clientPrivateKey &&
        m_HostContext.managedHostToken == hostContext.managedHostToken &&
        m_HostContext.fileClipboard == hostContext.fileClipboard) {
        return;
    }
    // Pooled TLS connections must not survive a change of paired identity.
    delete m_Nam;
    m_Nam = nullptr;
    m_HostContext = hostContext;
}

void ClipboardSync::start()
{
    if (m_Active) {
        return;
    }

#ifdef Q_OS_MACOS
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        ClipboardLog::warn("ClipboardSync: no QClipboard available; sync disabled");
        return;
    }

    m_LastPasteboardChangeCount = ClipboardHelperPasteboardChangeCount();
    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onMacClipboardDataChanged,
            Qt::UniqueConnection);
    if (m_PasteboardPollTimer == nullptr) {
        m_PasteboardPollTimer = new QTimer(this);
        connect(m_PasteboardPollTimer, &QTimer::timeout,
                this, &ClipboardSync::pollPasteboardChangeCount);
    }
    m_PasteboardPollTimer->start(500);
#else
    if (clipboardBridgeNeeded()) {
        // Attach change forwarding on the GUI thread, then listen from the
        // engine thread; the bridge signal is queued back here.
        QMetaObject::invokeMethod(
            m_Bridge,
            [bridge = QPointer<ClipboardBridge>(m_Bridge)]() {
                if (bridge != nullptr) {
                    bridge->attachForwarding();
                }
            },
            Qt::BlockingQueuedConnection);
        connect(m_Bridge, &ClipboardBridge::clipboardDataChanged, this,
                &ClipboardSync::onLocalClipboardChanged, Qt::UniqueConnection);
    } else {
        QClipboard* cb = QGuiApplication::clipboard();
        if (cb == nullptr) {
            ClipboardLog::warn("ClipboardSync: no QClipboard available; sync disabled");
            return;
        }
        auto connection = Qt::UniqueConnection;
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == "windows") {
            connection = static_cast<Qt::ConnectionType>(Qt::QueuedConnection | Qt::UniqueConnection);
        }
#endif
        connect(cb, &QClipboard::dataChanged, this, &ClipboardSync::onLocalClipboardChanged, connection);
    }
#endif

#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows") {
        if (!m_WindowsClipboardPoll) {
            m_WindowsClipboardPoll = new QTimer(this);
            connect(m_WindowsClipboardPoll, &QTimer::timeout, this, &ClipboardSync::onLocalClipboardChanged);
        }
        m_WindowsClipboardSequence = GetClipboardSequenceNumber();
        m_WindowsClipboardPoll->start(125);
    }
#endif
    m_Active = true;
    ClipboardLog::info(m_HostContext.fileClipboard
        ? "ClipboardSync: started (text, PNG and files, bidirectional)"
        : "ClipboardSync: started (text + PNG, bidirectional)");
}

void ClipboardSync::stop(bool waitForBridge)
{
    if (!m_Active) {
        return;
    }

#ifdef Q_OS_WIN
    if (m_WindowsClipboardPoll) {
        m_WindowsClipboardPoll->stop();
    }
#endif
#ifdef Q_OS_MACOS
    Q_UNUSED(waitForBridge);
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb != nullptr) {
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onMacClipboardDataChanged);
    }
#else
    if (m_Bridge != nullptr) {
        disconnect(m_Bridge, &ClipboardBridge::clipboardDataChanged, this,
                   &ClipboardSync::onLocalClipboardChanged);
        if (clipboardBridgeNeeded() && waitForBridge) {
            QMetaObject::invokeMethod(
                m_Bridge,
                [bridge = QPointer<ClipboardBridge>(m_Bridge)]() {
                    if (bridge != nullptr) {
                        bridge->detachForwarding();
                    }
                },
                Qt::BlockingQueuedConnection);
        } else if (clipboardBridgeNeeded()) {
            // Destruction can happen after app.exec() has stopped dispatching
            // GUI events. Queue the detach for the normal case, but let the
            // process teardown path continue when the event loop is gone.
            QMetaObject::invokeMethod(
                m_Bridge,
                [bridge = QPointer<ClipboardBridge>(m_Bridge)]() {
                    if (bridge != nullptr) {
                        bridge->detachForwarding();
                    }
                },
                Qt::QueuedConnection);
        } else if (auto* cb = QGuiApplication::clipboard()) {
            disconnect(cb, &QClipboard::dataChanged, this, &ClipboardSync::onLocalClipboardChanged);
        }
    }
#endif

    m_Active = false;
    beginClipboardChange();
    m_RetiredTokens.clear();
    m_HaveClipboardSnapshot = false;
#ifdef Q_OS_MACOS
    if (m_PasteboardPollTimer != nullptr) {
        m_PasteboardPollTimer->stop();
    }
    m_LastPasteboardChangeCount = -1;
#endif

    ClipboardLog::info("ClipboardSync: stopped");
}

void ClipboardSync::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length < 10 || length > MAX_PAYLOAD + 10) {
        return;
    }

    // Copy out of the recv-thread buffer and marshal to the engine thread.
    QByteArray frame(data, length);
    QMetaObject::invokeMethod(this, "onIncomingFrame", Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame));
}

#ifdef Q_OS_MACOS
void ClipboardSync::onMacClipboardDataChanged()
{
    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount >= 0) {
        m_LastPasteboardChangeCount = changeCount;
    }

    onLocalClipboardChanged();
}

void ClipboardSync::pollPasteboardChangeCount()
{
    if (!m_Active) {
        return;
    }

    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount < 0) {
        return;
    }

    if (m_LastPasteboardChangeCount < 0) {
        m_LastPasteboardChangeCount = changeCount;
        return;
    }

    if (changeCount == m_LastPasteboardChangeCount) {
        return;
    }

    m_LastPasteboardChangeCount = changeCount;
    onLocalClipboardChanged();
}
#endif

void ClipboardSync::onIncomingFrame(QByteArray frame)
{
    if (!m_Active) {
        return;
    }

    uint8_t kind = 0;
    quint32 token = 0;
    QByteArray payload;
    if (!decodeFrame(frame, kind, token, payload)) {
        ClipboardLog::warn("ClipboardSync: discarding malformed inbound frame (%d bytes)",
                    static_cast<int>(frame.size()));
        return;
    }

    if (kind != KIND_TEXT && kind != KIND_PNG && kind != KIND_REF) {
        return;
    }
    QString id, mime;
    qint64 size = 0;
    if (kind == KIND_REF) {
        // Payload is a small UTF-8 JSON descriptor: {"id":"...","mime":"...","size":N}.
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with bad JSON (%s)",
                               jerr.errorString().toUtf8().constData());
            return;
        }
        QJsonObject obj = doc.object();
        id = obj.value(QStringLiteral("id")).toString();
        mime = obj.value(QStringLiteral("mime")).toString();
        const double declaredSize = obj.value(QStringLiteral("size")).toDouble(-1);
        const qint64 maxSize = (m_HostContext.fileClipboard && mime == kFileClipboardMime)
            ? MAX_FILE_CLIPBOARD_BYTES : MAX_BLOB_BYTES;
        if (!std::isfinite(declaredSize) || declaredSize < 0 || declaredSize > static_cast<double>(maxSize) ||
            std::floor(declaredSize) != declaredSize) {
            return;
        }
        size = static_cast<qint64>(declaredSize);
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch()) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with invalid blob id");
            return;
        }
        if (mime != QStringLiteral("image/png") && !mime.startsWith(QStringLiteral("text/")) &&
            !(m_HostContext.fileClipboard && mime == kFileClipboardMime)) {
            return;
        }
    }
    // A server may broadcast our own frames back while an image upload is
    // still pending. Such echoes must not cancel that upload or split the
    // local compound clipboard into a text-only value.
    if (m_HaveClipboardSnapshot && token != 0 && token == m_OutgoingToken) {
        if (kind == KIND_TEXT && hashBytes(payload) == m_LastTextHash) {
            return;
        }
        QImage image;
        if (kind == KIND_PNG && decodeImage(payload, image, "PNG") &&
            hashImagePixels(image) == m_LastImageHash) {
            return;
        }
    }
    if (kind == KIND_REF && !m_LastOutboundRef.isEmpty() && payload == m_LastOutboundRef) {
        return;
    }
    while (!m_RetiredTokens.isEmpty() &&
           m_Clock.elapsed() - m_RetiredTokens.head().second > 30000) {
        m_RetiredTokens.dequeue();
    }
    for (const auto& retired : m_RetiredTokens) {
        if (token != 0 && token == retired.first) {
            return;
        }
    }
    if (token == 0 || token != m_CurrentToken) {
        beginClipboardChange(token);
    }

    if (kind == KIND_TEXT) {
        if (token == 0) {
            applyInboundText(payload);
        } else {
            handleBurstFrame(kind, token, payload);
        }
        return;
    }

    if (kind == KIND_PNG) {
        if (token == 0) {
            applyInboundPng(payload);
        } else {
            handleBurstFrame(kind, token, payload);
        }
        return;
    }

    if (kind == KIND_REF) {
        if (token == 0 || mime == kFileClipboardMime) {
            fetchRefAndApply(id, mime, size);
            return;
        }

        // Burst REF: start the fetch now; the bytes land in the aggregator
        // (or apply standalone if the burst window already closed).
        // Unsupported MIME types were rejected before starting this change.
        const uint8_t flavorKind = (mime == QStringLiteral("image/png")) ? KIND_PNG : KIND_TEXT;
        fetchBlob(id, size,
                  [this, token, flavorKind](const QByteArray& bytes, const QString& fetchedMime) {
                      Q_UNUSED(fetchedMime);
                      burstFlavorResolved(token, flavorKind, bytes);
                  });
        return;
    }

    // Unknown kind — ignore.
}

void ClipboardSync::applyInboundText(const QByteArray& payload)
{
    if (payload.contains('\0')) {
        ClipboardLog::warn("ClipboardSync: dropping inbound text payload with embedded NUL");
        return;
    }

#ifndef Q_OS_MACOS
    if (clipboardBridgeNeeded()
#ifdef Q_OS_WIN
        || m_WindowsClipboardPoll
#endif
        ) {
        if (clipboardBridgeFileGuard("inbound text")) {
            return;
        }

        rememberClipboard(QString::fromUtf8(payload).toUtf8(), QImage());
        clipboardBridgeWrite(QString::fromUtf8(payload), QImage(), QByteArray());
        return;
    }
#endif

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (!m_HostContext.fileClipboard && hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; inbound text ignored");
        return;
    }

    rememberClipboard(QString::fromUtf8(payload).toUtf8(), QImage());
    cb->setText(QString::fromUtf8(payload));
}

void ClipboardSync::applyInboundPng(const QByteArray& payload)
{
    QImage image;
    if (!decodeImage(payload, image, "PNG") || image.isNull()) {
        ClipboardLog::warn("ClipboardSync: dropping inbound PNG payload (decode failed, %d bytes)",
                    static_cast<int>(payload.size()));
        return;
    }

#ifndef Q_OS_MACOS
    if (clipboardBridgeNeeded()
#ifdef Q_OS_WIN
        || m_WindowsClipboardPoll
#endif
        ) {
        if (clipboardBridgeFileGuard("inbound PNG")) {
            return;
        }

        rememberClipboard(QByteArray(), image);
        clipboardBridgeWrite(QString(), image, payload);
        return;
    }
#endif

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (!m_HostContext.fileClipboard && hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; inbound PNG ignored");
        return;
    }

    rememberClipboard(QByteArray(), image);

    QMimeData* mime = new QMimeData();
    mime->setImageData(image);
    // Provide raw PNG bytes too so apps that prefer image/png over CF_DIB
    // (browsers, modern image editors) get the lossless copy verbatim.
    // Intentionally NOT setting HTML: pasting a giant base64 data: URI into
    // CF_HTML adds nothing useful (Office takes the bitmap path anyway) and
    // some Windows clipboard hooks reject oversize CF_HTML, dropping the
    // entire mime data set on the floor.
    mime->setData(QStringLiteral("image/png"), payload);
    cb->setMimeData(mime);
}

bool ClipboardSync::encodeImageAsPng(const QImage& image,
                                     QByteArray& outPng,
                                     const char* sourceDescription) const
{
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
        ClipboardLog::info("ClipboardSync: image too large from %s (%dx%d), dropping",
                    sourceDescription,
                    image.width(),
                    image.height());
        return false;
    }

    outPng.clear();
    outPng.reserve(64 * 1024);
    QBuffer buf(&outPng);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG") || outPng.isEmpty()) {
        ClipboardLog::warn("ClipboardSync: PNG encode failed for %s (%dx%d)",
                    sourceDescription,
                    image.width(),
                    image.height());
        outPng.clear();
        return false;
    }

    return true;
}

void ClipboardSync::sendClipboardPng(const QByteArray& png,
                                     const QString& sourceDescription,
                                     quint32 token)
{
    const QByteArray sourceUtf8 = sourceDescription.isEmpty()
            ? QByteArrayLiteral("unknown source")
            : sourceDescription.toUtf8();

    if (png.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: PNG payload %lld B from %s exceeds %lld B blob cap, dropping",
                    static_cast<long long>(png.size()),
                    sourceUtf8.constData(),
                    static_cast<long long>(MAX_BLOB_BYTES));
        return;
    }

    if (shouldTransferOutOfBand(png.size())) {
        uploadAndSendRef(png, QStringLiteral("image/png"), token);
        return;
    }

    QByteArray frame;
    if (encodeFrame(KIND_PNG, token, png, frame)) {
        ClipboardLog::debug("ClipboardSync: outbound PNG frame queued (%d bytes from %s, token %u)",
                     static_cast<int>(frame.size()),
                     sourceUtf8.constData(),
                     token);
        emit outboundFrame(frame);
    }
}

void ClipboardSync::sendTextOutbound(const QByteArray& utf8, quint32 token)
{
    if (utf8.isEmpty()) {
        return;
    }

    if (utf8.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: text payload %lld B exceeds %lld B blob cap, dropping",
                    static_cast<long long>(utf8.size()),
                    static_cast<long long>(MAX_BLOB_BYTES));
        return;
    }

    if (token == 0 && shouldTransferOutOfBand(utf8.size())) {
        ClipboardLog::debug("ClipboardSync: uploading outbound text blob (%lld bytes)",
                     static_cast<long long>(utf8.size()));
        // Sunshine's blob endpoint intentionally accepts only a bare
        // RFC 6838 type/subtype without parameters. KIND_TEXT already
        // defines the blob bytes as UTF-8.
        uploadAndSendRef(utf8, QStringLiteral("text/plain"), token);
        return;
    }

    QByteArray frame;
    if (!encodeFrame(KIND_TEXT, token, utf8, frame)) {
        return;
    }

    ClipboardLog::debug("ClipboardSync: outbound text frame queued (%d bytes, token %u)",
                 static_cast<int>(frame.size()),
                 token);
    emit outboundFrame(frame);
}

bool ClipboardSync::tryExtractImageBytes(const QMimeData* mime,
                                         const QStringList& preferredFormats,
                                         QByteArray& outPng,
                                         QString* outSourceDescription) const
{
    if (mime == nullptr) {
        return false;
    }

    QStringList candidateFormats = preferredFormats;
    for (const QString& format : mime->formats()) {
        if (!isImageLikeMimeFormat(format) || candidateFormats.contains(format, Qt::CaseInsensitive)) {
            continue;
        }
        candidateFormats.append(format);
    }

    for (const QString& format : candidateFormats) {
        QByteArray bytes = mime->data(format);
        if (bytes.isEmpty()) {
            continue;
        }

        QImage image;
        if (!decodeImage(bytes, image)) {
            continue;
        }

        if ((format.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""), Qt::CaseInsensitive) == 0)
                && looksLikePngBytes(bytes)) {
            const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
            if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                ClipboardLog::info("ClipboardSync: image too large from mime %s (%dx%d), dropping",
                            format.toUtf8().constData(),
                            image.width(),
                            image.height());
                return false;
            }
            outPng = bytes;
        }
        else if (!encodeImageAsPng(image, outPng, format.toUtf8().constData())) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = format;
        }
        return true;
    }

    return false;
}

bool ClipboardSync::tryExtractImageFromHtml(const QMimeData* mime,
                                            QByteArray& outPng,
                                            QString* outSourceDescription) const
{
    if (mime == nullptr || !mime->hasHtml()) {
        return false;
    }

    const QString html = mime->html();
    if (html.isEmpty()) {
        return false;
    }

    static const QRegularExpression imgSrcRegex(
        QStringLiteral("<img[^>]+src\\s*=\\s*['\"]([^'\"]+)['\"]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dataUrlRegex(
        QStringLiteral("^data:(image/[^;,]+)?;base64,(.+)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    QRegularExpressionMatchIterator it = imgSrcRegex.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString src = match.captured(1).trimmed();
        if (src.isEmpty()) {
            continue;
        }

        const QRegularExpressionMatch dataUrlMatch = dataUrlRegex.match(src);
        if (dataUrlMatch.hasMatch()) {
            QByteArray bytes = QByteArray::fromBase64(dataUrlMatch.captured(2).toUtf8());
            if (bytes.isEmpty()) {
                continue;
            }

            QImage image;
            if (!decodeImage(bytes, image)) {
                continue;
            }

            if (looksLikePngBytes(bytes)) {
                const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
                if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                    ClipboardLog::info("ClipboardSync: image too large from html data URL (%dx%d), dropping",
                                image.width(),
                                image.height());
                    return false;
                }
                outPng = bytes;
            } else if (!encodeImageAsPng(image, outPng, "html data url")) {
                continue;
            }

            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("html:data-url");
            }
            return true;
        }

        // Never open local files referenced by markup from another application.
    }

    return false;
}

bool ClipboardSync::extractClipboardPng(const QMimeData* mime,
                                        QByteArray& outPng,
                                        QString* outSourceDescription) const
{
    outPng.clear();
    if (outSourceDescription != nullptr) {
        outSourceDescription->clear();
    }

    if (mime == nullptr) {
        return false;
    }

    // Prefer raw image bytes attached by the producer (including our own
    // applyInboundPng which sets "image/png" verbatim) BEFORE falling back
    // to re-encoding mime->imageData(). This keeps echoed inbound PNGs
    // byte-identical so the FNV-1a hash matches the echo cache and we
    // don't ping-pong the same image back to the host.
    const QStringList preferredFormats {
        QStringLiteral("image/png"),
        QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""),
        QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""),
        QStringLiteral("image/tiff"),
        QStringLiteral("image/jpeg"),
        QStringLiteral("image/jpg"),
        QStringLiteral("image/bmp"),
        QStringLiteral("image/webp"),
        QStringLiteral("application/x-qt-image")
    };

    if (tryExtractImageBytes(mime, preferredFormats, outPng, outSourceDescription)) {
        return true;
    }

    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull() && encodeImageAsPng(image, outPng, "mime imageData")) {
            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("imageData");
            }
            return true;
        }
    }

    if (tryExtractImageFromHtml(mime, outPng, outSourceDescription)) {
        return true;
    }

    return false;
}

void ClipboardSync::onLocalClipboardChanged()
{
    if (!m_Active) {
        return;
    }

    // On the helper's worker thread, fetch a detached snapshot on the GUI
    // thread first; its deferred delete is posted back to the GUI loop.
    OwnedClipboardSnapshot owned;
    const QMimeData* mime = nullptr;
#ifdef Q_OS_WIN
    const bool native = m_WindowsClipboardPoll && m_WindowsClipboardPoll->isActive();
    const quint32 sequence = native ? GetClipboardSequenceNumber() : 0;
    std::unique_ptr<QMimeData> nativeSnapshot;
    if (native) {
        if (sequence == m_WindowsClipboardSequence) {
            return;
        }
        bool retry = false;
        nativeSnapshot = readWindowsClipboard(m_HostContext.fileClipboard, retry);
        if (retry) {
            return; // Another program owns the clipboard lock; try again on the next tick.
        }
        mime = nativeSnapshot.get();
    }
#endif
    if (!mime) {
#ifndef Q_OS_MACOS
        if (clipboardBridgeNeeded()) {
            owned.data = clipboardBridgeClone();
            mime = owned.data;
        } else
#endif
        {
            QClipboard* cb = QGuiApplication::clipboard();
            if (cb == nullptr) {
                return;
            }
            mime = cb->mimeData();
        }
    }
#ifdef Q_OS_WIN
    if (native) {
        if (sequence != GetClipboardSequenceNumber() ||
            (!mime && (IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                       IsClipboardFormatAvailable(CF_HDROP) ||
                       IsClipboardFormatAvailable(CF_DIB)))) {
            return;
        }
        m_WindowsClipboardSequence = sequence;
    }
#endif
    QMimeData emptyClipboard;
    if (mime == nullptr) {
        // An empty clipboard may have no MIME object. Keep processing it as
        // empty content so the snapshot state is updated without dereferencing
        // a null pointer.
        mime = &emptyClipboard;
    }

    // File copies must not leak their contents as clipboard messages:
    // Finder/Explorer copies would send the file name as text and their
    // icon bitmap as an image. But document- and chat-app image copies
    // (Preview, WeChat/QQ message copies) also attach local file
    // references alongside the real bitmap — those must still sync, or
    // the most common macOS image-copy flows silently do nothing.
    // Distinguish by origin: Finder stamps its file clipboards with
    // noderef/fndf markers; anything else carrying a transferable image
    // is treated as an image copy. File references without a usable
    // image stay skipped (the inbound guards below still protect a
    // pending local file paste from being clobbered by remote content).
    bool finderOrigin = false;
#ifdef Q_OS_MACOS
    // Finder stamps its file copies with node-reference flavors that Qt
    // never surfaces in QMimeData::formats(); ask the raw pasteboard.
    finderOrigin = ClipboardHelperIsFinderPasteboard();
#endif
    if (mime != nullptr) {
        for (const QString& format : mime->formats()) {
            if (format.contains(QStringLiteral("Shell IDList Array"), Qt::CaseInsensitive) ||
                format.contains(QStringLiteral("FileGroupDescriptor"), Qt::CaseInsensitive)) {
                finderOrigin = true;
            }
        }
    }
    if (m_HostContext.fileClipboard && mime->hasUrls() &&
        (finderOrigin || (!mime->hasImage() && hasFileReferences(mime)))) {
        const auto urls = mime->urls();
        const auto receiveRoot = clipboardTransferRoot() + '/';
        if (std::any_of(urls.cbegin(), urls.cend(), [&receiveRoot](const QUrl& url) {
                return url.toLocalFile().startsWith(receiveRoot, Qt::CaseInsensitive);
            })) {
            return;
        }
        beginClipboardChange();
        m_HaveClipboardSnapshot = false;
        uploadFilesAndSendRef(urls);
        return;
    }
    if (finderOrigin) {
        beginClipboardChange();
        m_HaveClipboardSnapshot = false;
        ClipboardLog::debug("ClipboardSync: Finder file clipboard detected; sync skipped");
        return;
    }

    // Image takes precedence — some applications attach a fallback text label
    // (file path, alt text) alongside the bitmap; we want the picture, not the
    // path. To match HarmonyOS' record iteration behavior more closely, try
    // multiple extraction paths rather than only mime->imageData().
    QByteArray png;
    QString imageSourceDescription;
    const bool havePng = extractClipboardPng(mime, png, &imageSourceDescription);
    if (hasFileReferences(mime) && !havePng) {
        beginClipboardChange();
        m_HaveClipboardSnapshot = false;
        ClipboardLog::debug(
            "ClipboardSync: file clipboard without transferable image; sync skipped");
        return;
    }

    // A clipboard change can genuinely carry both flavors (browser image
    // copies attach the source URL, IM clients alt text). Emit both as a
    // compound burst so aggregating peers restore the full clipboard; peers
    // without aggregation apply the frames in order and keep the v1 outcome.
    // Note the burst text frame is inline-only: a REF text frame would race
    // the image frame onto the wire (blob upload latency) and flip legacy
    // peers' final state to text, so oversize text is dropped from bursts.
    QString text = hasFileReferences(mime) ? QString() : mime->text();
    QByteArray utf8;
    if (!text.isEmpty()) {
        utf8 = text.toUtf8();
    }

    QImage image;
    if (havePng && !decodeImage(png, image, "PNG")) {
        return;
    }
    const uint64_t textHash = hashBytes(utf8);
    const uint64_t imageHash = hashImagePixels(image);
    if (m_HaveClipboardSnapshot && textHash == m_LastTextHash && imageHash == m_LastImageHash) {
        return;
    }
    beginClipboardChange();
    rememberClipboard(utf8, image);

    if (havePng) {
        if (!utf8.isEmpty() && utf8.size() <= MAX_INLINE_PAYLOAD) {
            const quint32 token = nextToken();
            m_OutgoingToken = token;
            sendTextOutbound(utf8, token);
            sendClipboardPng(png, imageSourceDescription, token);
        } else {
            if (!utf8.isEmpty()) {
                ClipboardLog::info("ClipboardSync: burst text %lld B exceeds inline cap; sending image only",
                            static_cast<long long>(utf8.size()));
            }
            sendClipboardPng(png, imageSourceDescription);
        }
        return;
    }

    if (mime != nullptr) {
        bool hasImageLikeHints = mime->hasImage() || mime->hasUrls() || mime->hasHtml();
        if (!hasImageLikeHints) {
            for (const QString& format : mime->formats()) {
                if (isImageLikeMimeFormat(format)) {
                    hasImageLikeHints = true;
                    break;
                }
            }
        }

        if (hasImageLikeHints) {
            const QString formatsSummary = mime->formats().join(QStringLiteral(", "));
            ClipboardLog::debug("ClipboardSync: no transferable image found in clipboard formats [%s]",
                         formatsSummary.toUtf8().constData());
        }
    }

    sendTextOutbound(utf8, 0);
}

bool ClipboardSync::encodeFrame(uint8_t kind, quint32 token, const QByteArray& payload, QByteArray& outFrame) const
{
    if (payload.size() > MAX_INLINE_PAYLOAD) {
        return false;
    }

    outFrame.clear();
    outFrame.reserve(10 + payload.size());

    // u8 version
    outFrame.append(static_cast<char>(WIRE_VERSION));
    // u8 kind
    outFrame.append(static_cast<char>(kind));
    // u32 token (LE) - 0 for standalone frames; frames of one compound burst
    // share a non-zero token (see class comment).
    quint32 tokenLE = qToLittleEndian<quint32>(token);
    outFrame.append(reinterpret_cast<const char*>(&tokenLE), sizeof(tokenLE));
    // u32 length (LE)
    quint32 lenLE = qToLittleEndian<quint32>(static_cast<quint32>(payload.size()));
    outFrame.append(reinterpret_cast<const char*>(&lenLE), sizeof(lenLE));
    // bytes payload
    outFrame.append(payload);
    return true;
}

bool ClipboardSync::decodeFrame(const QByteArray& frame,
                                uint8_t& outKind,
                                quint32& outToken,
                                QByteArray& outPayload) const
{
    // 1 + 1 + 4 + 4 = 10 byte header minimum.
    if (frame.size() < 10) {
        return false;
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.constData());
    uint8_t version = p[0];
    if (version != WIRE_VERSION) {
        return false;
    }

    outKind = p[1];

    quint32 tokenLE = 0;
    memcpy(&tokenLE, p + 2, sizeof(tokenLE));
    outToken = qFromLittleEndian<quint32>(tokenLE);

    quint32 lenLE = 0;
    memcpy(&lenLE, p + 6, sizeof(lenLE));
    quint32 length = qFromLittleEndian<quint32>(lenLE);

    if (length > MAX_PAYLOAD || length != static_cast<quint32>(frame.size() - 10)) {
        return false;
    }

    outPayload = QByteArray(reinterpret_cast<const char*>(p + 10),
                            static_cast<int>(length));
    return true;
}

uint64_t ClipboardSync::hashBytes(const QByteArray& bytes)
{
    // FNV-1a 64-bit. Cheap and good enough for echo suppression.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < bytes.size(); ++i) {
        h ^= static_cast<uint8_t>(bytes[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

uint64_t ClipboardSync::hashImagePixels(const QImage& image)
{
    if (image.isNull()) {
        return 0;
    }

    // Normalize to one straight-alpha format so identical pixel content
    // hashes identically no matter which flavor (PNG/TIFF/DIB) or Qt format
    // it came back as. Opaque images (the overwhelmingly common clipboard
    // case) survive every lossless conversion bit-exact; semi-transparent
    // pixels may drift by rounding through a premultiplied intermediate,
    // which at worst degrades back to byte-hash-only suppression.
    const QImage canonical = image.convertToFormat(QImage::Format_RGBA8888);
    if (canonical.isNull() || canonical.width() <= 0 || canonical.height() <= 0) {
        return 0;
    }

    uint64_t h = 0xcbf29ce484222325ULL;
    const auto mixBytes = [&h](const uchar* bytes, int count) {
        for (int i = 0; i < count; ++i) {
            h ^= bytes[i];
            h *= 0x100000001b3ULL;
        }
    };

    // Fold dimensions in so equal bytes under different geometry don't collide.
    const quint32 dimensions[2] = {
        static_cast<quint32>(canonical.width()),
        static_cast<quint32>(canonical.height()),
    };
    mixBytes(reinterpret_cast<const uchar*>(dimensions), sizeof(dimensions));

    // Scan row data only, skipping any per-row stride padding.
    const int rowBytes = canonical.width() * 4;
    for (int y = 0; y < canonical.height(); ++y) {
        mixBytes(canonical.constScanLine(y), rowBytes);
    }

    return h;
}

bool ClipboardSync::decodeImage(const QByteArray& bytes, QImage& image, const char* format)
{
    if (bytes.isEmpty() || bytes.size() > MAX_BLOB_BYTES) {
        return false;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format ? QByteArray(format) : QByteArray());
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 ||
        qint64(size.width()) * size.height() > MAX_IMAGE_PIXELS) {
        return false;
    }
    image = reader.read();
    return !image.isNull() && qint64(image.width()) * image.height() <= MAX_IMAGE_PIXELS;
}

void ClipboardSync::rememberClipboard(const QByteArray& text, const QImage& image)
{
    m_HaveClipboardSnapshot = true;
    m_LastTextHash = hashBytes(text);
    m_LastImageHash = hashImagePixels(image);
}

void ClipboardSync::beginClipboardChange(quint32 token)
{
    ++m_Revision;
    if (m_CurrentToken != 0) {
        m_RetiredTokens.enqueue(qMakePair(m_CurrentToken, m_Clock.elapsed()));
        while (m_RetiredTokens.size() > MAX_PENDING_BURSTS) {
            m_RetiredTokens.dequeue();
        }
    }
    m_CurrentToken = token;
    m_OutgoingToken = 0;
    m_LastOutboundRef.clear();
    const auto replies = m_BlobReplies.values();
    for (QNetworkReply* reply : replies) {
        reply->abort();
    }
    const auto tokens = m_PendingBursts.keys();
    for (quint32 oldToken : tokens) {
        removeBurst(oldToken);
    }
}

QNetworkAccessManager* ClipboardSync::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        if (!m_HostContext.managedHostToken.isEmpty()) {
            m_Nam->setProxy(QNetworkProxy::NoProxy);
        }
        // Validate every new TLS connection before HTTP data is sent. Reused
        // connections belong to this manager's immutable paired identity.
        const QSslCertificate pinned = m_HostContext.serverCertificate;
        PairedCertificate::enforce(m_Nam, this, pinned);
        connect(
            m_Nam, &QNetworkAccessManager::sslErrors, this,
            [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                if (PairedCertificate::canIgnoreErrors(m_HostContext.serverCertificate, errors)) {
                    reply->ignoreSslErrors(errors);
                }
            });
    }
    return m_Nam;
}

bool ClipboardSync::buildBlobUrl(const QString& tail, QUrl& outUrl) const
{
    if (m_HostContext.address.isEmpty() ||
            m_HostContext.httpsPort == 0 ||
            m_HostContext.serverCertificate.isNull()) {
        return false;
    }
    outUrl = QUrl();
    outUrl.setScheme(QStringLiteral("https"));
    outUrl.setHost(m_HostContext.address);
    outUrl.setPort(m_HostContext.httpsPort);
    outUrl.setPath(QStringLiteral("/api/v1/clipboard") + tail);
    return outUrl.isValid();
}

void ClipboardSync::readBlobReply(
    QNetworkReply* reply, qint64 limit,
    const std::function<void(const QByteArray&, const QString&)>& onFetched)
{
    m_BlobReplies.insert(reply);
    const quint64 revision = m_Revision;
    const QSslCertificate pinned = m_HostContext.serverCertificate;
    const auto bytes = std::make_shared<QByteArray>();
    const auto lastSent = std::make_shared<qint64>(0);
    reply->setReadBufferSize(256 * 1024);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(30000);
    connect(reply, &QNetworkReply::uploadProgress, this, [timeout, lastSent](qint64 sent, qint64) {
        if (sent > *lastSent) {
            *lastSent = sent;
            timeout->start(30000);
        }
    });
    const auto drain = [reply, bytes, limit, timeout]() {
        if (!reply->isOpen() || reply->error() != QNetworkReply::NoError) {
            return;
        }
        timeout->start(30000);
        bytes->append(reply->read(limit - bytes->size() + 1));
        if (bytes->size() > limit) {
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, bytes, limit]() {
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (contentLength > limit) {
            reply->abort();
        } else if (contentLength > 0) {
            bytes->reserve(static_cast<qsizetype>(contentLength));
        }
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, bytes, revision, pinned, timeout, onFetched, limit]() {
            timeout->stop();
            m_BlobReplies.remove(reply);
            reply->deleteLater();
            if (!m_Active || revision != m_Revision) {
                return;
            }
            // Read the last bytes even when the backend omitted a final readyRead.
            // Do not re-enter finished via abort() while draining the completed reply.
            if (reply->error() == QNetworkReply::NoError && reply->isOpen()) {
                bytes->append(reply->read(limit - bytes->size() + 1));
            }
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || bytes->size() > limit ||
                !PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate()) ||
                status < 200 || status >= 300) {
                ClipboardLog::warn("ClipboardSync: blob transfer failed or exceeded its limits");
                // Let an explicit repeat copy retry a failed upload.
                if (reply->operation() == QNetworkAccessManager::PostOperation) {
                    m_HaveClipboardSnapshot = false;
                }
                return;
            }
            onFetched(*bytes, reply->header(QNetworkRequest::ContentTypeHeader).toString());
        });
}

void ClipboardSync::uploadFilesAndSendRef(const QList<QUrl>& urls, quint32 token)
{
    QByteArray headerAndMeta;
    QStringList filePaths;
    QVector<qint64> fileSizes;
    qint64 totalBytes = 0;
    if (!inspectClipboardFiles(urls, MAX_FILE_CLIPBOARD_BYTES, headerAndMeta, filePaths, fileSizes, totalBytes)) {
        ClipboardLog::info("ClipboardSync: skipping unsupported or oversized file clipboard selection");
        return;
    }

    // Small file selections use the fast single-shot upload path.
    if (totalBytes <= FILE_STREAM_THRESHOLD) {
        const auto bytes = encodeClipboardFiles(urls, FILE_STREAM_THRESHOLD);
        if (!bytes.isEmpty()) {
            uploadAndSendRef(bytes, kFileClipboardMime, token);
        }
        return;
    }

    QUrl initUrl;
    if (!m_Active || m_BlobReplies.size() >= 4 || !buildBlobUrl(QStringLiteral("/blob/init"), initUrl)) {
        m_HaveClipboardSnapshot = false;
        return;
    }

    struct StreamUploadState {
        QByteArray headerAndMeta;
        QStringList filePaths;
        QVector<qint64> fileSizes;
        qint64 totalBytes = 0;
        qint64 headerOffset = 0;
        int fileIndex = 0;
        qint64 fileOffset = 0;
        QString blobId;
        quint64 revision = 0;
        quint32 token = 0;
    };

    auto state = std::make_shared<StreamUploadState>();
    state->headerAndMeta = std::move(headerAndMeta);
    state->filePaths = std::move(filePaths);
    state->fileSizes = std::move(fileSizes);
    state->totalBytes = totalBytes;
    state->revision = m_Revision;
    state->token = token;

    auto makeRequest = [this](const QUrl& url) {
        QNetworkRequest req(url);
        if (!m_HostContext.managedHostToken.isEmpty()) {
            req.setRawHeader("X-SunAndMon-Clipboard", m_HostContext.managedHostToken);
        }
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
        sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
        sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
        req.setSslConfiguration(sslConfig);
        return req;
    };

    auto sendNextChunk = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakSend(sendNextChunk);
    *sendNextChunk = [this, state, makeRequest, weakSend]() {
        if (!m_Active || state->revision != m_Revision) {
            return;
        }
        QByteArray chunk;
        chunk.reserve(static_cast<qsizetype>(FILE_CHUNK_BYTES));
        if (state->headerOffset < state->headerAndMeta.size()) {
            const qint64 take = qMin<qint64>(FILE_CHUNK_BYTES,
                                             state->headerAndMeta.size() - state->headerOffset);
            chunk.append(state->headerAndMeta.constData() + state->headerOffset,
                         static_cast<qsizetype>(take));
            state->headerOffset += take;
        }
        while (chunk.size() < FILE_CHUNK_BYTES && state->fileIndex < state->filePaths.size()) {
            const qint64 expectedSize = state->fileSizes[state->fileIndex];
            if (state->fileOffset >= expectedSize) {
                ++state->fileIndex;
                state->fileOffset = 0;
                continue;
            }
            QFile file(state->filePaths[state->fileIndex]);
            if (!file.open(QIODevice::ReadOnly) || !file.seek(state->fileOffset)) {
                ClipboardLog::warn("ClipboardSync: failed to read local file for chunked upload");
                m_HaveClipboardSnapshot = false;
                return;
            }
            const qint64 want = qMin<qint64>(FILE_CHUNK_BYTES - chunk.size(),
                                             expectedSize - state->fileOffset);
            const QByteArray part = file.read(want);
            if (part.isEmpty() || file.error() != QFileDevice::NoError) {
                ClipboardLog::warn("ClipboardSync: local file changed during chunked upload");
                m_HaveClipboardSnapshot = false;
                return;
            }
            chunk.append(part);
            state->fileOffset += part.size();
            if (state->fileOffset >= expectedSize) {
                ++state->fileIndex;
                state->fileOffset = 0;
            }
        }
        while (state->fileIndex < state->filePaths.size() &&
               state->fileOffset >= state->fileSizes[state->fileIndex]) {
            ++state->fileIndex;
            state->fileOffset = 0;
        }
        if (chunk.isEmpty()) {
            return;
        }
        QUrl chunkUrl;
        if (!buildBlobUrl(QStringLiteral("/blob/") + state->blobId + QStringLiteral("/chunk"), chunkUrl)) {
            m_HaveClipboardSnapshot = false;
            return;
        }
        const bool isLast = state->headerOffset >= state->headerAndMeta.size() &&
                            state->fileIndex >= state->filePaths.size();
        auto nextSend = weakSend.lock();
        readBlobReply(nam()->post(makeRequest(chunkUrl), chunk), 16 * 1024,
                      [this, state, isLast, nextSend](const QByteArray& response, const QString&) {
                          if (!m_Active || state->revision != m_Revision) {
                              return;
                          }
                          const QJsonObject obj = QJsonDocument::fromJson(response).object();
                          const bool complete = obj.value(QStringLiteral("complete")).toBool(false);
                          if (isLast) {
                              if (!complete) {
                                  ClipboardLog::warn("ClipboardSync: chunked upload did not complete");
                                  m_HaveClipboardSnapshot = false;
                                  return;
                              }
                              QJsonObject meta;
                              meta.insert(QStringLiteral("id"), state->blobId);
                              meta.insert(QStringLiteral("mime"), kFileClipboardMime);
                              meta.insert(QStringLiteral("size"), static_cast<double>(state->totalBytes));
                              QByteArray frame;
                              m_LastOutboundRef = QJsonDocument(meta).toJson(QJsonDocument::Compact);
                              if (encodeFrame(KIND_REF, state->token, m_LastOutboundRef, frame)) {
                                  emit outboundFrame(frame);
                              }
                              return;
                          }
                          if (nextSend) {
                              (*nextSend)();
                          }
                      });
    };

    QNetworkRequest initReq = makeRequest(initUrl);
    initReq.setRawHeader("X-Clipboard-Mime", kFileClipboardMime.toUtf8());
    initReq.setRawHeader("X-Clipboard-Total-Size", QByteArray::number(totalBytes));
    readBlobReply(nam()->post(initReq, QByteArray()), 16 * 1024,
                  [this, state, sendNextChunk](const QByteArray& response, const QString&) {
                      if (!m_Active || state->revision != m_Revision) {
                          return;
                      }
                      const QString id = QJsonDocument::fromJson(response).object()
                                             .value(QStringLiteral("id")).toString();
                      if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"))
                               .match(id)
                               .hasMatch()) {
                          m_HaveClipboardSnapshot = false;
                          return;
                      }
                      state->blobId = id;
                      (*sendNextChunk)();
                  });
}

void ClipboardSync::uploadAndSendRef(const QByteArray& payload, const QString& mime, quint32 token)
{
    QUrl url;
    if (!m_Active || payload.size() > MAX_BLOB_BYTES || m_BlobReplies.size() >= 4 ||
        !buildBlobUrl(QStringLiteral("/blob"), url)) {
        m_HaveClipboardSnapshot = false;
        return;
    }
    QNetworkRequest req(url);
    if (!m_HostContext.managedHostToken.isEmpty()) {
        req.setRawHeader("X-SunAndMon-Clipboard", m_HostContext.managedHostToken);
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    req.setRawHeader("X-Clipboard-Mime", mime.toUtf8());
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    readBlobReply(nam()->post(req, payload), 16 * 1024,
                  [this, mime, token, payloadSize = payload.size()](const QByteArray& response,
                                                                    const QString&) {
                      const QJsonDocument doc = QJsonDocument::fromJson(response);
                      const QString id = doc.object().value(QStringLiteral("id")).toString();
                      if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"))
                               .match(id)
                               .hasMatch()) {
                          m_HaveClipboardSnapshot = false;
                          return;
                      }
                      QJsonObject meta;
                      meta.insert(QStringLiteral("id"), id);
                      meta.insert(QStringLiteral("mime"), mime);
                      meta.insert(QStringLiteral("size"), static_cast<double>(payloadSize));
                      QByteArray frame;
                      m_LastOutboundRef = QJsonDocument(meta).toJson(QJsonDocument::Compact);
                      if (encodeFrame(KIND_REF, token, m_LastOutboundRef, frame)) {
                          emit outboundFrame(frame);
                      }
                  });
}

void ClipboardSync::applyInboundFiles(const QByteArray& payload)
{
    const auto urls = extractClipboardFiles(payload);
    if (urls.isEmpty()) {
        return;
    }
    writeClipboardFiles(urls);
}

void ClipboardSync::writeClipboardFiles(const QList<QUrl>& urls, int retriesRemaining)
{
    bool written = false;
#ifdef Q_OS_WIN
    const bool native = m_WindowsClipboardPoll && m_WindowsClipboardPoll->isActive();
    const auto sequence = m_WindowsClipboardSequence;
    auto sequenceAfterWrite = sequence;
#else
    Q_UNUSED(retriesRemaining)
#endif
    const auto apply = [&] {
#ifdef Q_OS_WIN
        if (native && sequence != GetClipboardSequenceNumber()) {
            return;
        }
#endif
        if (auto* clipboard = QGuiApplication::clipboard()) {
            auto* mime = new QMimeData;
            mime->setUrls(urls);
            clipboard->setMimeData(mime);
#ifndef Q_OS_MACOS
            written = clipboardWriteSucceeded(clipboard);
#else
            written = true;
#endif
        }
#ifdef Q_OS_WIN
        if (written && native) {
            sequenceAfterWrite = GetClipboardSequenceNumber();
        }
#endif
    };
#ifndef Q_OS_MACOS
    if (clipboardBridgeNeeded()) {
        QMetaObject::invokeMethod(m_Bridge, apply, Qt::BlockingQueuedConnection);
    } else
#endif
    {
        apply();
    }
#ifdef Q_OS_WIN
    if (written) {
        m_WindowsClipboardSequence = sequenceAfterWrite;
    } else if (native && sequence == GetClipboardSequenceNumber() && retriesRemaining > 0) {
        const auto revision = m_Revision;
        QTimer::singleShot(100, this, [this, urls, sequence, revision, retriesRemaining] {
            if (m_Active && revision == m_Revision && sequence == GetClipboardSequenceNumber()) {
                writeClipboardFiles(urls, retriesRemaining - 1);
            }
        });
    }
#else
    Q_UNUSED(written)
#endif
}

void ClipboardSync::fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize)
{
    if (mime == kFileClipboardMime) {
        fetchFileBlobAndApply(id, advertisedSize);
        return;
    }
    fetchBlob(id, advertisedSize, [this, mime](const QByteArray& bytes, const QString& fetchedMime) {
        // Legacy text and PNG peers use the MIME from the descriptor.
        if (mime == QStringLiteral("image/png")) {
            applyInboundPng(bytes);
        } else if (mime.startsWith(QStringLiteral("text/"))) {
            applyInboundText(bytes);
        } else if (mime == kFileClipboardMime && fetchedMime == kFileClipboardMime) {
            applyInboundFiles(bytes);
        } else {
            ClipboardLog::info("ClipboardSync: dropping fetched blob with unsupported mime '%s'",
                        mime.toUtf8().constData());
        }
    });
}

void ClipboardSync::fetchFileBlobAndApply(const QString& id, qint64 advertisedSize)
{
    QUrl url;
    if (!m_Active || m_BlobReplies.size() >= 4 ||
        !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch() ||
        !buildBlobUrl(QStringLiteral("/blob/") + id, url)) {
        return;
    }
    const auto transferRoot = clipboardTransferRoot();
    if (transferRoot.isEmpty() || !QDir().mkpath(transferRoot)) {
        return;
    }

    struct StreamDownloadState {
        QByteArray headerBuf;
        bool metadataParsed = false;
        quint32 metadataSize = 0;
        QStringList fileNames;
        QVector<qint64> fileSizes;
        qint64 expectedTotalBytes = 0;
        qint64 receivedBytes = 0;
        int fileIndex = 0;
        qint64 currentFileWritten = 0;
        QString transferRoot;
        std::unique_ptr<QTemporaryDir> transferDir;
        std::unique_ptr<QFile> currentFile;
        QList<QUrl> urls;
        bool failed = false;
    };

    auto state = std::make_shared<StreamDownloadState>();
    state->transferRoot = transferRoot;

    auto advanceFiles = [](const std::shared_ptr<StreamDownloadState>& st) -> bool {
        if (st->currentFile) {
            if (!st->currentFile->flush()) {
                return false;
            }
            st->currentFile->close();
            st->currentFile.reset();
        }
        while (st->fileIndex < st->fileNames.size()) {
            auto file = std::make_unique<QFile>(st->transferDir->filePath(st->fileNames[st->fileIndex]));
            if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                return false;
            }
            st->urls.append(QUrl::fromLocalFile(file->fileName()));
            st->currentFileWritten = 0;
            if (st->fileSizes[st->fileIndex] == 0) {
                if (!file->flush()) {
                    return false;
                }
                file->close();
                ++st->fileIndex;
                continue;
            }
            st->currentFile = std::move(file);
            return true;
        }
        return true;
    };

    const qint64 limit = advertisedSize > 0 ? advertisedSize : MAX_FILE_CLIPBOARD_BYTES;
    auto feedChunk = [state, limit, advertisedSize, advanceFiles](const QByteArray& chunk) {
        if (state->failed || chunk.isEmpty()) {
            return;
        }
        if (chunk.size() > limit - state->receivedBytes) {
            state->failed = true;
            return;
        }
        state->receivedBytes += chunk.size();
        const char* ptr = chunk.constData();
        qint64 remaining = chunk.size();

        while (remaining > 0 && !state->failed) {
            if (!state->metadataParsed) {
                if (state->headerBuf.size() < 4) {
                    const qint64 take = qMin<qint64>(4 - state->headerBuf.size(), remaining);
                    state->headerBuf.append(ptr, static_cast<qsizetype>(take));
                    ptr += take;
                    remaining -= take;
                    if (state->headerBuf.size() < 4) {
                        continue;
                    }
                    state->metadataSize = qFromBigEndian<quint32>(state->headerBuf.constData());
                    if (state->metadataSize == 0 || state->metadataSize > kMaxClipboardMetadataBytes ||
                        4 + static_cast<qint64>(state->metadataSize) > limit) {
                        state->failed = true;
                        return;
                    }
                }
                const qint64 headerTotal = 4 + static_cast<qint64>(state->metadataSize);
                if (state->headerBuf.size() < headerTotal) {
                    const qint64 take = qMin<qint64>(headerTotal - state->headerBuf.size(), remaining);
                    state->headerBuf.append(ptr, static_cast<qsizetype>(take));
                    ptr += take;
                    remaining -= take;
                    if (state->headerBuf.size() < headerTotal) {
                        continue;
                    }
                }
                const auto entries = QJsonDocument::fromJson(
                    state->headerBuf.mid(4, static_cast<qsizetype>(state->metadataSize))).array();
                state->headerBuf.clear();
                if (entries.isEmpty() || entries.size() > kMaxClipboardFiles) {
                    state->failed = true;
                    return;
                }
                QSet<QString> names;
                qint64 totalDataBytes = 0;
                for (const auto& value : entries) {
                    const auto entry = value.toObject();
                    const auto name = entry.value("name").toString();
                    const auto size = entry.value("size").toInteger(-1);
                    const auto key = name.toCaseFolded();
                    if (!validClipboardFilename(name) || names.contains(key) || size < 0 ||
                        size > MAX_FILE_CLIPBOARD_BYTES - headerTotal - totalDataBytes) {
                        state->failed = true;
                        return;
                    }
                    state->fileNames.append(name);
                    state->fileSizes.append(size);
                    totalDataBytes += size;
                    names.insert(key);
                }
                state->expectedTotalBytes = headerTotal + totalDataBytes;
                if ((advertisedSize > 0 && state->expectedTotalBytes != advertisedSize) ||
                    state->expectedTotalBytes > limit) {
                    state->failed = true;
                    return;
                }
                state->transferDir = std::make_unique<QTemporaryDir>(
                    QDir(state->transferRoot).filePath("paste-XXXXXX"));
                if (!state->transferDir->isValid() || !advanceFiles(state)) {
                    state->failed = true;
                    return;
                }
                state->metadataParsed = true;
                continue;
            }

            if (!state->currentFile || state->fileIndex >= state->fileNames.size()) {
                state->failed = true;
                return;
            }
            const qint64 want = qMin<qint64>(
                remaining, state->fileSizes[state->fileIndex] - state->currentFileWritten);
            if (state->currentFile->write(ptr, want) != want) {
                state->failed = true;
                return;
            }
            ptr += want;
            remaining -= want;
            state->currentFileWritten += want;
            if (state->currentFileWritten >= state->fileSizes[state->fileIndex]) {
                ++state->fileIndex;
                if (!advanceFiles(state)) {
                    state->failed = true;
                    return;
                }
            }
        }
    };

    QNetworkRequest req(url);
    if (!m_HostContext.managedHostToken.isEmpty()) {
        req.setRawHeader("X-SunAndMon-Clipboard", m_HostContext.managedHostToken);
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    QNetworkReply* reply = nam()->get(req);
    m_BlobReplies.insert(reply);
    const quint64 revision = m_Revision;
    const QSslCertificate pinned = m_HostContext.serverCertificate;
    reply->setReadBufferSize(256 * 1024);

    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(30000);

    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, limit]() {
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (contentLength > limit) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, state, timeout, feedChunk]() {
        if (!reply->isOpen() || reply->error() != QNetworkReply::NoError) {
            return;
        }
        timeout->start(30000);
        while (reply->bytesAvailable() > 0 && !state->failed) {
            feedChunk(reply->read(256 * 1024));
        }
        if (state->failed) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, state, revision, pinned, timeout, advertisedSize, feedChunk]() {
                timeout->stop();
                m_BlobReplies.remove(reply);
                reply->deleteLater();
                if (reply->error() == QNetworkReply::NoError && reply->isOpen()) {
                    while (reply->bytesAvailable() > 0 && !state->failed) {
                        feedChunk(reply->read(256 * 1024));
                    }
                }
                if (state->currentFile) {
                    state->currentFile->close();
                    state->currentFile.reset();
                }
                if (!m_Active || revision != m_Revision) {
                    return;
                }
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QString fetchedMime = reply->header(QNetworkRequest::ContentTypeHeader).toString();
                if (state->failed ||
                    reply->error() != QNetworkReply::NoError ||
                    !PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate()) ||
                    status < 200 || status >= 300 ||
                    fetchedMime != kFileClipboardMime ||
                    !state->metadataParsed ||
                    state->fileIndex != state->fileNames.size() ||
                    state->receivedBytes != state->expectedTotalBytes ||
                    (advertisedSize > 0 && state->receivedBytes != advertisedSize) ||
                    state->urls.isEmpty() || !state->transferDir) {
                    ClipboardLog::warn(
                        "ClipboardSync: file blob download failed (status=%d err=%d received=%lld expected=%lld)",
                        status,
                        static_cast<int>(reply->error()),
                        static_cast<long long>(state->receivedBytes),
                        static_cast<long long>(state->expectedTotalBytes));
                    return;
                }
                state->transferDir->setAutoRemove(false);
                pruneOldClipboardTransfers(state->transferRoot, state->transferDir->path());
                writeClipboardFiles(state->urls);
            });
}

void ClipboardSync::fetchBlob(const QString& id,
                              qint64 advertisedSize,
                              const std::function<void(const QByteArray&, const QString&)>& onFetched)
{
    QUrl url;
    if (!m_Active || m_BlobReplies.size() >= 4 ||
        !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch() ||
        !buildBlobUrl(QStringLiteral("/blob/") + id, url)) {
        return;
    }
    QNetworkRequest req(url);
    if (!m_HostContext.managedHostToken.isEmpty()) {
        req.setRawHeader("X-SunAndMon-Clipboard", m_HostContext.managedHostToken);
    }
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);
    readBlobReply(nam()->get(req), advertisedSize > 0 ? advertisedSize : MAX_BLOB_BYTES,
                  [advertisedSize, onFetched](const QByteArray& bytes, const QString& mime) {
                      if (advertisedSize > 0 && bytes.size() != advertisedSize) {
                          ClipboardLog::warn("ClipboardSync: blob size mismatch");
                          return;
                      }
                      onFetched(bytes, mime);
                  });
}

quint32 ClipboardSync::nextToken()
{
    quint32 token = m_NextToken++;
    if (m_NextToken == 0) {
        m_NextToken = 1;
    }
    return token;
}

void ClipboardSync::handleBurstFrame(uint8_t kind, quint32 token, const QByteArray& payload)
{
    if (!m_PendingBursts.contains(token)) {
        if (m_PendingBursts.size() >= MAX_PENDING_BURSTS) {
            // Defensive bound: a pathological sender must not grow the map
            // without limit; drop the oldest (already-applied) burst.
            ClipboardLog::warn("ClipboardSync: burst backlog exceeded; dropping oldest burst");
            removeBurst(m_PendingBursts.constBegin().key());
        }

        BurstState state;
        state.timer = new QTimer(this);
        state.timer->setSingleShot(true);
        connect(state.timer, &QTimer::timeout, this, [this, token]() {
            if (token == m_CurrentToken && !m_BlobReplies.isEmpty()) {
                m_PendingBursts[token].timer->start(BURST_RETENTION_MS);
                return;
            }
            removeBurst(token);
        });
        state.timer->start(BURST_RETENTION_MS);
        m_PendingBursts.insert(token, state);
    }

    BurstState& state = m_PendingBursts[token];
    if (kind == KIND_TEXT && !state.haveText) {
        state.haveText = true;
        state.textPayload = payload;
    } else if (kind == KIND_PNG && !state.havePng) {
        state.havePng = true;
        state.pngPayload = payload;
    } else {
        // Duplicate kind for this token; keep the first copy.
        return;
    }

    if (state.haveText && state.havePng) {
        applyCompound(state.textPayload, state.pngPayload);
    } else if (kind == KIND_TEXT) {
        applyInboundText(state.textPayload);
    } else {
        applyInboundPng(state.pngPayload);
    }
}

void ClipboardSync::burstFlavorResolved(quint32 token, uint8_t kind, const QByteArray& payload)
{
    if (kind == 0) {
        // Unsupported REF mime: nothing to contribute.
        return;
    }

    if (token != m_CurrentToken) {
        return;
    }

    handleBurstFrame(kind, token, payload);
}

void ClipboardSync::removeBurst(quint32 token)
{
    const auto it = m_PendingBursts.find(token);
    if (it == m_PendingBursts.end()) {
        return;
    }

    QTimer* timer = it.value().timer;
    m_PendingBursts.erase(it);
    if (timer != nullptr) {
        timer->stop();
        timer->deleteLater();
    }
}

void ClipboardSync::applyCompound(const QByteArray& text, const QByteArray& png)
{
#ifndef Q_OS_MACOS
    if (clipboardBridgeNeeded()
#ifdef Q_OS_WIN
        || m_WindowsClipboardPoll
#endif
        ) {
        if (clipboardBridgeFileGuard("inbound compound")) {
            return;
        }
    } else
#endif
    {
        QClipboard* cb = QGuiApplication::clipboard();
        if (cb == nullptr) {
            return;
        }

        if (!m_HostContext.fileClipboard && hasFileReferences(cb->mimeData())) {
            ClipboardLog::debug(
                "ClipboardSync: preserving local file clipboard; inbound compound ignored");
            return;
        }
    }

    const bool textOk = !text.isEmpty() && !text.contains('\0');
    QImage image;
    if (!decodeImage(png, image, "PNG") || image.isNull()) {
        image = QImage();
    }

    if (textOk && !image.isNull()) {
#ifdef Q_OS_MACOS
        // Qt's macOS clipboard backend drops the text flavor from a
        // QMimeData that also carries an image, so compound writes go
        // through the native pasteboard instead.
        rememberClipboard(QString::fromUtf8(text).toUtf8(), image);
        if (ClipboardHelperWritePasteboardCompound(
                text.constData(), reinterpret_cast<const unsigned char*>(png.constData()),
                png.size())) {
            ClipboardLog::debug("ClipboardSync: applied compound clipboard (text %d B + PNG %d B)",
                         text.size(),
                         png.size());
            return;
        }
        ClipboardLog::warn("ClipboardSync: native compound pasteboard write failed; falling back to image only");
#endif

        rememberClipboard(QString::fromUtf8(text).toUtf8(), image);

#ifndef Q_OS_MACOS
        if (clipboardBridgeNeeded()
#ifdef Q_OS_WIN
            || m_WindowsClipboardPoll
#endif
            ) {
            clipboardBridgeWrite(QString::fromUtf8(text), image, png);
        } else
#endif
        {
            QMimeData* mime = new QMimeData();
            mime->setText(QString::fromUtf8(text));
            mime->setImageData(image);
            // Provide raw PNG bytes too so apps that prefer image/png over
            // CF_DIB get the lossless copy verbatim (mirrors applyInboundPng).
            mime->setData(QStringLiteral("image/png"), png);
            QClipboard* cb = QGuiApplication::clipboard();
            if (cb != nullptr) {
                cb->setMimeData(mime);
            } else {
                delete mime;
            }
        }
        ClipboardLog::debug("ClipboardSync: applied compound clipboard (text %d B + PNG %d B)",
                     text.size(),
                     png.size());
        return;
    }

    // One flavor was unusable — degrade to a single-flavor apply (each of
    // these records its own echo identities).
    if (textOk) {
        applyInboundText(text);
    } else if (!image.isNull()) {
        applyInboundPng(png);
    }
}
