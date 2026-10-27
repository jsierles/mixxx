#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QTimer>

class EngineDjUsbLibrary;
class TidalPlaylistLibrary;

// A one-level browser over Mixxx's existing sidebar and active track models.
// Navigation is independent of keyboard focus and never loads a folder's stale track.
class SimpleLibraryModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role { SubtitleRole = Qt::UserRole + 10, NodeRole, BpmRole, LocationRole, ArtworkRole, CacheStateRole, DownloadProgressRole, DownloadDetailRole, TidalIdRole };
    explicit SimpleLibraryModel(QObject* parent = nullptr);
    void setNavigationModel(QAbstractItemModel* model);
    void setTrackModel(QAbstractItemModel* model, const QHash<QString, int>& columns = {});
    void setEngineLibrary(EngineDjUsbLibrary* library);
    void setTidalLibrary(TidalPlaylistLibrary* library);
    QModelIndex tidalSelection() const;
    bool isTidalContext() const;
    bool canLoadSelected() const;
    bool canGoBack() const { return m_levels.size() > 1; }
    QString emptyText() const;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    int selectedRow() const { return m_currentRow; }
    QString breadcrumb() const;
    void selectRow(int row);
    void moveSelection(int steps);
    void moveTrackSelection(int steps);
    void enter(const QString& group = QStringLiteral("[Channel1]"));
    void back();
    void loadSelected(const QString& group, bool play);

  signals:
    void activateNode(const QModelIndex& index);
    void loadTrack(const QModelIndex& index, const QString& group, bool play);
    void selectionChanged(int row);
    void breadcrumbChanged(const QString& text);

  private:
    struct Level {
        QPersistentModelIndex node;
        QString title;
        QPersistentModelIndex selection;
        bool selectedTrack = false;
        int selectedRow = 0;
    };
    struct Folder { QPersistentModelIndex index; QString title; };
    void refresh();
    void watchModel(QAbstractItemModel* model, QList<QMetaObject::Connection>* connections);
    void clearTracks();
    int trackColumn(const QString& name) const;
    QPersistentModelIndex sourceAt(int row) const;

    QPointer<EngineDjUsbLibrary> m_engineLibrary;
    QPointer<TidalPlaylistLibrary> m_tidalLibrary;
    QList<QMetaObject::Connection> m_tidalConnections;
    QList<QMetaObject::Connection> m_engineConnections;
    QPointer<QAbstractItemModel> m_navigation;
    QPointer<QAbstractItemModel> m_tracks;
    QHash<QString, int> m_trackColumns;
    QList<QMetaObject::Connection> m_trackConnections;
    QList<QMetaObject::Connection> m_navigationConnections;
    QList<Level> m_levels;
    QList<Folder> m_folders;
    QTimer m_refreshTimer;
    int m_currentRow = -1;
};
