#include "library/simplelibrarymodel.h"

#include <QFileInfo>
#include <algorithm>
#include "util/fpclassify.h"
#include "library/enginedjusblibrary.h"
#include "library/tidalplaylistlibrary.h"

#include "moc_simplelibrarymodel.cpp"

SimpleLibraryModel::SimpleLibraryModel(QObject* parent) : QAbstractListModel(parent) {
    m_levels.append(Level{});
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, &SimpleLibraryModel::refresh);
}

void SimpleLibraryModel::watchModel(QAbstractItemModel* model,
        QList<QMetaObject::Connection>* connections) {
    auto changed = [this] { m_refreshTimer.start(0); };
    connections->append(connect(model, &QAbstractItemModel::rowsInserted, this, changed));
    connections->append(connect(model, &QAbstractItemModel::rowsRemoved, this, changed));
    connections->append(connect(model, &QAbstractItemModel::rowsMoved, this, changed));
    connections->append(connect(model, &QAbstractItemModel::modelReset, this, changed));
    connections->append(connect(model, &QAbstractItemModel::layoutChanged, this, changed));
    connections->append(connect(model, &QAbstractItemModel::dataChanged, this,
            [this, model](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                // Status text is read directly by the widget; it does not change
                // playlist rows or justify resetting selection/scroll remotely.
                if (m_tidalLibrary && model == m_tidalLibrary->navigation() &&
                        roles == QList<int>{TidalPlaylistLibrary::StatusRole}) return;
                if (model != m_tracks) { m_refreshTimer.start(0); return; }
                const bool statusOnly = !roles.isEmpty() && std::all_of(roles.cbegin(), roles.cend(), [](int role) {
                    return role >= TidalPlaylistLibrary::CacheStateRole && role <= TidalPlaylistLibrary::DetailRole;
                });
                emit dataChanged(index(m_folders.size() + first.row(), 0), index(m_folders.size() + last.row(), 0),
                        statusOnly ? QList<int>{CacheStateRole, DownloadProgressRole, DownloadDetailRole} : QList<int>{});
            }));
    connections->append(connect(model, &QObject::destroyed, this, changed));
}

void SimpleLibraryModel::setNavigationModel(QAbstractItemModel* model) {
    for (const auto& connection : std::as_const(m_navigationConnections)) disconnect(connection);
    m_navigationConnections.clear();
    clearTracks();
    m_levels = {Level{}};
    m_navigation = model;
    if (model) watchModel(model, &m_navigationConnections);
    refresh();
}

void SimpleLibraryModel::setEngineLibrary(EngineDjUsbLibrary* library) {
    for (const auto& connection : std::as_const(m_engineConnections)) disconnect(connection);
    m_engineConnections.clear();
    clearTracks();
    m_levels = {Level{}};
    m_engineLibrary = library;
    if (library) watchModel(library->navigation(), &m_engineConnections);
    refresh();
}

QString SimpleLibraryModel::emptyText() const {
    const QString status = m_levels.last().node.data(EngineDjUsbLibrary::StatusRole).toString();
    const QString tidal = m_levels.last().node.data(TidalPlaylistLibrary::StatusRole).toString();
    return !status.isEmpty() ? status : !tidal.isEmpty() ? tidal : tr("Empty");
}

void SimpleLibraryModel::setTidalLibrary(TidalPlaylistLibrary* library) {
    for (const auto& connection : std::as_const(m_tidalConnections)) disconnect(connection);
    m_tidalConnections.clear();
    m_tidalLibrary = library;
    if (library) watchModel(library->navigation(), &m_tidalConnections);
    refresh();
}

QModelIndex SimpleLibraryModel::tidalSelection() const {
    const auto source = sourceAt(m_currentRow);
    return m_tidalLibrary && source.model() == m_tidalLibrary->tracks() ? QModelIndex(source) : QModelIndex{};
}

bool SimpleLibraryModel::isTidalContext() const {
    return m_tidalLibrary && m_levels.last().node.model() == m_tidalLibrary->navigation();
}

void SimpleLibraryModel::clearTracks() {
    for (const auto& connection : std::as_const(m_trackConnections)) disconnect(connection);
    m_trackConnections.clear();
    m_tracks = nullptr;
    m_trackColumns.clear();
}

void SimpleLibraryModel::setTrackModel(QAbstractItemModel* model, const QHash<QString, int>& columns) {
    // Feature home pages are categories, not track lists. Also ignore startup
    // activation of Mixxx's default feature while this browser is at its root.
    if (m_levels.size() < 3 || m_tracks == model) return;
    if (m_engineLibrary) {
        const bool engineNode = m_levels.last().node.model() == m_engineLibrary->navigation();
        if (engineNode != (model == m_engineLibrary->tracks())) return;
    }
    if (m_tidalLibrary) {
        const bool tidalNode = m_levels.last().node.model() == m_tidalLibrary->navigation();
        if (tidalNode != (model == m_tidalLibrary->tracks())) return;
    }
    clearTracks();
    m_tracks = model;
    m_trackColumns = columns;
    if (model) watchModel(model, &m_trackConnections);
    refresh();
}

int SimpleLibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_folders.size() + (m_tracks ? m_tracks->rowCount() : 0);
}

int SimpleLibraryModel::trackColumn(const QString& name) const {
    if (!m_tracks) return -1;
    if (m_trackColumns.contains(name)) return m_trackColumns.value(name);
    // TrackModel::kHeaderNameRole, shared by library and filesystem models.
    for (int col = 0; col < m_tracks->columnCount(); ++col) {
        if (m_tracks->headerData(col, Qt::Horizontal, Qt::UserRole + 1).toString() == name) return col;
    }
    return -1;
}

QVariant SimpleLibraryModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (!index.isValid() || row < 0 || row >= rowCount()) return {};
    const bool folder = row < m_folders.size();
    if (role == NodeRole) return folder;
    if (folder) {
        if (role == Qt::DisplayRole) return m_folders[row].title;
        return {};
    }
    const int trackRow = row - m_folders.size();
    auto value = [this, trackRow](const QString& name) {
        const int column = trackColumn(name);
        return column < 0 ? QString{} : m_tracks->index(trackRow, column).data().toString();
    };
    if (m_tidalLibrary && m_tracks == m_tidalLibrary->tracks()) {
        const auto source = m_tracks->index(trackRow, 0);
        if (role == CacheStateRole) return source.data(TidalPlaylistLibrary::CacheStateRole);
        if (role == DownloadProgressRole) return source.data(TidalPlaylistLibrary::ProgressRole);
        if (role == DownloadDetailRole || role == Qt::ToolTipRole) return source.data(TidalPlaylistLibrary::DetailRole);
        if (role == TidalIdRole) return source.data(TidalPlaylistLibrary::TrackIdRole);
    }
    if (role == ArtworkRole) return value(QStringLiteral("coverurl"));
    if (role == LocationRole) return value(QStringLiteral("location"));
    if (role == BpmRole) {
        bool valid = false;
        const double bpm = value(QStringLiteral("bpm")).toDouble(&valid);
        return valid && util_isfinite(bpm) && bpm > 0 ? QVariant(bpm) : QVariant{};
    }
    if (role == SubtitleRole) {
        if (m_tidalLibrary && m_tracks == m_tidalLibrary->tracks() &&
                m_tracks->index(trackRow, 0).data(TidalPlaylistLibrary::CacheStateRole).toString() == "failed")
            return QString(value(QStringLiteral("artist")) + QStringLiteral(" · ") + m_tracks->index(trackRow, 0).data(TidalPlaylistLibrary::DetailRole).toString());
        if (m_tidalLibrary && m_tracks == m_tidalLibrary->tracks() &&
                !m_tracks->index(trackRow, 0).data(TidalPlaylistLibrary::AvailableRole).toBool())
            return tr("Unavailable on Tidal");
        return value(QStringLiteral("artist"));
    }
    if (role == Qt::DisplayRole) {
        QString title = value(QStringLiteral("title"));
        if (title.isEmpty()) title = value(QStringLiteral("filename"));
        if (title.isEmpty()) title = QFileInfo(value(QStringLiteral("location"))).fileName();
        if (title.isEmpty()) title = tr("Untitled track");
        return title;
    }
    return {};
}

QString SimpleLibraryModel::breadcrumb() const {
    QStringList parts;
    for (int i = 1; i < m_levels.size(); ++i) parts.append(m_levels[i].title);
    return parts.isEmpty() ? tr("Music") : parts.join(QStringLiteral(" / "));
}

QPersistentModelIndex SimpleLibraryModel::sourceAt(int row) const {
    if (row < 0 || row >= rowCount()) return {};
    if (row < m_folders.size()) return m_folders[row].index;
    return m_tracks->index(row - m_folders.size(), 0);
}

void SimpleLibraryModel::selectRow(int row) {
    if (row < 0 || row >= rowCount()) return;
    m_currentRow = row;
    auto& level = m_levels.last();
    level.selectedRow = row;
    level.selection = sourceAt(row);
    level.selectedTrack = row >= m_folders.size();
    emit selectionChanged(row);
}

void SimpleLibraryModel::moveSelection(int steps) {
    if (!steps || !rowCount()) return;
    const qint64 next = qint64(std::max(m_currentRow, 0)) + steps;
    selectRow(int(std::clamp(next, qint64(0), qint64(rowCount() - 1))));
}

void SimpleLibraryModel::moveTrackSelection(int steps) {
    const int firstTrack = static_cast<int>(m_folders.size());
    if (!steps || m_currentRow < firstTrack || rowCount() <= firstTrack) return;
    const qint64 next = qint64(m_currentRow) + steps;
    selectRow(int(std::clamp(next, qint64(firstTrack), qint64(rowCount() - 1))));
}

void SimpleLibraryModel::enter(const QString& group) {
    if (m_currentRow < 0 || m_currentRow >= rowCount()) return;
    if (m_currentRow >= m_folders.size()) {
        loadSelected(group, false);
        return;
    }
    const auto folder = m_folders[m_currentRow];
    if (!folder.index.isValid()) return;
    // Capture the highlighted parent even if it was selected automatically.
    selectRow(m_currentRow);
    if (m_tidalLibrary && folder.index.model() == m_tidalLibrary->navigation() &&
            m_tidalLibrary->isAction(folder.index)) {
        m_tidalLibrary->activate(folder.index);
        return;
    }
    clearTracks();
    Level level;
    level.node = folder.index;
    level.title = folder.title;
    m_levels.append(level);
    refresh();
    if (m_tidalLibrary && folder.index.model() == m_tidalLibrary->navigation()) {
        m_tidalLibrary->activate(folder.index);
        setTrackModel(m_tidalLibrary->tracks());
    } else if (m_engineLibrary && folder.index.model() == m_engineLibrary->navigation()) {
        m_engineLibrary->activate(folder.index);
        setTrackModel(m_engineLibrary->tracks());
    } else {
        emit activateNode(folder.index);
    }
    m_refreshTimer.start(0);
}

void SimpleLibraryModel::back() {
    if (m_levels.size() <= 1) return;
    if (m_tidalLibrary && m_levels.last().node.model() == m_tidalLibrary->navigation()) m_tidalLibrary->cancel();
    m_levels.removeLast();
    clearTracks();
    refresh();
    const auto node = m_levels.last().node;
    if (node.isValid()) {
        if (m_tidalLibrary && node.model() == m_tidalLibrary->navigation()) {
            m_tidalLibrary->activate(node);
            setTrackModel(m_tidalLibrary->tracks());
        } else if (m_engineLibrary && node.model() == m_engineLibrary->navigation()) {
            m_engineLibrary->activate(node);
            setTrackModel(m_engineLibrary->tracks());
        } else {
            emit activateNode(node);
        }
    }
    m_refreshTimer.start(0);
}

bool SimpleLibraryModel::canLoadSelected() const {
    if (m_currentRow < m_folders.size()) return false;
    const auto source = sourceAt(m_currentRow);
    if (!source.isValid() || source.model() != m_tracks) return false;
    if (m_tidalLibrary && source.model() == m_tidalLibrary->tracks()) return m_tidalLibrary->canPrepare(source);
    return !m_engineLibrary || source.model() != m_engineLibrary->tracks() ||
            !m_engineLibrary->trackLocation(source).isEmpty();
}

void SimpleLibraryModel::loadSelected(const QString& group, bool play) {
    if (!canLoadSelected()) return;
    const auto source = sourceAt(m_currentRow);
    if (m_tidalLibrary && source.model() == m_tidalLibrary->tracks()) {
        m_tidalLibrary->prepare(source, group);
        return;
    }
    if (source.isValid() && source.model() == m_tracks) emit loadTrack(source, group, play);
}

void SimpleLibraryModel::refresh() {
    m_refreshTimer.stop();
    while (m_levels.size() > 1 && !m_levels.last().node.isValid()) {
        m_levels.removeLast();
        clearTracks();
    }
    beginResetModel();
    m_folders.clear();
    const auto parent = m_levels.last().node;
    const auto* navigation = parent.isValid() ? parent.model() : m_navigation.data();
    if (navigation) {
        for (int row = 0; row < navigation->rowCount(parent); ++row) {
            const auto index = navigation->index(row, 0, parent);
            QString title = index.data().toString();
            if (m_levels.size() == 2 &&
                    parent.data(Qt::UserRole + 1).toString() == QStringLiteral("computer") &&
                    index.data(Qt::UserRole + 2).toString() == QStringLiteral("::mixxx_quick_lnk_node::")) {
                // Present bookmarked music folders directly: Quick Links is a
                // desktop sidebar grouping, not another level for this browser.
                for (int child = 0; child < m_navigation->rowCount(index); ++child) {
                    const auto link = m_navigation->index(child, 0, index);
                    m_folders.append({link, link.data().toString()});
                }
                continue;
            }
            if (m_levels.size() == 1) {
                const QString icon = index.data(Qt::UserRole + 1).toString();
                if (icon == QStringLiteral("playlist")) title = tr("Playlists");
                else if (icon == QStringLiteral("computer")) title = tr("Folders");
                else continue;
            }
            m_folders.append({index, title});
        }
    }
    if (m_levels.size() == 1 && m_engineLibrary) {
        const auto home = m_engineLibrary->navigation()->index(0, 0);
        if (m_engineLibrary->navigation()->rowCount(home)) {
            m_folders.append({home, home.data().toString()});
        }
    }
    if (m_levels.size() == 1 && m_tidalLibrary) {
        const auto home = m_tidalLibrary->navigation()->index(0, 0);
        m_folders.append({home, home.data().toString()});
    }
    const auto& level = m_levels.last();
    int selected = level.selectedRow;
    if (level.selection.isValid()) {
        if (level.selectedTrack && level.selection.model() == m_tracks) {
            selected = m_folders.size() + level.selection.row();
        } else if (!level.selectedTrack) {
            for (int row = 0; row < m_folders.size(); ++row) {
                if (m_folders[row].index == level.selection) { selected = row; break; }
            }
        }
    }
    m_currentRow = rowCount() ? std::clamp(selected, 0, rowCount() - 1) : -1;
    endResetModel();
    emit breadcrumbChanged(breadcrumb());
    emit selectionChanged(m_currentRow);
}
