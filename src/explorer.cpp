#include "explorer.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

Explorer::Explorer(QObject *parent) : QFileSystemModel(parent) {
    m_treeModel.setSourceModel(this);
    setReadOnly(true);
    setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    setNameFilters({QStringLiteral("*.md"), QStringLiteral("*.txt")});
    setNameFilterDisables(false);
    QString folder = QSettings().value(QStringLiteral("explorer/folder")).toString();
    if (!QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (!QFileInfo(folder).isDir())
        folder = QDir::homePath();
    chooseFolder(QUrl::fromLocalFile(folder));
}

QString Explorer::folderName() const {
    const QString name = QFileInfo(rootPath()).fileName();
    return name.isEmpty() ? rootPath() : name;
}

bool Explorer::fail(const QString &message) {
    m_error = message;
    emit errorChanged();
    return false;
}

bool Explorer::validName(const QString &name) {
    if (name.trimmed().isEmpty() || name == "." || name == ".." || name.contains('/')
        || name.contains('\\') || name.contains(QChar::Null))
        return fail(QStringLiteral("Enter a name without path separators."));
    m_error.clear();
    emit errorChanged();
    return true;
}

bool Explorer::insideRoot(const QString &path) const {
    const QString root = QFileInfo(rootPath()).canonicalFilePath();
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return !root.isEmpty() && !resolved.isEmpty()
        && (resolved == root || resolved.startsWith(root == "/" ? root : root + '/'));
}

bool Explorer::validFileName(const QString &name) {
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (suffix != "md" && suffix != "txt")
        return fail(QStringLiteral("File names must end in .md or .txt."));
    return true;
}

void Explorer::chooseFolder(const QUrl &url) {
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isDir()) {
        fail(QStringLiteral("Choose an existing local folder."));
        return;
    }
    setRootPath(QFileInfo(url.toLocalFile()).canonicalFilePath());
    QSettings().setValue(QStringLiteral("explorer/folder"), rootPath());
    m_error.clear();
    emit errorChanged();
    emit folderChanged();
}

QUrl Explorer::urlForIndex(const QModelIndex &index) const {
    const QModelIndex source = index.model() == &m_treeModel ? m_treeModel.mapToSource(index) : index;
    return source.isValid() ? QUrl::fromLocalFile(filePath(source)) : QUrl();
}
bool Explorer::directory(const QModelIndex &index) const {
    return isDir(index.model() == &m_treeModel ? m_treeModel.mapToSource(index) : index);
}
bool Explorer::isDirectory(const QString &path) const { return QFileInfo(path).isDir(); }
QString Explorer::nameForUrl(const QUrl &url) const { return QFileInfo(url.toLocalFile()).fileName(); }
QUrl Explorer::parentFolder(const QUrl &url) const {
    return QUrl::fromLocalFile(QFileInfo(url.toLocalFile()).absolutePath());
}

QUrl Explorer::createEntry(const QUrl &parent, const QString &name, bool folder) {
    if (!validName(name)) return {};
    if (!folder && !validFileName(name)) return {};
    if (!parent.isLocalFile() || !QFileInfo(parent.toLocalFile()).isDir()
        || !insideRoot(parent.toLocalFile())) {
        fail(QStringLiteral("Choose a folder inside the workspace."));
        return {};
    }
    const QString path = QDir(parent.toLocalFile()).filePath(name);
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        fail(QStringLiteral("That name already exists."));
        return {};
    }
    QFile file(path);
    const bool ok = folder ? QDir().mkdir(path) : file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    if (!ok) {
        fail(QStringLiteral("Could not create the entry. Check folder permissions."));
        return {};
    }
    return QUrl::fromLocalFile(path);
}

bool Explorer::renameEntry(const QUrl &url, const QString &name) {
    if (!validName(name)) return false;
    const QFileInfo info(url.toLocalFile());
    if (!info.isDir() && !validFileName(name)) return false;
    if (!url.isLocalFile() || !insideRoot(info.absolutePath()) || info.absoluteFilePath() == rootPath())
        return fail(QStringLiteral("Choose an entry inside the workspace."));
    const QString target = info.dir().filePath(name);
    if (target == info.absoluteFilePath()) return true;
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink())
        return fail(QStringLiteral("That name already exists."));
    if (!info.dir().rename(info.fileName(), name))
        return fail(QStringLiteral("Could not rename the entry. Check folder permissions."));
    emit entryRenamed(url, QUrl::fromLocalFile(target));
    return true;
}

bool Explorer::moveEntry(const QUrl &url, const QUrl &destinationFolder) {
    const QFileInfo source(url.toLocalFile());
    const QFileInfo destination(destinationFolder.toLocalFile());
    if (!url.isLocalFile() || !destinationFolder.isLocalFile()
        || !insideRoot(source.absolutePath()) || !insideRoot(destination.absoluteFilePath())
        || !destination.isDir() || source.absoluteFilePath() == rootPath())
        return fail(QStringLiteral("Move entries between folders inside the workspace."));
    const QString target = QDir(destination.absoluteFilePath()).filePath(source.fileName());
    if (target == source.absoluteFilePath()) return true;
    const QString resolvedSource = source.canonicalFilePath();
    const QString resolvedDestination = destination.canonicalFilePath();
    if (source.isDir() && (resolvedDestination == resolvedSource
        || resolvedDestination.startsWith(resolvedSource + '/')))
        return fail(QStringLiteral("A folder cannot be moved into itself."));
    if (QFileInfo::exists(target) || QFileInfo(target).isSymLink())
        return fail(QStringLiteral("The destination already contains that name."));
    if (!QDir().rename(source.absoluteFilePath(), target))
        return fail(QStringLiteral("Could not move the entry. Check folder permissions."));
    m_error.clear();
    emit errorChanged();
    emit entryRenamed(url, QUrl::fromLocalFile(target));
    return true;
}

bool Explorer::trashEntry(const QUrl &url) {
    const QFileInfo info(url.toLocalFile());
    if (!url.isLocalFile() || info.absoluteFilePath() == rootPath() || !insideRoot(info.absolutePath()))
        return fail(QStringLiteral("Choose a file or folder inside the workspace."));
    if (!QFile::moveToTrash(info.absoluteFilePath()))
        return fail(QStringLiteral("Could not move the entry to Trash."));
    m_error.clear();
    emit errorChanged();
    emit entryTrashed(url);
    return true;
}
