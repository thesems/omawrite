#pragma once

#include <QFileSystemModel>
#include <QSortFilterProxyModel>
#include <QUrl>

class ExplorerColumns : public QSortFilterProxyModel {
protected:
    bool filterAcceptsColumn(int column, const QModelIndex &) const override { return column == 0; }
};

class Explorer : public QFileSystemModel {
    Q_OBJECT
    Q_PROPERTY(QModelIndex folderIndex READ folderIndex NOTIFY folderChanged)
    Q_PROPERTY(QUrl folderUrl READ folderUrl NOTIFY folderChanged)
    Q_PROPERTY(QString folderName READ folderName NOTIFY folderChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QAbstractItemModel *treeModel READ treeModel CONSTANT)
public:
    explicit Explorer(QObject *parent = nullptr);
    QAbstractItemModel *treeModel() { return &m_treeModel; }
    QModelIndex folderIndex() const { return m_treeModel.mapFromSource(index(rootPath())); }
    QUrl folderUrl() const { return QUrl::fromLocalFile(rootPath()); }
    QString folderName() const;
    QString error() const { return m_error; }
    Q_INVOKABLE void chooseFolder(const QUrl &url);
    Q_INVOKABLE QUrl urlForIndex(const QModelIndex &index) const;
    Q_INVOKABLE bool directory(const QModelIndex &index) const;
    Q_INVOKABLE QUrl urlForPath(const QString &path) const { return QUrl::fromLocalFile(path); }
    Q_INVOKABLE bool isDirectory(const QString &path) const;
    Q_INVOKABLE QString nameForUrl(const QUrl &url) const;
    Q_INVOKABLE QUrl parentFolder(const QUrl &url) const;
    Q_INVOKABLE QUrl createEntry(const QUrl &parent, const QString &name, bool folder);
    Q_INVOKABLE bool renameEntry(const QUrl &url, const QString &name);
    Q_INVOKABLE bool moveEntry(const QUrl &url, const QUrl &destinationFolder);
    Q_INVOKABLE bool trashEntry(const QUrl &url);
signals:
    void folderChanged();
    void errorChanged();
    void entryRenamed(const QUrl &oldUrl, const QUrl &newUrl);
    void entryTrashed(const QUrl &url);
private:
    bool fail(const QString &message);
    bool validName(const QString &name);
    bool validFileName(const QString &name);
    bool insideRoot(const QString &path) const;
    QString m_error;
    ExplorerColumns m_treeModel;
};
