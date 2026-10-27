#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include "library/enginedjcueparser.h"

// Read-only browsing of modern Engine DJ USB libraries. SQLite only opens a
// private local snapshot, so even WAL/shared-memory files stay off the USB.
class EngineDjUsbLibrary : public QObject {
    Q_OBJECT
  public:
    enum Role { MountRole = Qt::UserRole + 100, PlaylistRole, StatusRole, LocationRole, TrackIdRole };
    explicit EngineDjUsbLibrary(QObject* parent = nullptr);
    ~EngineDjUsbLibrary() override;
    QStandardItemModel* navigation() { return &m_navigation; }
    QStandardItemModel* tracks() { return &m_tracks; }
    void startMonitoring();
    void scanDevices(const QStringList& mountRoots);
    void activate(const QModelIndex& index);
    QString trackLocation(const QModelIndex& index) const;
    EngineDjCueData trackCues(const QModelIndex& index) const;

  private:
    void scanMountedDevices();
    bool snapshot(const QString& mountRoot);
    void closeSnapshot();
    QString resolvePath(const QString& path) const;
    QStandardItemModel m_navigation;
    QStandardItemModel m_tracks;
    QTimer m_timer;
    QSqlDatabase m_database;
    std::unique_ptr<QTemporaryDir> m_snapshot;
    QString m_mountRoot;
};
