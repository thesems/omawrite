#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMimeData>
#include <QProcess>
#include <QPrintDialog>
#include <QPrinter>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>
#include <QWindow>

#include <algorithm>

#include "markdownhighlighter.h"
#include "markdownstructure.h"

namespace {
using MarkdownStructure::listPrefix;

bool updateFence(const QString &text, QChar &fence, int &length) {
    const int state = fence.isNull() ? 0 : (length << 1) | int(fence == QLatin1Char('~'));
    const auto line = MarkdownStructure::fenceLine(text, state);
    length = line.nextState >> 1;
    fence = line.nextState == 0 ? QChar() : QLatin1Char((line.nextState & 1) ? '~' : '`');
    return line.code;
}
}

constexpr qreal typoraLineHeightPercent = 140;
const QString lastSaveDirectorySetting = QStringLiteral("file/lastSaveDirectory");
const QString editorFontSetting = QStringLiteral("editor/font");

// Noto ships a separate family per script, which buries every other font in
// the picker. Offer only the core Latin families; the rest still serve as
// fallbacks for whatever scripts a document contains.
const QStringList keptNotoFamilies = {QStringLiteral("Noto Sans"),
                                      QStringLiteral("Noto Sans Mono"),
                                      QStringLiteral("Noto Serif")};

// Fontconfig's generic aliases just point at another listed family.
const QStringList genericFontAliases = {QStringLiteral("Monospace"), QStringLiteral("Mono"),
                                        QStringLiteral("Sans Serif"), QStringLiteral("Sans"),
                                        QStringLiteral("Serif"), QStringLiteral("System-ui")};

// Symbol fonts often claim Latin coverage because they map their glyphs onto
// ASCII, so they can only be told apart by name. D050000L is URW's Dingbats.
const QRegularExpression symbolFontName(
    QStringLiteral("symbol|dingbat|emoji|icon|awesome|^D050000L$"),
    QRegularExpression::CaseInsensitiveOption);

QString Backend::normalizedLinkUrl(const QString &clipboardText) {
    QString candidate = clipboardText.trimmed();
    static const QRegularExpression lineBreakRe(QStringLiteral("[\\r\\n]"));
    const int lineBreak = candidate.indexOf(lineBreakRe);
    if (lineBreak >= 0)
        candidate = candidate.left(lineBreak).trimmed();

    if (candidate.isEmpty())
        return {};

    if (candidate.startsWith(QStringLiteral("www."), Qt::CaseInsensitive))
        candidate.prepend(QStringLiteral("https://"));

    static const QRegularExpression schemeRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (!schemeRe.match(candidate).hasMatch())
        return {};

    const QUrl url(candidate);
    if (!url.isValid() || url.scheme().isEmpty())
        return {};

    const QString scheme = url.scheme().toLower();
    const bool webUrl = scheme == QStringLiteral("http")
        || scheme == QStringLiteral("https")
        || scheme == QStringLiteral("ftp");
    if (webUrl && url.host().isEmpty())
        return {};

    if (!webUrl && scheme != QStringLiteral("mailto"))
        return {};

    return url.toString();
}

Backend::Backend(QObject *parent) : QObject(parent) {
    const QString savedFont = QSettings().value(editorFontSetting).toString();
    m_editorFont = !savedFont.isEmpty() && QFontDatabase::hasFamily(savedFont)
        ? savedFont
        : defaultEditorFont();

    const QString stateDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(stateDirectory);
    // Claim an orphaned snapshot before taking an empty slot. This ensures a
    // crash in window 2 is still recovered even if window 1 exited normally.
    for (int pass = 0; pass < 2 && !m_recoveryLock; ++pass) {
        for (int slot = 0; slot < 100; ++slot) {
            const QString base = QDir(stateDirectory).filePath(
                QStringLiteral("recovery-%1").arg(slot));
            const bool snapshotExists = QFileInfo::exists(base + QStringLiteral(".json"));
            if ((pass == 0) != snapshotExists)
                continue;
            auto lock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
            if (lock->tryLock()) {
                m_recoveryPath = base + QStringLiteral(".json");
                m_recoveryLock = std::move(lock);
                break;
            }
        }
    }
    m_wordCountTimer.setSingleShot(true);
    m_wordCountTimer.setInterval(120);
    connect(&m_wordCountTimer, &QTimer::timeout, this, &Backend::refreshWordCount);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setInterval(750);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &Backend::writeRecovery);
    connect(&m_fileWatcher, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &path) {
                if (path != m_fileUrl.toLocalFile())
                    return;

                const bool deleted = !QFileInfo::exists(path);
                if (!deleted && m_hasKnownFileContents) {
                    QFile file(path);
                    if (file.open(QIODevice::ReadOnly)
                            && file.readAll() == m_lastKnownFileContents) {
                        // Atomic saves can replace the watched inode. Re-arm the
                        // watcher, but do not report our own save as an outside edit.
                        watchCurrentFile();
                        return;
                    }
                }

                emit externalChangeDetected(deleted, m_modified);
            });

    loadOmarchyTheme();
    watchOmarchyTheme();
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, [this]() {
        loadOmarchyTheme();
        watchOmarchyTheme();
    });
}

Backend::~Backend() = default;

void Backend::setParentWindow(QWindow *window) {
    m_parentWindow = window;
}

QString Backend::fileName() const {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty())
        return QStringLiteral("Untitled.md");

    if (m_fileUrl.isLocalFile()) {
        const QFileInfo info(m_fileUrl.toLocalFile());
        if (!info.fileName().isEmpty())
            return info.fileName();
    }

    const QString name = m_fileUrl.fileName();
    return name.isEmpty() ? QStringLiteral("Untitled.md") : name;
}

void Backend::setDarkMode(bool darkMode) {
    if (m_darkMode == darkMode)
        return;

    m_darkMode = darkMode;
    loadOmarchyTheme();
    emit darkModeChanged();
}

void Backend::setTextScale(qreal textScale) {
    if (qFuzzyCompare(m_textScale, textScale))
        return;

    m_textScale = textScale;
    emit textScaleChanged();
}

void Backend::attachDocument(QObject *textDocument) {
    auto *quickDocument = qobject_cast<QQuickTextDocument *>(textDocument);
    if (!quickDocument || !quickDocument->textDocument()) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    if (m_highlighter)
        delete m_highlighter.data();

    m_document = quickDocument->textDocument();
    m_lastDocumentText = m_document->toPlainText();
    m_highlighter = new MarkdownHighlighter(m_document);
    m_highlighter->setDarkMode(m_darkMode);
    m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);

    applyDocumentTypography();
    restoreRecovery();
}

void Backend::openDialog() {
    emit openDialogRequested();
}

void Backend::relocateFile(const QUrl &oldUrl, const QUrl &newUrl) {
    if (!m_fileUrl.isLocalFile()) return;
    const QString current = m_fileUrl.toLocalFile();
    const QString oldPath = oldUrl.toLocalFile();
    if (current != oldPath && !current.startsWith(oldPath + '/')) return;
    setFileUrl(QUrl::fromLocalFile(newUrl.toLocalFile() + current.mid(oldPath.size())));
    watchCurrentFile();
    if (m_modified) scheduleRecovery();
}

void Backend::detachTrashedFile(const QUrl &url) {
    if (m_fileUrl != url && (!m_fileUrl.isLocalFile()
        || !m_fileUrl.toLocalFile().startsWith(url.toLocalFile() + '/'))) return;
    setFileUrl(QUrl());
    watchCurrentFile();
    setModified(true);
    scheduleRecovery();
    setStatus(QStringLiteral("Moved to Trash; text kept as an unsaved draft"));
}

void Backend::open(const QUrl &url) {
    if (!url.isLocalFile()) {
        setStatus(QStringLiteral("Only local files can be opened."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStatus(QStringLiteral("Could not open %1.").arg(targetName));
        return;
    }

    const QByteArray contents = file.readAll();
    loadDocumentText(QString::fromUtf8(contents));
    clearRecovery();
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = true;
    setFileUrl(url);
    watchCurrentFile();
    setModified(false);
    setStatus(QStringLiteral("Opened %1").arg(fileName()));
    emit documentOpened();
}

void Backend::save() {
    if (!m_fileUrl.isValid() || m_fileUrl.isEmpty()) {
        saveAsDialog();
        return;
    }

    saveTo(m_fileUrl);
}

void Backend::saveForClose() {
    if (!m_modified) {
        emit closeAfterSave();
        return;
    }

    m_closeAfterSave = true;
    save();
}

void Backend::saveAsDialog() {
    emit saveDialogRequested(suggestedSaveUrl());
}

void Backend::saveAs(const QUrl &url) {
    saveTo(url);
}

void Backend::fileDialogCanceled() {
    m_closeAfterSave = false;
}

void Backend::discardRecovery() {
    clearRecovery();
}

void Backend::reloadFromDisk() {
    if (m_fileUrl.isLocalFile())
        open(m_fileUrl);
}

void Backend::keepExternalVersion() {
    QFile file(m_fileUrl.toLocalFile());
    if (file.open(QIODevice::ReadOnly)) {
        m_lastKnownFileContents = file.readAll();
        m_hasKnownFileContents = true;
    } else {
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
    }
    setModified(true);
    scheduleRecovery();
    watchCurrentFile();
    setStatus(QStringLiteral("Kept your version"));
}

void Backend::printDocument() {
    if (!m_document) {
        setStatus(QStringLiteral("There is no document to print."));
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dialog(&printer);
    dialog.setWindowTitle(QStringLiteral("Print %1").arg(fileName()));
    dialog.winId();
    if (dialog.windowHandle() && m_parentWindow)
        dialog.windowHandle()->setTransientParent(m_parentWindow);

    if (dialog.exec() == QDialog::Accepted) {
        QTextDocument rendered;
        rendered.setDefaultFont(m_document->defaultFont());
        rendered.setMarkdown(currentDocumentText());
        rendered.print(&printer);
    }
}

void Backend::newWindow() {
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                 QStringList());
    if (!started)
        setStatus(QStringLiteral("Could not open a new window."));
}

QString Backend::clipboardUrl() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    if (!mimeData)
        return {};

    if (mimeData->hasUrls()) {
        const QList<QUrl> urls = mimeData->urls();
        for (const QUrl &url : urls) {
            const QString normalized = normalizedLinkUrl(url.toString());
            if (!normalized.isEmpty())
                return normalized;
        }
    }

    if (!mimeData->hasText())
        return {};

    return normalizedLinkUrl(mimeData->text());
}

QString Backend::clipboardText() const {
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return {};

    const QMimeData *mimeData = clipboard->mimeData();
    return mimeData && mimeData->hasText() ? mimeData->text() : QString();
}

bool Backend::editorTextChanged() {
    if (m_loading || m_formattingTypography)
        return false;

    const QString text = currentDocumentText();
    if (text == m_lastDocumentText)
        return false;
    m_lastDocumentText = text;

    refreshDocumentTypography();

    scheduleWordCount();
    setModified(true);
    setStatus(QStringLiteral("Unsaved"));
    scheduleRecovery();
    return true;
}

QVariantList Backend::hiddenRangesAt(int position) const {
    QVariantList ranges;
    if (!m_document)
        return ranges;

    const QTextBlock block =
        m_document->findBlock(qBound(0, position, m_document->characterCount() - 1));
    if (!block.isValid())
        return ranges;

    const int lineStart = block.position();
    QList<QPair<int, int>> spans;
    const auto task = checklistAt(position);
    if (!task.isEmpty()) {
        const int marker = task.value(QStringLiteral("position")).toInt();
        spans.append({marker, marker + 3});
    }
    const QList<MarkdownHighlighter::InlineMarkup> markup =
        MarkdownHighlighter::inlineMarkup(block.text());
    for (const MarkdownHighlighter::InlineMarkup &item : markup) {
        for (const MarkdownHighlighter::Span &marker : item.markers) {
            spans.append({lineStart + marker.start,
                          lineStart + marker.start + marker.length});
        }
    }
    std::sort(spans.begin(), spans.end());

    for (const auto &span : spans) {
        ranges.append(QVariantMap{{QStringLiteral("start"), span.first},
                                  {QStringLiteral("end"), span.second}});
    }
    return ranges;
}

void Backend::setSearchHighlight(const QString &query, int currentMatchStart) {
    if (m_highlighter)
        m_highlighter->setSearch(query, currentMatchStart);
}

void Backend::openExternalUrl(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https")
            || scheme == QStringLiteral("mailto"))
        QDesktopServices::openUrl(url);
}

QVariantMap Backend::windowGeometry() const {
    QSettings settings;
    return {{QStringLiteral("x"), settings.value(QStringLiteral("window/x"), -1)},
            {QStringLiteral("y"), settings.value(QStringLiteral("window/y"), -1)},
            {QStringLiteral("width"), settings.value(QStringLiteral("window/width"), 1280)},
            {QStringLiteral("height"), settings.value(QStringLiteral("window/height"), 820)},
            {QStringLiteral("maximized"), settings.value(QStringLiteral("window/maximized"), false)}};
}

void Backend::saveWindowGeometry(int x, int y, int width, int height, bool maximized) {
    QSettings settings;
    if (!maximized) {
        settings.setValue(QStringLiteral("window/x"), x);
        settings.setValue(QStringLiteral("window/y"), y);
        settings.setValue(QStringLiteral("window/width"), width);
        settings.setValue(QStringLiteral("window/height"), height);
    }
    settings.setValue(QStringLiteral("window/maximized"), maximized);
}

QString Backend::defaultEditorFont() {
    return QStringLiteral("IBM Plex Mono");
}

void Backend::setEditorFont(const QString &family) {
    if (family.isEmpty() || family == m_editorFont)
        return;

    m_editorFont = family;
    QSettings().setValue(editorFontSetting, family);
    emit editorFontChanged();
}

QStringList Backend::availableFonts() const {
    // Leave out icon, emoji, and other symbol fonts, along with anything that
    // cannot set the Latin text the editor is written in.
    QStringList families;
    for (const QString &family : QFontDatabase::families(QFontDatabase::Latin)) {
        if (QFontDatabase::isPrivateFamily(family)
                || QFontDatabase::writingSystems(family).contains(QFontDatabase::Symbol))
            continue;
        families.append(family);
    }
    return selectableFontFamilies(families);
}

QStringList Backend::selectableFontFamilies(const QStringList &families) {
    QStringList selectable;
    static const QRegularExpression foundrySuffix(QStringLiteral("\\s*\\[[^\\]]*\\]$"));
    for (QString family : families) {
        // The same family from two foundries is listed as "Name [Foundry]".
        family.remove(foundrySuffix);
        if (family.startsWith(QStringLiteral("Noto ")) && !keptNotoFamilies.contains(family))
            continue;
        if (genericFontAliases.contains(family, Qt::CaseInsensitive)
                || family.contains(symbolFontName))
            continue;
        if (family == defaultEditorFont() || selectable.contains(family, Qt::CaseInsensitive))
            continue;
        selectable.append(family);
    }
    std::sort(selectable.begin(), selectable.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    selectable.prepend(defaultEditorFont());
    return selectable;
}

void Backend::documentFontChanged() {
    if (m_highlighter)
        m_highlighter->refreshFont();
    refreshDocumentTypography();
}

void Backend::loadDocumentText(const QString &text) {
    if (!m_document) {
        setStatus(QStringLiteral("Could not attach the Markdown renderer."));
        return;
    }

    m_loading = true;
    m_document->setPlainText(text);
    m_lastDocumentText = text;
    m_loading = false;

    applyDocumentTypography();
    m_wordCountTimer.stop();
    setWordCount(countWords(text));
}

void Backend::setFileUrl(const QUrl &url) {
    if (m_fileUrl == url)
        return;

    m_fileUrl = url;
    emit fileUrlChanged();
    watchCurrentFile();
}

void Backend::setModified(bool modified) {
    if (m_modified == modified)
        return;

    m_modified = modified;
    emit modifiedChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;

    m_status = status;
    emit statusChanged();
}

void Backend::saveTo(const QUrl &url) {
    if (!url.isLocalFile()) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Only local files can be saved."));
        return;
    }

    const QString targetName = QFileInfo(url.toLocalFile()).fileName();
    QSaveFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not save %1.").arg(targetName));
        return;
    }

    const QByteArray contents = currentDocumentText().toUtf8();
    file.write(contents);

    // QSaveFile commits by replacing the target. Stop watching the old inode
    // before that replacement so our own write is not classified as external.
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);

    // commit() flushes, fsyncs, and atomically renames the temp file into place,
    // returning false (and leaving the original untouched) on any write error.
    if (!file.commit()) {
        watchCurrentFile();
        m_closeAfterSave = false;
        setStatus(QStringLiteral("Could not write %1.").arg(targetName));
        return;
    }

    const bool shouldClose = m_closeAfterSave;
    m_closeAfterSave = false;
    m_lastKnownFileContents = contents;
    m_hasKnownFileContents = true;
    setFileUrl(url);
    watchCurrentFile();
    QSettings().setValue(lastSaveDirectorySetting,
                         QFileInfo(url.toLocalFile()).absolutePath());
    setModified(false);
    setStatus(QStringLiteral("Saved %1").arg(fileName()));
    clearRecovery();
    emit saveSucceeded();

    if (shouldClose)
        emit closeAfterSave();
}

void Backend::scheduleRecovery() {
    m_recoveryTimer.start();
}

QString Backend::recoveryPath() const {
    return m_recoveryPath;
}

void Backend::writeRecovery() {
    if (!m_modified)
        return;
    const QString path = recoveryPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QJsonObject recovery{{QStringLiteral("fileUrl"), m_fileUrl.toString()},
                               {QStringLiteral("text"), currentDocumentText()}};
    file.write(QJsonDocument(recovery).toJson(QJsonDocument::Compact));
    file.commit();
}

void Backend::restoreRecovery() {
    QFile file(recoveryPath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll());
    if (!json.isObject() || !json.object().contains(QStringLiteral("text")))
        return;
    const QJsonObject recovery = json.object();
    loadDocumentText(recovery.value(QStringLiteral("text")).toString());
    const QUrl recoveredUrl(recovery.value(QStringLiteral("fileUrl")).toString());
    QFile diskFile(recoveredUrl.toLocalFile());
    if (recoveredUrl.isLocalFile() && diskFile.open(QIODevice::ReadOnly)) {
        m_lastKnownFileContents = diskFile.readAll();
        m_hasKnownFileContents = true;
    } else {
        m_lastKnownFileContents.clear();
        m_hasKnownFileContents = false;
    }
    setFileUrl(recoveredUrl);
    setModified(true);
    setStatus(QStringLiteral("Recovered unsaved changes"));
}

void Backend::clearRecovery() {
    m_recoveryTimer.stop();
    QFile::remove(recoveryPath());
}

void Backend::watchCurrentFile() {
    const QStringList watched = m_fileWatcher.files();
    if (!watched.isEmpty())
        m_fileWatcher.removePaths(watched);
    if (m_fileUrl.isLocalFile() && QFileInfo::exists(m_fileUrl.toLocalFile()))
        m_fileWatcher.addPath(m_fileUrl.toLocalFile());
}

void Backend::loadOmarchyTheme() {
    m_themeBackground = m_darkMode ? QStringLiteral("#101010") : QStringLiteral("#ffffff");
    m_themeForeground = m_darkMode ? QStringLiteral("#eeeeee") : QStringLiteral("#222324");
    m_themeAccent = m_darkMode ? QStringLiteral("#5584aa") : QStringLiteral("#2077b2");
    m_themeSelection = m_darkMode ? QStringLiteral("#186a9a") : QStringLiteral("#2077b2");

    const QString colorsPath = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current/theme/colors.toml");
    QString themeMode;
    QFile file(colorsPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;

            const int equals = line.indexOf(QLatin1Char('='));
            if (equals < 0)
                continue;

            const QString key = line.left(equals).trimmed();
            QString value = line.mid(equals + 1).trimmed();
            if (value.size() >= 2
                    && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
                        || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
                value = value.mid(1, value.size() - 2);

            if (key == QStringLiteral("mode"))
                themeMode = value;
            else if (key == QStringLiteral("background"))
                m_themeBackground = value;
            else if (key == QStringLiteral("foreground"))
                m_themeForeground = value;
            else if (key == QStringLiteral("accent"))
                m_themeAccent = value;
            else if (key == QStringLiteral("selection"))
                m_themeSelection = value;
        }
    }

    bool themeModeKnown = false;
    bool themeIsDark = m_darkMode;
    if (themeMode == QStringLiteral("dark")) {
        themeIsDark = true;
        themeModeKnown = true;
    } else if (themeMode == QStringLiteral("light")) {
        themeIsDark = false;
        themeModeKnown = true;
    } else {
        const QColor background(m_themeBackground);
        if (background.isValid()) {
            const double luminance = 0.299 * background.redF()
                + 0.587 * background.greenF() + 0.114 * background.blueF();
            themeIsDark = luminance < 0.5;
            themeModeKnown = true;
        }
    }
    if (themeModeKnown && themeIsDark != m_darkMode) {
        m_darkMode = themeIsDark;
        emit darkModeChanged();
    }

    if (m_highlighter) {
        m_highlighter->setDarkMode(m_darkMode);
        m_highlighter->setColors(m_themeBackground, m_themeForeground, m_themeAccent);
    }

    emit themeColorsChanged();
}

void Backend::watchOmarchyTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = QDir::homePath()
        + QStringLiteral("/.local/state/omarchy/current");
    const QString themeDir = currentDir + QStringLiteral("/theme");
    const QString colorsPath = themeDir + QStringLiteral("/colors.toml");

    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFile::exists(colorsPath))
        m_themeWatcher.addPath(colorsPath);
}

QUrl Backend::suggestedSaveUrl() const {
    if (m_fileUrl.isLocalFile())
        return m_fileUrl;

    const QString savedDirectory = QSettings().value(lastSaveDirectorySetting).toString();
    const QDir directory = savedDirectory.isEmpty() || !QDir(savedDirectory).exists()
        ? QDir::home()
        : QDir(savedDirectory);
    return QUrl::fromLocalFile(
        directory.filePath(suggestedFileName(currentDocumentText())));
}

QString Backend::currentDocumentText() const {
    return m_document ? m_document->toPlainText() : QString();
}

int Backend::countWords(const QString &text) {
    static const QRegularExpression wordRe(
        QStringLiteral("[\\p{L}\\p{N}]+(?:['-][\\p{L}\\p{N}]+)*"));
    int count = 0;
    QRegularExpressionMatchIterator it = wordRe.globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

QString Backend::suggestedFileName(const QString &text) {
    QString name = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    name.replace(QRegularExpression(QStringLiteral("[/\\x00-\\x1f\\x7f]")),
                 QStringLiteral("-"));
    name = name.left(120).trimmed();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        name = QStringLiteral("Untitled");
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");
    return name;
}

void Backend::setWordCount(int words) {
    if (m_wordCount == words)
        return;

    m_wordCount = words;
    emit wordCountChanged();
}

void Backend::refreshWordCount() {
    setWordCount(countWords(currentDocumentText()));
}

void Backend::scheduleWordCount() {
    m_wordCountTimer.start();
}

void Backend::applyDocumentTypography() {
    if (!m_document)
        return;

    const bool undoEnabled = m_document->isUndoRedoEnabled();
    m_document->setUndoRedoEnabled(false);
    refreshDocumentTypography();
    m_document->setUndoRedoEnabled(undoEnabled);
}

void Backend::refreshDocumentTypography() {
    if (!m_document)
        return;

    if (m_formattingTypography)
        return;
    m_formattingTypography = true;
    const QFontMetricsF metrics(m_document->defaultFont());
    const qreal tabWidth = qMax(qreal(1), metrics.horizontalAdvance(QLatin1Char(' ')) * 4);
    QTextOption option = m_document->defaultTextOption();
    if (!qFuzzyCompare(option.tabStopDistance(), tabWidth)) {
        option.setTabStopDistance(tabWidth);
        m_document->setDefaultTextOption(option);
    }
    const auto advance = [&metrics, tabWidth](const QString &text, qreal width) {
        for (QChar character : text) {
            if (character == QLatin1Char('\t'))
                width = (qFloor(width / tabWidth) + 1) * tabWidth;
            else
                width += metrics.horizontalAdvance(character);
        }
        return width;
    };
    QChar fence;
    int fenceLength = 0;
    QVariantList tasks;
    QTextCursor cursor(m_document);
    // A fence edit can change later blocks, so scan the document but only
    // update changed formats. Join the text edit to keep undo atomic.
    cursor.joinPreviousEditBlock();
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        const bool inFence = updateFence(text, fence, fenceLength);
        const auto prefix = inFence ? QRegularExpressionMatch() : listPrefix(text);
        qreal prefixWidth = 0;
        if (prefix.hasMatch())
            prefixWidth = advance(prefix.captured(), 0);
        const auto task = inFence ? QRegularExpressionMatch() : MarkdownStructure::checklist(text);
        if (task.hasMatch()) {
            prefixWidth += MarkdownStructure::checkboxWidth(m_document->defaultFont());
            prefixWidth = advance(task.captured(4), prefixWidth);
            tasks.append(QVariantMap{
                {QStringLiteral("lineStart"), block.position()},
                {QStringLiteral("position"), block.position() + task.capturedStart(2)},
                {QStringLiteral("contentStart"), block.position() + task.capturedEnd()},
                {QStringLiteral("checked"), task.captured(3).compare(QLatin1String("x"), Qt::CaseInsensitive) == 0}
            });
        }
        // Keep literal Markdown on the first line; wrapped text starts beneath
        // the item content, including the indentation of nested items.
        QTextBlockFormat format = block.blockFormat();
        if (qFuzzyCompare(format.leftMargin() + 1, prefixWidth + 1)
                && qFuzzyCompare(format.textIndent() + 1, 1 - prefixWidth)
                && format.lineHeight() == typoraLineHeightPercent)
            continue;
        format.setLeftMargin(prefixWidth);
        format.setTextIndent(-prefixWidth);
        format.setLineHeight(typoraLineHeightPercent, QTextBlockFormat::ProportionalHeight);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
    m_formattingTypography = false;
    if (m_checklistItems != tasks) {
        m_checklistItems = tasks;
        emit checklistItemsChanged();
    }
}

QVariantMap Backend::checklistAt(int position) const {
    if (!m_document) return {};
    const int lineStart = m_document->findBlock(position).position();
    for (const auto &value : m_checklistItems) {
        const auto item = value.toMap();
        if (item.value(QStringLiteral("lineStart")).toInt() == lineStart) return item;
    }
    return {};
}

bool Backend::isCodeBlockAt(int position) const {
    if (!m_document) return false;
    int state = 0;
    const auto target = m_document->findBlock(position);
    for (auto block = m_document->begin(); block.isValid(); block = block.next()) {
        const auto line = MarkdownStructure::fenceLine(block.text(), state);
        if (block == target) return line.code;
        state = line.nextState;
    }
    return false;
}

QVariantMap Backend::toggleChecklist(int start, int end) {
    if (!m_document) return {};
    start = qBound(0, start, m_document->characterCount() - 1);
    end = qBound(start, end, m_document->characterCount() - 1);
    const auto first = m_document->findBlock(start);
    const auto last = m_document->findBlock(end > start ? end - 1 : end);
    struct Edit { int position; int length; QString text; };
    QList<Edit> edits;
    int state = 0;
    for (auto block = m_document->begin(); block.isValid(); block = block.next()) {
        const auto line = MarkdownStructure::fenceLine(block.text(), state);
        state = line.nextState;
        if (block.position() > last.position()) break;
        if (block.position() < first.position() || line.code) continue;
        const auto prefix = listPrefix(block.text());
        if (!prefix.hasMatch()) continue;
        const auto task = MarkdownStructure::checklist(block.text());
        if (task.hasMatch()) {
            const bool checked = task.captured(3).compare(QLatin1String("x"), Qt::CaseInsensitive) == 0;
            edits.append({block.position() + int(task.capturedStart(3)), 1,
                          checked ? QStringLiteral(" ") : QStringLiteral("x")});
        } else {
            edits.append({block.position() + int(prefix.capturedEnd()), 0, QStringLiteral("[ ] ")});
        }
    }
    if (edits.isEmpty()) return {};
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    int adjustedStart = start, adjustedEnd = end;
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        cursor.insertText(it->text);
        const int delta = it->text.size() - it->length;
        if (it->position <= start) adjustedStart += delta;
        if (it->position <= end) adjustedEnd += delta;
    }
    cursor.endEditBlock();
    return {{QStringLiteral("start"), adjustedStart}, {QStringLiteral("end"), adjustedEnd}};
}

QVariantMap Backend::renumberList(int start, int end) {
    if (!m_document) return {};
    struct Level { int indent; int number; QString path; };
    struct Edit { int position; int length; QString marker; };
    QList<Level> levels;
    QList<Edit> edits;
    QList<Edit> replacements;
    int groupStart = -1;
    QChar fence;
    int fenceLength = 0;
    int adjustedStart = start, adjustedEnd = end;
    const auto flush = [&](int groupEnd) {
        if (groupStart >= 0 && start < groupEnd && end >= groupStart)
            replacements.append(edits);
        edits.clear();
        levels.clear();
        groupStart = -1;
    };
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        const bool inFence = updateFence(text, fence, fenceLength);
        const auto prefix = inFence ? QRegularExpressionMatch() : listPrefix(text);
        if (!prefix.hasMatch()) {
            if (!inFence && groupStart >= 0 && (text.trimmed().isEmpty()
                    || text.startsWith(QLatin1Char(' ')) || text.startsWith(QLatin1Char('\t'))))
                continue;
            flush(block.position());
            continue;
        }
        if (groupStart < 0) groupStart = block.position();
        int indent = 0;
        for (QChar c : prefix.captured(1))
            indent = c == QLatin1Char('\t') ? (indent / 4 + 1) * 4 : indent + 1;
        while (!levels.isEmpty() && levels.last().indent > indent) levels.removeLast();
        const QString marker = prefix.captured(2);
        const bool numbered = marker.front().isDigit();
        int number = 0;
        if (!levels.isEmpty() && levels.last().indent == indent) {
            const auto previous = levels.takeLast();
            if (numbered && previous.number > 0) number = previous.number + 1;
        }
        if (numbered && number == 0)
            number = levels.isEmpty() ? marker.section(QLatin1Char('.'), 0, 0)
                .remove(QLatin1Char(')')).toInt() : 1;
        QString path;
        if (numbered) {
            const QString parent = levels.isEmpty() ? QString() : levels.last().path;
            path = parent.isEmpty() ? QString::number(number) : parent + QLatin1Char('.') + QString::number(number);
            const QString replacement = path + marker.back();
            if (replacement != marker)
                edits.append({block.position() + int(prefix.capturedStart(2)), int(marker.size()), replacement});
        }
        levels.append({indent, number, path});
    }
    flush(m_document->characterCount());
    // Apply from the end after scanning to preserve all source positions.
    QTextCursor cursor(m_document);
    cursor.joinPreviousEditBlock();
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it) {
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        cursor.insertText(it->marker);
        const int delta = it->marker.size() - it->length;
        const auto adjust = [&](int original, int &position) {
            if (original >= it->position + it->length) position += delta;
            else if (original > it->position)
                position = it->position + qMin(original - it->position, int(it->marker.size()));
        };
        adjust(start, adjustedStart);
        adjust(end, adjustedEnd);
    }
    cursor.endEditBlock();
    return {{QStringLiteral("start"), adjustedStart}, {QStringLiteral("end"), adjustedEnd}};
}

QVariantMap Backend::indentList(int start, int end, bool outdent) {
    if (!m_document) return {};
    start = qBound(0, start, m_document->characterCount() - 1);
    end = qBound(start, end, m_document->characterCount() - 1);
    const QTextBlock first = m_document->findBlock(start);
    const QTextBlock last = m_document->findBlock(end > start ? end - 1 : end);
    QList<QPair<int, int>> edits;
    bool numbered = false;
    QChar fence;
    int fenceLength = 0;
    for (QTextBlock block = m_document->begin(); block.isValid(); block = block.next()) {
        const bool inFence = updateFence(block.text(), fence, fenceLength);
        if (block.position() > last.position()) break;
        if (block.position() < first.position()) continue;
        const auto prefix = inFence ? QRegularExpressionMatch() : listPrefix(block.text());
        if (!prefix.hasMatch()) return {};
        numbered = numbered || prefix.captured(2).front().isDigit();
        const QString indentation = prefix.captured(1);
        int removed = 0;
        if (outdent && !indentation.isEmpty()) {
            if (indentation.front() == QLatin1Char('\t')) {
                removed = 1;
            } else {
                while (removed < qMin(4, int(indentation.size()))
                       && indentation.at(removed) == QLatin1Char(' '))
                    ++removed;
            }
        }
        edits.append({block.position(), removed});
    }
    QTextCursor cursor(m_document);
    cursor.beginEditBlock();
    int adjustedStart = start, adjustedEnd = end;
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        cursor.setPosition(it->first);
        cursor.setPosition(it->first + it->second, QTextCursor::KeepAnchor);
        if (outdent) cursor.removeSelectedText();
        else cursor.insertText(QStringLiteral("    "));
        const int delta = outdent ? -it->second : 4;
        if (it->first <= start) adjustedStart = qMax(it->first, adjustedStart + delta);
        if (it->first <= end) adjustedEnd = qMax(it->first, adjustedEnd + delta);
    }
    const auto range = numbered ? renumberList(adjustedStart, adjustedEnd)
        : QVariantMap{{QStringLiteral("start"), adjustedStart}, {QStringLiteral("end"), adjustedEnd}};
    cursor.endEditBlock();
    return range;
}
