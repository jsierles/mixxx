#include "library/enginedjusblibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStorageInfo>
#include <QUuid>
#include <QDebug>

#include "moc_enginedjusblibrary.cpp"

namespace {
const QString kDatabase = QStringLiteral("Engine Library/Database2/m.db");

// Engine stores playlist and track order as linked lists, not row IDs. Reject
// broken chains instead of looping or silently presenting a different order.
struct Entry {
    qint64 id;
    qint64 next;
    QString title;
    QString artist;
    QString path;
    qint64 trackId = 0;
    QVariant bpm = {};
};
bool orderEntries(const QList<Entry>& entries, QList<Entry>* ordered) {
    QHash<qint64, Entry> remaining;
    QSet<qint64> successors;
    for (const auto& entry : entries) {
        if (entry.id <= 0 || remaining.contains(entry.id)) return false;
        remaining.insert(entry.id, entry);
        if (entry.next) successors.insert(entry.next);
    }
    qint64 head = 0;
    for (const auto& entry : entries) {
        if (!successors.contains(entry.id)) {
            if (head) return false;
            head = entry.id;
        }
    }
    while (head && remaining.contains(head)) {
        const auto entry = remaining.take(head);
        ordered->append(entry);
        head = entry.next;
    }
    return head == 0 && remaining.isEmpty();
}
QString containedFile(const QString& root, const QString& path) {
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    const QString prefix = root.endsWith('/') ? root : root + '/';
    return file.isFile() && file.isReadable() && canonical.startsWith(prefix)
            ? canonical : QString{};
}
} // namespace

EngineDjUsbLibrary::EngineDjUsbLibrary(QObject* parent) : QObject(parent) {
    m_navigation.appendRow(new QStandardItem(tr("Engine DJ")));
    m_tracks.setColumnCount(4);
    for (int column = 0; column < 4; ++column) {
        const auto name = QStringList{"title", "artist", "location", "bpm"}[column];
        m_tracks.setHeaderData(column, Qt::Horizontal, name, Qt::UserRole + 1);
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
            QStringLiteral("quietvinyl-engine-%1").arg(QUuid::createUuid().toString()));
    m_database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, &EngineDjUsbLibrary::scanMountedDevices);
}

EngineDjUsbLibrary::~EngineDjUsbLibrary() {
    closeSnapshot();
    const auto name = m_database.connectionName();
    m_database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(name);
}

void EngineDjUsbLibrary::startMonitoring() {
    scanMountedDevices();
    m_timer.start();
}

void EngineDjUsbLibrary::scanMountedDevices() {
    QStringList roots;
    for (const auto& volume : QStorageInfo::mountedVolumes()) {
        if (volume.isValid() && volume.isReady()) roots.append(volume.rootPath());
    }
    scanDevices(roots);
}

void EngineDjUsbLibrary::scanDevices(const QStringList& mountRoots) {
    QSet<QString> detected;
    for (const auto& root : mountRoots) {
        const QString canonical = QFileInfo(root).canonicalFilePath();
        if (canonical.isEmpty()) continue;
        if (!containedFile(canonical, QDir(canonical).filePath(kDatabase)).isEmpty() ||
                !containedFile(canonical, QDir(canonical).filePath(
                        QStringLiteral("Engine Library/m.db"))).isEmpty()) {
            detected.insert(canonical);
        }
    }
    auto* home = m_navigation.item(0);
    for (int row = home->rowCount() - 1; row >= 0; --row) {
        auto* item = home->child(row);
        const QString root = item->data(MountRole).toString();
        if (!detected.contains(root)) {
            if (root == m_mountRoot) closeSnapshot();
            home->removeRow(row);
        } else {
            detected.remove(root);
        }
    }
    auto newRoots = detected.values();
    newRoots.sort();
    for (const auto& root : newRoots) {
        const QStorageInfo volume(root);
        QString label = volume.name();
        if (label.isEmpty()) label = QFileInfo(root).fileName();
        auto* device = new QStandardItem(label.isEmpty() ? root : label);
        device->setData(root, MountRole);
        device->setData(qint64(0), PlaylistRole);
        home->appendRow(device);
    }
    home->setData(tr("Insert an Engine DJ USB drive"), StatusRole);
}

void EngineDjUsbLibrary::closeSnapshot() {
    m_tracks.removeRows(0, m_tracks.rowCount());
    m_database.close();
    m_snapshot.reset();
    m_mountRoot.clear();
}

bool EngineDjUsbLibrary::snapshot(const QString& mountRoot) {
    closeSnapshot();
    const QString source = containedFile(mountRoot, QDir(mountRoot).filePath(kDatabase));
    if (source.isEmpty()) return false;
    auto copy = std::make_unique<QTemporaryDir>();
    if (!copy->isValid() || !QFile::copy(source, copy->filePath("m.db"))) return false;
    // Include uncheckpointed data. Never open the original with SQLite, even
    // read-only: a WAL reader may create a shared-memory file beside it.
    for (const auto& suffix : {QStringLiteral("-wal"), QStringLiteral("-journal")}) {
        if (QFileInfo::exists(source + suffix) &&
                !QFile::copy(source + suffix, copy->filePath("m.db") + suffix)) return false;
    }
    m_database.setDatabaseName(copy->filePath("m.db"));
    if (!m_database.open()) return false;
    m_snapshot = std::move(copy);
    m_mountRoot = mountRoot;
    // Validate just the columns this reader uses; ignore unrelated additions
    // in later Engine versions. Legacy Engine Prime schemas are unsupported.
    QSqlQuery query(m_database);
    for (const auto& sql : {
                 "SELECT id, title, parentListId, nextListId FROM Playlist LIMIT 0",
                 "SELECT id, listId, trackId, databaseUuid, nextEntityId FROM PlaylistEntity LIMIT 0",
                 "SELECT id, title, artist, path, originDatabaseUuid, originTrackId FROM Track LIMIT 0",
                 "SELECT uuid FROM Information LIMIT 0"}) {
        if (!query.exec(QString::fromLatin1(sql))) return false;
    }
    return true;
}

void EngineDjUsbLibrary::activate(const QModelIndex& index) {
    if (index.model() != &m_navigation || !index.isValid()) return;
    m_tracks.removeRows(0, m_tracks.rowCount());
    auto* item = m_navigation.itemFromIndex(index);
    const QString root = item->data(MountRole).toString();
    if (root.isEmpty()) return; // Engine DJ home
    const qint64 playlist = item->data(PlaylistRole).toLongLong();
    item->removeRows(0, item->rowCount());
    auto fail = [this, item](const QString& message) {
        m_tracks.removeRows(0, m_tracks.rowCount());
        item->removeRows(0, item->rowCount());
        item->setData(message, StatusRole);
        qWarning() << "Engine DJ USB:" << message << m_database.lastError().text();
    };
    if (containedFile(root, QDir(root).filePath(kDatabase)).isEmpty()) {
        fail(tr("Engine DJ database unavailable or legacy format unsupported"));
        return;
    }
    if ((!playlist || root != m_mountRoot) && !snapshot(root)) {
        fail(tr("Cannot read this Engine DJ database format"));
        closeSnapshot();
        return;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT id, nextListId, title FROM Playlist "
                                 "WHERE parentListId = ? ORDER BY id"));
    query.addBindValue(playlist);
    if (!query.exec()) { fail(tr("Cannot read Engine DJ playlists")); return; }
    QList<Entry> entries, children;
    while (query.next()) entries.append({query.value(0).toLongLong(),
            query.value(1).toLongLong(), query.value(2).toString(), {}, {}});
    if (!orderEntries(entries, &children)) {
        fail(tr("Engine DJ playlist order is damaged")); return;
    }
    for (const auto& child : children) {
        // A corrupt parent cycle must not permit an endlessly deep browser.
        for (auto ancestor = index; ancestor.isValid(); ancestor = ancestor.parent()) {
            if (ancestor.data(PlaylistRole).toLongLong() == child.id) {
                fail(tr("Engine DJ playlist hierarchy is damaged")); return;
            }
        }
        auto* node = new QStandardItem(child.title.isEmpty() ? tr("Untitled playlist") : child.title);
        node->setData(root, MountRole);
        node->setData(child.id, PlaylistRole);
        item->appendRow(node);
    }
    if (!playlist) {
        item->setData(tr("No playlists on this Engine DJ drive"), StatusRole);
        return;
    }
    // Exported playlists can retain the desktop library's UUID/track ID even
    // when the USB assigns new local IDs. Resolve that original identity pair;
    // an unrelated device's same-numbered track must remain unavailable.
    const auto trackColumns = m_database.record("Track");
    const bool hasBpm = trackColumns.indexOf("bpm") >= 0;
    const bool hasAnalyzedBpm = trackColumns.indexOf("bpmAnalyzed") >= 0;
    const QString bpmColumn = hasBpm && hasAnalyzedBpm
            ? QStringLiteral("COALESCE(NULLIF(t.bpm, 0), t.bpmAnalyzed)")
            : hasBpm ? QStringLiteral("t.bpm")
                     : hasAnalyzedBpm ? QStringLiteral("t.bpmAnalyzed") : QStringLiteral("NULL");
    query.prepare(QStringLiteral(
            "SELECT e.id, e.nextEntityId, t.title, t.artist, t.path, t.id, %1 "
            "FROM PlaylistEntity e LEFT JOIN Track t ON "
            "(t.id = e.trackId AND (e.databaseUuid IS NULL OR e.databaseUuid = '' OR "
            "e.databaseUuid = (SELECT uuid FROM Information LIMIT 1))) OR "
            "(t.originTrackId = e.trackId AND t.originDatabaseUuid = e.databaseUuid "
            "AND e.databaseUuid <> (SELECT uuid FROM Information LIMIT 1)) "
            "WHERE e.listId = ? ORDER BY e.id").arg(bpmColumn));
    query.addBindValue(playlist);
    if (!query.exec()) { fail(tr("Cannot read Engine DJ playlist tracks")); return; }
    entries.clear();
    QList<Entry> ordered;
    while (query.next()) entries.append({query.value(0).toLongLong(),
            query.value(1).toLongLong(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toLongLong(), query.value(6)});
    if (!orderEntries(entries, &ordered)) {
        fail(tr("Engine DJ track order is damaged")); return;
    }
    for (const auto& track : ordered) {
        QString title = track.title;
        if (title.isEmpty()) title = QFileInfo(track.path).fileName();
        if (title.isEmpty()) title = tr("Unavailable track");
        const QString location = resolvePath(track.path);
        QString subtitle = track.artist;
        if (location.isEmpty()) subtitle += tr(" · Unavailable on USB");
        auto* name = new QStandardItem(title);
        name->setData(track.path, LocationRole);
        name->setData(track.trackId, TrackIdRole);
        auto* bpm = new QStandardItem;
        bpm->setData(track.bpm, Qt::DisplayRole);
        m_tracks.appendRow({name, new QStandardItem(subtitle), new QStandardItem(location), bpm});
    }
    item->setData(tr("Empty Engine DJ playlist"), StatusRole);
}

QString EngineDjUsbLibrary::resolvePath(const QString& path) const {
    if (path.isEmpty() || m_mountRoot.isEmpty()) return {};
    // Track.path is relative to Engine Library, even with Database2 storage.
    // ../Music/... is normal; canonical containment also rejects symlink escapes.
    if (QDir::isAbsolutePath(path)) return {};
    return containedFile(m_mountRoot,
            QDir(m_mountRoot + QStringLiteral("/Engine Library")).filePath(path));
}

QString EngineDjUsbLibrary::trackLocation(const QModelIndex& index) const {
    if (!index.isValid() || index.model() != &m_tracks ||
            containedFile(m_mountRoot, QDir(m_mountRoot).filePath(kDatabase)).isEmpty()) return {};
    return resolvePath(index.sibling(index.row(), 0).data(LocationRole).toString());
}

EngineDjCueData EngineDjUsbLibrary::trackCues(const QModelIndex& index) const {
    if (trackLocation(index).isEmpty()) return {};
    const qint64 id = index.sibling(index.row(), 0).data(TrackIdRole).toLongLong();
    if (id <= 0) return {};
    // Engine 2 stores these columns on Track; Engine 3 moved them into
    // PerformanceData. Both queries use the resolved local USB identity.
    const bool separate = m_database.record("PerformanceData").indexOf("quickCues") >= 0;
    QSqlQuery query(m_database);
    query.prepare(separate
            ? "SELECT quickCues, trackData FROM PerformanceData WHERE trackId = ?"
            : "SELECT quickCues, trackData FROM Track WHERE id = ?");
    query.addBindValue(id);
    if (!query.exec() || !query.next()) return {};
    return decodeEngineDjCues(query.value(0).toByteArray(), query.value(1).toByteArray());
}
