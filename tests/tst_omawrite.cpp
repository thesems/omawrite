#include <QtTest>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickTextDocument>

#include "backend.h"
#include "explorer.h"
#include "markdownhighlighter.h"

class OmawriteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(m_settingsDirectory.isValid());
        QQuickStyle::setStyle(QStringLiteral("Material"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDirectory.path());
    }

    void countsWords() {
        QCOMPARE(Backend::countWords(QStringLiteral("one two-three don't 42")), 4);
        QCOMPARE(Backend::countWords(QStringLiteral("你好 世界")), 2);
        QCOMPARE(Backend::countWords(QString()), 0);
    }

    void normalizesLinks() {
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("www.example.com/path")),
                 QStringLiteral("https://www.example.com/path"));
        QCOMPARE(Backend::normalizedLinkUrl(QStringLiteral("mailto:writer@example.com")),
                 QStringLiteral("mailto:writer@example.com"));
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("example.com")).isEmpty());
        QVERIFY(Backend::normalizedLinkUrl(QStringLiteral("file:///tmp/private")).isEmpty());
    }

    void suggestsSafeNames() {
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("My first draft\nBody")),
                 QStringLiteral("My first draft.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("A/B")), QStringLiteral("A-B.md"));
        QCOMPARE(Backend::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
        QCOMPARE(Backend::suggestedFileName(QStringLiteral("Already.md")),
                 QStringLiteral("Already.md"));
    }

    void keepsNotoFromCrowdingTheFontList() {
        const QStringList fonts = Backend::selectableFontFamilies({
            QStringLiteral("Noto Sans Devanagari"), QStringLiteral("Liberation Serif"),
            QStringLiteral("Noto Serif"), QStringLiteral("Noto Sans Tamil UI"),
            QStringLiteral("adwaita Sans"), QStringLiteral("IBM Plex Mono"),
            QStringLiteral("Noto Sans"), QStringLiteral("Noto Sans Mono"),
            QStringLiteral("Liberation Serif"), QStringLiteral("Monospace"),
            QStringLiteral("Standard Symbols PS"), QStringLiteral("D050000L"),
            QStringLiteral("Noto Color Emoji"), QStringLiteral("Nimbus Sans [UKWN]"),
            QStringLiteral("Nimbus Sans [URW ]")});
        QCOMPARE(fonts, QStringList({QStringLiteral("IBM Plex Mono"),
                                     QStringLiteral("adwaita Sans"),
                                     QStringLiteral("Liberation Serif"),
                                     QStringLiteral("Nimbus Sans"),
                                     QStringLiteral("Noto Sans"),
                                     QStringLiteral("Noto Sans Mono"),
                                     QStringLiteral("Noto Serif")}));
    }

    void remembersEditorFont() {
        const QString installed = Backend().availableFonts().value(1);
        QVERIFY(!installed.isEmpty());

        {
            Backend backend;
            QCOMPARE(backend.editorFont(), Backend::defaultEditorFont());
            QSignalSpy fontSpy(&backend, &Backend::editorFontChanged);
            backend.setEditorFont(installed);
            QCOMPARE(fontSpy.count(), 1);
        }

        QCOMPARE(Backend().editorFont(), installed);

        // A remembered font that has since been uninstalled falls back.
        QSettings().setValue(QStringLiteral("editor/font"), QStringLiteral("No Such Font"));
        QCOMPARE(Backend().editorFont(), Backend::defaultEditorFont());
        QSettings().remove(QStringLiteral("editor/font"));
    }

    void findsInlineMarkdownRanges() {
        const auto markup = MarkdownHighlighter::inlineMarkup(
            QStringLiteral("**bold** and *italic* and [site](https://example.com)"));
        QCOMPARE(markup.size(), 3);
        QCOMPARE(markup.at(0).content.start, 2);
        QCOMPARE(markup.at(0).content.length, 4);
        QCOMPARE(markup.at(2).content.length, 4);
        QCOMPARE(markup.at(2).markers[0].length, 1);
    }

    void loadsCurrentOmarchyTheme() {
        QTemporaryDir homeDirectory;
        QVERIFY(homeDirectory.isValid());

        const QByteArray originalHome = qgetenv("HOME");
        struct HomeRestorer {
            QByteArray value;
            ~HomeRestorer() { qputenv("HOME", value); }
        } restoreHome{originalHome};
        QVERIFY(qputenv("HOME", homeDirectory.path().toUtf8()));

        const QString themeDirectory = homeDirectory.path()
            + QStringLiteral("/.local/state/omarchy/current/theme");
        QVERIFY(QDir().mkpath(themeDirectory));

        QFile colorsFile(themeDirectory + QStringLiteral("/colors.toml"));
        QVERIFY(colorsFile.open(QIODevice::WriteOnly | QIODevice::Text));
        const QByteArray palette(
            "mode = \"light\"\n"
            "accent = \"#112233\"\n"
            "selection = \"#445566\"\n"
            "background = \"#fefefe\"\n"
            "foreground = \"#101010\"\n");
        QCOMPARE(colorsFile.write(palette), qint64(palette.size()));
        colorsFile.close();

        Backend backend;
        QCOMPARE(backend.themeBackground(), QStringLiteral("#fefefe"));
        QCOMPARE(backend.themeForeground(), QStringLiteral("#101010"));
        QCOMPARE(backend.themeAccent(), QStringLiteral("#112233"));
        QCOMPARE(backend.themeSelection(), QStringLiteral("#445566"));
        QVERIFY(!backend.darkMode());
    }

    void ignoresFileWatcherEventsForSavedContents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("first-save.md"));
        Backend backend;
        QSignalSpy externalChangeSpy(&backend, &Backend::externalChangeDetected);

        backend.saveAs(QUrl::fromLocalFile(path));
        QVERIFY(QFileInfo::exists(path));

        QFile sameContents(path);
        QVERIFY(sameContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sameContents.close();
        QTest::qWait(100);
        QCOMPARE(externalChangeSpy.count(), 0);

        QFile changedContents(path);
        QVERIFY(changedContents.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(changedContents.write("changed elsewhere"), qint64(17));
        changedContents.close();
        QTRY_COMPARE(externalChangeSpy.count(), 1);
    }

    void keepsCursorAndSelectionStableAcrossInsertions() {
        const QString mutationsPath = QFINDTESTDATA("../src/EditorMutations.js");
        QVERIFY(!mutationsPath.isEmpty());

        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QByteArray harness = R"QML(
            import QtQuick
            import "EditorMutations.js" as EditorMutations

            TextEdit {
                property string insertionText
                property int insertionCursor
                property string wrappedText
                property int wrappedSelectionStart
                property int wrappedSelectionEnd

                Component.onCompleted: {
                    text = "alpha omega";
                    cursorPosition = 5;
                    EditorMutations.replaceRange(this, 5, 5, "one\r\ntwo");
                    insertionText = text;
                    insertionCursor = cursorPosition;

                    text = "alpha beta omega";
                    select(6, 10);
                    EditorMutations.replaceRange(this, selectionStart, selectionEnd,
                                                 "**beta**", 2, 6);
                    wrappedText = text;
                    wrappedSelectionStart = selectionStart;
                    wrappedSelectionEnd = selectionEnd;
                }
            }
        )QML";
        const QUrl harnessUrl = QUrl::fromLocalFile(
            QFileInfo(mutationsPath).absolutePath() + QStringLiteral("/MutationHarness.qml"));
        component.setData(harness, harnessUrl);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> editor(component.create());
        QVERIFY2(editor, qPrintable(component.errorString()));

        QCOMPARE(editor->property("insertionText").toString(),
                 QStringLiteral("alphaone\ntwo omega"));
        QCOMPARE(editor->property("insertionCursor").toInt(), 12);
        QCOMPARE(editor->property("wrappedText").toString(),
                 QStringLiteral("alpha **beta** omega"));
        QCOMPARE(editor->property("wrappedSelectionStart").toInt(), 8);
        QCOMPARE(editor->property("wrappedSelectionEnd").toInt(), 12);
    }

    void savesAndOpensFromFooterButtons() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        Explorer explorer;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("explorer"), &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QVERIFY(window->findChild<QObject *>(QStringLiteral("sourceEditor")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("renderedPreview")));
        QVERIFY(!window->findChild<QObject *>(QStringLiteral("modeToggle")));

        QObject *saveButton = window->findChild<QObject *>(QStringLiteral("saveButton"));
        QObject *openButton = window->findChild<QObject *>(QStringLiteral("openButton"));
        QVERIFY(saveButton);
        QVERIFY(openButton);

        QSignalSpy saveDialogSpy(&backend, &Backend::saveDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(saveButton, "clicked"));
        QCOMPARE(saveDialogSpy.count(), 1);

        QSignalSpy openDialogSpy(&backend, &Backend::openDialogRequested);
        QVERIFY(QMetaObject::invokeMethod(openButton, "clicked"));
        QCOMPARE(openDialogSpy.count(), 1);
    }

    void scalesTextWithDesktopTextSize() {
        const QString mainQmlPath = QFINDTESTDATA("../src/Main.qml");
        QVERIFY(!mainQmlPath.isEmpty());

        Backend backend;
        Explorer explorer;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        engine.rootContext()->setContextProperty(QStringLiteral("explorer"), &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(mainQmlPath));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));

        QObject *editor = window->findChild<QObject *>(QStringLiteral("sourceEditor"));
        QVERIFY(editor);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 20);

        // `omarchy display text size 16` sets the GNOME factor to 16/12.
        backend.setTextScale(16.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 27);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 27);

        backend.setTextScale(9.0 / 12.0);
        QCOMPARE(window->property("editorFontPixelSize").toInt(), 15);
        QCOMPARE(editor->property("font").value<QFont>().pixelSize(), 15);
    }

    void remembersLastSaveDirectory() {
        QTemporaryDir saveDirectory;
        QVERIFY(saveDirectory.isValid());

        const QString savedPath = saveDirectory.filePath(QStringLiteral("first.md"));
        Backend savedDocument;
        savedDocument.saveAs(QUrl::fromLocalFile(savedPath));

        Backend nextDocument;
        QSignalSpy saveDialogSpy(&nextDocument, &Backend::saveDialogRequested);
        nextDocument.saveAsDialog();
        QCOMPARE(saveDialogSpy.count(), 1);

        const QUrl suggestedUrl = saveDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).absolutePath(),
                 saveDirectory.path());
        QCOMPARE(QFileInfo(suggestedUrl.toLocalFile()).fileName(),
                 QStringLiteral("Untitled.md"));

        QSettings().setValue(QStringLiteral("file/lastSaveDirectory"),
                             saveDirectory.filePath(QStringLiteral("missing")));
        Backend fallbackDocument;
        QSignalSpy fallbackDialogSpy(&fallbackDocument, &Backend::saveDialogRequested);
        fallbackDocument.saveAsDialog();
        const QUrl fallbackUrl = fallbackDialogSpy.takeFirst().constFirst().toUrl();
        QCOMPARE(QFileInfo(fallbackUrl.toLocalFile()).absolutePath(), QDir::homePath());
    }

    void explorerProtectsExistingFilesAndCreatesNestedNotes() {
        QTemporaryDir workspace;
        QTemporaryDir outside;
        Explorer explorer;
        const QUrl root = QUrl::fromLocalFile(workspace.path());
        explorer.chooseFolder(root);
        const QUrl folder = explorer.createEntry(root, "Ideas", true);
        QVERIFY(QFileInfo(folder.toLocalFile()).isDir());
        const QUrl note = explorer.createEntry(folder, "hello.md", false);
        QVERIFY(QFileInfo::exists(note.toLocalFile()));
        QFile contents(note.toLocalFile());
        QVERIFY(contents.open(QIODevice::WriteOnly));
        contents.write("Keep my writing");
        contents.close();
        QVERIFY(explorer.createEntry(folder, "hello.md", false).isEmpty());
        QVERIFY(explorer.createEntry(root, "../escape.md", false).isEmpty());
        QVERIFY(explorer.createEntry(QUrl::fromLocalFile(outside.path()), "escape.md", false).isEmpty());
        QVERIFY(contents.open(QIODevice::ReadOnly));
        QCOMPARE(contents.readAll(), QByteArray("Keep my writing"));
        contents.close();
        QSignalSpy renameSpy(&explorer, &Explorer::entryRenamed);
        QVERIFY(explorer.renameEntry(note, "renamed.md"));
        QCOMPARE(renameSpy.count(), 1);
        QVERIFY(!QFileInfo::exists(note.toLocalFile()));
        QVERIFY(QFileInfo::exists(workspace.filePath("Ideas/renamed.md")));
        QVERIFY(!explorer.renameEntry(folder, "../bad"));
    }

    void explorerRequiresMdOrTxtForFiles() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        for (const QString name : {"fees", "fees.markdown", "fees.pdf", "fees.md.exe", "fees.txt."}) {
            QVERIFY(explorer.createEntry(explorer.folderUrl(), name, false).isEmpty());
            QVERIFY(!QFileInfo::exists(workspace.filePath(name)));
            QCOMPARE(explorer.error(), QString("File names must end in .md or .txt."));
        }
        const QUrl markdown = explorer.createEntry(explorer.folderUrl(), "fees.md", false);
        QVERIFY(!markdown.isEmpty());
        QVERIFY(!explorer.createEntry(explorer.folderUrl(), "fees.txt", false).isEmpty());
        QVERIFY(!explorer.createEntry(explorer.folderUrl(), "FEES.MD", false).isEmpty());
        QVERIFY(!explorer.renameEntry(markdown, "fees"));
        QVERIFY(QFileInfo::exists(markdown.toLocalFile()));
        QVERIFY(explorer.renameEntry(markdown, "renamed.txt"));
        QVERIFY(!explorer.createEntry(explorer.folderUrl(), "Folder", true).isEmpty());
    }

    void renamingFolderKeepsOpenDocumentSavePath() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        const QUrl folder = explorer.createEntry(explorer.folderUrl(), "Before", true);
        const QUrl note = explorer.createEntry(folder, "note.md", false);
        Backend backend;
        QObject::connect(&explorer, &Explorer::entryRenamed, &backend, &Backend::relocateFile);
        backend.open(note);
        QVERIFY(explorer.renameEntry(folder, "After"));
        QCOMPARE(backend.fileUrl().toLocalFile(), workspace.filePath("After/note.md"));
        backend.save();
        QVERIFY(QFileInfo::exists(backend.fileUrl().toLocalFile()));
        QVERIFY(!QFileInfo::exists(note.toLocalFile()));
    }

    void movesRefuseCollisionsAndFolderCycles() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        const QUrl folder = explorer.createEntry(explorer.folderUrl(), "Folder", true);
        const QUrl nested = explorer.createEntry(folder, "Nested", true);
        const QUrl note = explorer.createEntry(explorer.folderUrl(), "note.md", false);
        const QUrl duplicate = explorer.createEntry(folder, "note.md", false);
        QVERIFY(!explorer.moveEntry(note, folder));
        QVERIFY(QFileInfo::exists(note.toLocalFile()));
        QVERIFY(!explorer.moveEntry(folder, nested));
        QVERIFY(QFileInfo::exists(folder.toLocalFile()));
        QVERIFY(QFile::remove(duplicate.toLocalFile()));
        Backend backend;
        QObject::connect(&explorer, &Explorer::entryRenamed, &backend, &Backend::relocateFile);
        backend.open(note);
        QVERIFY(explorer.moveEntry(note, folder));
        QCOMPARE(backend.fileUrl().toLocalFile(), workspace.filePath("Folder/note.md"));
    }

    void dragsNoteOntoFolderInSidebar() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        const QUrl folder = explorer.createEntry(explorer.folderUrl(), "Destination", true);
        const QUrl note = explorer.createEntry(explorer.folderUrl(), "drag.md", false);
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("explorer", &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QQuickItem *source = nullptr;
        QQuickItem *destination = nullptr;
        QTRY_VERIFY((source = findVisualItem(window->contentItem(), "entry:drag.md")));
        QTRY_VERIFY((destination = findVisualItem(window->contentItem(), "entry:Destination")));
        const QPoint start = source->mapToScene(QPointF(source->width() / 2, source->height() / 2)).toPoint();
        const QPoint end = destination->mapToScene(QPointF(destination->width() / 2, destination->height() / 2)).toPoint();
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(window, start + QPoint(20, 0), 30);
        QTest::mouseMove(window, start + QPoint(30, 0), 30);
        QTest::mouseMove(window, end, 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end, 30);
        QTRY_VERIFY(QFileInfo::exists(workspace.filePath("Destination/drag.md")));
        QVERIFY(!QFileInfo::exists(note.toLocalFile()));
        QTRY_COMPARE(visibleEntryCount(window->contentItem(), "entry:drag.md"), 1);
        QTRY_VERIFY((source = findVisualItem(window->contentItem(), "entry:drag.md"))
            && source->property("entryUrl").toUrl().toLocalFile() == workspace.filePath("Destination/drag.md"));
        QQuickItem *rootTarget = findVisualItem(window->contentItem(), "rootDropTarget");
        QVERIFY(rootTarget);
        const QPoint fromFolder = source->mapToScene(QPointF(source->width() / 2, source->height() / 2)).toPoint();
        QVERIFY(!rootTarget->isVisible());
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, fromFolder);
        QTest::mouseMove(window, fromFolder + QPoint(20, 0), 30);
        QTest::mouseMove(window, fromFolder + QPoint(30, 0), 30);
        QTRY_VERIFY(rootTarget->isVisible());
        QTest::qWait(30); // Let the layout place the newly revealed drop target.
        const QPoint toRoot = rootTarget->mapToScene(QPointF(rootTarget->width() / 2, rootTarget->height() / 2)).toPoint();
        QTest::mouseMove(window, toRoot, 30);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, toRoot, 30);
        QTRY_VERIFY(QFileInfo::exists(note.toLocalFile()));
        QVERIFY(!QFileInfo::exists(workspace.filePath("Destination/drag.md")));
        QTRY_COMPARE(visibleEntryCount(window->contentItem(), "entry:drag.md"), 1);
        QTRY_VERIFY(!rootTarget->isVisible());
        QQuickItem *tree = findVisualItem(window->contentItem(), "explorerTree");
        QVERIFY(tree);
        const QPoint emptySpace = tree->mapToScene(QPointF(80, tree->height() - 20)).toPoint();
        QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, emptySpace);
        QObject *menu = window->findChild<QObject *>("explorerContextMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QObject *sidebar = window->findChild<QObject *>("explorerSidebar");
        QVERIFY(sidebar);
        QCOMPARE(sidebar->property("selectedUrl").toUrl(), explorer.folderUrl());
        QObject *deleteItem = window->findChild<QObject *>("deleteEntryMenuItem");
        QVERIFY(deleteItem);
        QVERIFY(!deleteItem->property("enabled").toBool());
    }

    void trashingOpenFileKeepsDraft_data() {
        QTest::addColumn<bool>("deleteFolder");
        QTest::newRow("file") << false;
        QTest::newRow("folder-with-open-file") << true;
    }

    void trashingOpenFileKeepsDraft() {
        QFETCH(bool, deleteFolder);
        QTemporaryDir workspace;
        QTemporaryDir trashData;
        const QByteArray previousData = qgetenv("XDG_DATA_HOME");
        struct RestoreDataHome {
            QByteArray value;
            ~RestoreDataHome() {
                if (value.isNull()) qunsetenv("XDG_DATA_HOME");
                else qputenv("XDG_DATA_HOME", value);
            }
        } restore{previousData};
        qputenv("XDG_DATA_HOME", trashData.path().toUtf8());
        const QString folderPath = workspace.filePath("Folder");
        QVERIFY(QDir().mkdir(folderPath));
        const QUrl note = QUrl::fromLocalFile(folderPath + "/note.md");
        QFile file(note.toLocalFile());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Keep this draft");
        file.close();
        Backend backend;
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick; TextEdit {}", QUrl());
        QScopedPointer<QObject> editor(component.create());
        QVERIFY(editor);
        backend.attachDocument(editor->property("textDocument").value<QQuickTextDocument *>());
        backend.open(note);
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        QObject::connect(&explorer, &Explorer::entryTrashed, &backend, &Backend::detachTrashedFile);
        QVERIFY(!explorer.trashEntry(explorer.folderUrl()));
        QVERIFY(explorer.trashEntry(deleteFolder ? QUrl::fromLocalFile(folderPath) : note));
        QVERIFY(!QFileInfo::exists(note.toLocalFile()));
        QCOMPARE(QFileInfo::exists(folderPath), !deleteFolder);
        QVERIFY(backend.fileUrl().isEmpty());
        QVERIFY(backend.modified());
        const QUrl restored = QUrl::fromLocalFile(workspace.filePath("restored.md"));
        backend.saveAs(restored);
        QFile result(restored.toLocalFile());
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), QByteArray("Keep this draft"));
    }

    void sidebarSwitchPreservesUnsavedChanges() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        const QUrl first = explorer.createEntry(explorer.folderUrl(), "first.md", false);
        const QUrl second = explorer.createEntry(explorer.folderUrl(), "second.md", false);
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("explorer", &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QScopedPointer<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        backend.open(first);
        QObject *editor = window->findChild<QObject *>("sourceEditor");
        QVERIFY(editor);
        editor->setProperty("text", "Unsaved writing");
        QVERIFY(backend.modified());
        QVERIFY(QMetaObject::invokeMethod(window.data(), "requestOpen", Q_ARG(QVariant, QVariant(second))));
        QCOMPARE(backend.fileUrl(), first);
        QCOMPARE(editor->property("text").toString(), QString("Unsaved writing"));
        QCOMPARE(window->property("pendingAction").toString(), QString("open"));
        backend.save();
        QCOMPARE(backend.fileUrl(), second);
        QFile saved(first.toLocalFile());
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("Unsaved writing"));
    }

    void switchingFilesPlacesCaretAtEndWithoutMovingCaretOnSave() {
        QTemporaryDir workspace;
        const QUrl first = QUrl::fromLocalFile(workspace.filePath("long.md"));
        const QUrl second = QUrl::fromLocalFile(workspace.filePath("short.md"));
        QFile longFile(first.toLocalFile());
        QVERIFY(longFile.open(QIODevice::WriteOnly));
        longFile.write(QString("A line of writing\n").repeated(200).toUtf8());
        longFile.close();
        QFile shortFile(second.toLocalFile());
        QVERIFY(shortFile.open(QIODevice::WriteOnly));
        shortFile.write("Short document");
        shortFile.close();
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("explorer", &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY2(window, qPrintable(component.errorString()));
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QObject *editor = window->findChild<QObject *>("sourceEditor");
        QObject *scroll = window->findChild<QObject *>("editorScrollArea");
        QVERIFY(editor);
        QVERIFY(scroll);
        backend.open(first);
        QTRY_COMPARE(editor->property("cursorPosition").toInt(), editor->property("text").toString().size());
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
        editor->setProperty("cursorPosition", 2000);
        QVERIFY(QMetaObject::invokeMethod(editor, "select", Q_ARG(int, 1900), Q_ARG(int, 2000)));
        QTRY_VERIFY(scroll->property("contentY").toReal() > 0);
        backend.open(second);
        QTRY_COMPARE(editor->property("cursorPosition").toInt(), 14);
        QCOMPARE(editor->property("selectionStart").toInt(), 14);
        QCOMPARE(editor->property("selectionEnd").toInt(), 14);
        QTRY_COMPARE(scroll->property("contentY").toReal(), 0.0);
        QVERIFY(editor->property("activeFocus").toBool());
        for (const char letter : QByteArray(" New")) QTest::keyClick(window, letter);
        QCOMPARE(editor->property("text").toString(), QString("Short document New"));
        QCOMPARE(editor->property("cursorPosition").toInt(), 18);
        backend.save();
        QCOMPARE(editor->property("cursorPosition").toInt(), 18);
        backend.saveAs(QUrl::fromLocalFile(workspace.filePath("renamed.md")));
        QCOMPARE(editor->property("cursorPosition").toInt(), 18);
        backend.open(QUrl::fromLocalFile(workspace.filePath("missing.md")));
        QCOMPARE(editor->property("cursorPosition").toInt(), 18);
    }

    void insertingFileKeepsEveryExplorerRowMappedToItsFile() {
        QTemporaryDir workspace;
        Explorer explorer;
        explorer.chooseFolder(QUrl::fromLocalFile(workspace.path()));
        const QUrl linter = explorer.createEntry(explorer.folderUrl(), "linter.md", false);
        QFile content(linter.toLocalFile());
        QVERIFY(content.open(QIODevice::WriteOnly));
        content.write("Linter document");
        content.close();
        Backend backend;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("explorer", &explorer);
        QQmlComponent component(&engine, QUrl::fromLocalFile(QFINDTESTDATA("../src/Main.qml")));
        QScopedPointer<QObject> object(component.create());
        auto *window = qobject_cast<QQuickWindow *>(object.data());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(findVisualItem(window->contentItem(), "entry:linter.md"));
        backend.open(linter);
        const QUrl hyperliquid = explorer.createEntry(explorer.folderUrl(), "hyperliquid.md", false);
        QVERIFY(QMetaObject::invokeMethod(window, "requestOpen", Q_ARG(QVariant, QVariant(hyperliquid))));
        QCOMPARE(backend.fileUrl(), hyperliquid);
        QQuickItem *newEntry = nullptr;
        QTRY_VERIFY((newEntry = findVisualItem(window->contentItem(), "entry:hyperliquid.md")));
        QTRY_COMPARE(newEntry->property("entryUrl").toUrl(), hyperliquid);
        QQuickItem *oldEntry = findVisualItem(window->contentItem(), "entry:linter.md");
        QVERIFY(oldEntry);
        QCOMPARE(oldEntry->property("entryUrl").toUrl(), linter);
        const QPoint point = newEntry->mapToScene(QPointF(80, newEntry->height() / 2)).toPoint();
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(backend.fileUrl(), hyperliquid);
        QObject *editor = window->findChild<QObject *>("sourceEditor");
        QVERIFY(editor);
        QCOMPARE(editor->property("text").toString(), QString());
        QVERIFY(newEntry->property("highlighted").toBool());
        QVERIFY(!oldEntry->property("highlighted").toBool());
    }

private:
    static int visibleEntryCount(QQuickItem *item, const QString &name) {
        int count = item->objectName() == name && item->isVisible() ? 1 : 0;
        for (QQuickItem *child : item->childItems()) count += visibleEntryCount(child, name);
        return count;
    }
    static QQuickItem *findVisualItem(QQuickItem *item, const QString &name) {
        if (item->objectName() == name) return item;
        for (QQuickItem *child : item->childItems())
            if (QQuickItem *found = findVisualItem(child, name)) return found;
        return nullptr;
    }
    QTemporaryDir m_settingsDirectory;
};

QTEST_MAIN(OmawriteTest)
#include "tst_omawrite.moc"
