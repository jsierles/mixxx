#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStandardItemModel>
#include <QTimer>
#include <QQueue>
#include <QJsonArray>
#include <functional>

// A cancellable subprocess keeps account/network/decoding work off the UI and
// audio threads. Only complete, verified local files enter Mixxx's load path.
class TidalPlaylistLibrary : public QObject {
    Q_OBJECT
  public:
    enum Role { PlaylistRole = Qt::UserRole + 300, ActionRole, StatusRole, TrackIdRole, AvailableRole, CacheStateRole, ProgressRole, DetailRole };
    TidalPlaylistLibrary(QString program, QStringList helperArguments,
            QString profile, QString cache, QObject* parent = nullptr);
    ~TidalPlaylistLibrary() override;
    QStandardItemModel* navigation() { return &m_navigation; }
    QStandardItemModel* tracks() { return &m_tracks; }
    bool isAction(const QModelIndex& index) const { return index.data(ActionRole).toBool(); }
    bool canPrepare(const QModelIndex& index) const;
    void activate(const QModelIndex& index);
    void prepare(const QModelIndex& index, const QString& group);
    void cancel(); // Cancels navigation/load intent, not background downloads.
    void download(const QModelIndex& index);
    bool cached(const QModelIndex& index) const;
    QJsonArray downloads() const;
    bool preparing() const { return m_loadProcess; }
    QString status() const { return m_status; }

  signals:
    void statusChanged(const QString& text);
    void prepared(const QString& path, const QString& group);
    void preparationFailed();

  private:
    void start(const QString& operation, const QString& identity = {}, const QString& group = {});
    void consume(QProcess* process);
    void applyResult();
    void fail(const QString& message);
    void setStatus(const QString& message);
    void addConnect();
    void updatePlaylists(const QJsonArray& items);
    void readPlaylistCache();
    void savePlaylistCache() const;
    QJsonArray m_playlists;
    bool m_hasPlaylists = false;
    struct Transfer { QString state, detail; int progress = -1; };
    QHash<QString, Transfer> m_transfers;
    QQueue<QString> m_queue;
    QString m_downloadId;
    QPointer<QProcess> m_downloadProcess, m_loadProcess;
    void updateTransfer(const QString& id, const QString& state, int progress = -1, const QString& detail = {});
    void nextDownload();
    QProcess* runTransfer(const QString& operation, const QString& id,
            std::function<void(const QJsonObject&)> progress,
            std::function<void(const QJsonObject&, const QString&)> finished);
    QString verifiedPath(const QJsonObject& result, const QString& id) const;
    static QString errorText(const QString& code);
    static void stopProcess(QProcess* process);
    QStandardItem* root() const { return m_navigation.item(0); }
    QString m_program, m_profile, m_cache, m_operation, m_identity, m_group, m_status;
    QStringList m_helperArguments;
    QStandardItemModel m_navigation, m_tracks;
    QPointer<QProcess> m_process;
    QPersistentModelIndex m_activeNode;
    QTimer m_timeout;
    QByteArray m_output;
    QJsonObject m_result;
    bool m_done = false;
};
