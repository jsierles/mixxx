#include "library/tidalplaylistlibrary.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QUuid>
#include <cmath>
#include <memory>
#include <QDebug>

#include "moc_tidalplaylistlibrary.cpp"

namespace { constexpr int kPlaylistOrderRole = Qt::UserRole + 400; }

TidalPlaylistLibrary::TidalPlaylistLibrary(QString program, QStringList helperArguments,
        QString profile, QString cache, QObject* parent)
        : QObject(parent), m_program(std::move(program)), m_profile(std::move(profile)),
          m_cache(std::move(cache)), m_helperArguments(std::move(helperArguments)) {
    m_navigation.appendRow(new QStandardItem(tr("Tidal")));
    addConnect();
    m_navigation.setSortRole(kPlaylistOrderRole);
    readPlaylistCache();
    m_tracks.setColumnCount(6);
    const QStringList names{"title", "artist", "filename", "location", "bpm", "coverurl"};
    for (int column = 0; column < names.size(); ++column)
        m_tracks.setHeaderData(column, Qt::Horizontal, names[column], Qt::UserRole + 1);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        const bool loading = preparing();
        cancel();
        setStatus(tr("Tidal request timed out. Open the playlist again to retry."));
        if (loading) emit preparationFailed();
    });
}

TidalPlaylistLibrary::~TidalPlaylistLibrary() {
    cancel();
    auto* transfer = m_downloadProcess.data();
    stopProcess(transfer);
    if (transfer && !transfer->waitForFinished(2000)) transfer->kill();
}

void TidalPlaylistLibrary::addConnect() {
    auto* item = new QStandardItem(tr("Connect Tidal"));
    item->setData(true, ActionRole);
    root()->appendRow(item);
}

void TidalPlaylistLibrary::updatePlaylists(const QJsonArray& items) {
    QJsonArray playlists;
    QSet<QString> ids;
    for (const auto& value : items) {
        const auto item = value.toObject();
        const auto id = item.value("id").toString();
        if (QUuid(id).isNull() || ids.contains(id)) continue;
        ids.insert(id);
        playlists.append(QJsonObject{{"id", id}, {"title", item.value("title").toString().left(256)}});
    }
    if (m_hasPlaylists && playlists == m_playlists) return;
    // Keep unchanged playlist items alive, including their persistent selection.
    QHash<QString, QStandardItem*> existing;
    for (int row = root()->rowCount() - 1; row >= 0; --row) {
        auto* item = root()->child(row);
        if (item->data(ActionRole).toBool()) continue;
        const auto id = item->data(PlaylistRole).toString();
        if (!ids.contains(id)) root()->removeRow(row);
        else existing.insert(id, item);
    }
    int order = 0;
    for (const auto& value : playlists) {
        const auto entry = value.toObject();
        const auto id = entry.value("id").toString();
        auto* item = existing.value(id);
        if (!item) {
            item = new QStandardItem;
            item->setData(id, PlaylistRole);
            root()->appendRow(item);
        }
        item->setText(entry.value("title").toString());
        item->setData(order++, kPlaylistOrderRole);
    }
    for (int row = 0; row < root()->rowCount(); ++row) {
        auto* item = root()->child(row);
        if (item->data(ActionRole).toBool()) item->setData(order, kPlaylistOrderRole);
    }
    root()->sortChildren(0);
    m_playlists = playlists;
    m_hasPlaylists = true;
}

void TidalPlaylistLibrary::readPlaylistCache() {
    QFile file(QDir(m_profile).filePath(QStringLiteral("playlists.json")));
    const QFileInfo info(file);
    if (!info.isFile() || info.isSymLink() || info.size() > 8 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return;
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    if (saved.value("version").toInt() != 1 || !saved.value("items").isArray() || saved.value("items").toArray().size() > 10000) return;
    updatePlaylists(saved.value("items").toArray());
}

void TidalPlaylistLibrary::savePlaylistCache() const {
    QSaveFile file(QDir(m_profile).filePath(QStringLiteral("playlists.json")));
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"items", m_playlists}}).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) return;
    file.commit(); // A cache write failure must not discard the in-memory list.
}

void TidalPlaylistLibrary::setStatus(const QString& message) {
    m_status = message;
    if (m_activeNode.isValid()) m_navigation.setData(m_activeNode, message, StatusRole);
    emit statusChanged(message);
}

void TidalPlaylistLibrary::stopProcess(QProcess* process) {
    if (!process) return;
    disconnect(process, nullptr, nullptr, nullptr);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
    process->terminate();
    QTimer::singleShot(2000, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill(); else process->deleteLater();
    });
}

void TidalPlaylistLibrary::cancel() {
    auto* loader = m_loadProcess.data();
    m_loadProcess = nullptr;
    stopProcess(loader);
    m_timeout.stop();
    if (m_process) {
        auto* process = m_process.data();
        m_process = nullptr;
        disconnect(process, nullptr, this, nullptr);
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                process, &QObject::deleteLater);
        process->terminate();
        QTimer::singleShot(2000, process, [process] {
            if (process->state() != QProcess::NotRunning) process->kill();
            else process->deleteLater();
        });
    }
    m_output.clear();
    m_result = {};
    m_done = false;
}

bool TidalPlaylistLibrary::canPrepare(const QModelIndex& index) const {
    return index.isValid() && index.model() == &m_tracks &&
            index.data(AvailableRole).toBool() && !index.data(TrackIdRole).toString().isEmpty();
}

void TidalPlaylistLibrary::activate(const QModelIndex& index) {
    if (!index.isValid() || index.model() != &m_navigation) return;
    const bool login = isAction(index);
    m_activeNode = login ? root()->index() : index;
    if (login || index == root()->index()) {
        if (login) {
            // Explicit sign-in may switch accounts. Do not retain the old list.
            m_hasPlaylists = false;
            m_playlists = {};
            QFile::remove(QDir(m_profile).filePath(QStringLiteral("playlists.json")));
            root()->removeRows(0, root()->rowCount());
            addConnect();
        }
        start(login ? QStringLiteral("login") : QStringLiteral("playlists"));
    } else {
        m_tracks.removeRows(0, m_tracks.rowCount());
        start(QStringLiteral("tracks"), index.data(PlaylistRole).toString());
    }
}

bool TidalPlaylistLibrary::cached(const QModelIndex& index) const {
    return index.data(CacheStateRole).toString() == QStringLiteral("cached");
}

void TidalPlaylistLibrary::download(const QModelIndex& index) {
    if (!canPrepare(index) || cached(index)) return;
    const auto id = index.data(TrackIdRole).toString();
    const auto state = m_transfers.value(id).state;
    if (state == "queued" || state == "downloading" || state == "checking") return;
    if (m_queue.size() >= 50) { setStatus(tr("Download queue is full. Wait for a track to finish.")); return; }
    m_queue.enqueue(id);
    updateTransfer(id, "queued");
    nextDownload();
}

void TidalPlaylistLibrary::prepare(const QModelIndex& index, const QString& group) {
    if (!canPrepare(index) || (group != "[Channel1]" && group != "[Channel2]")) return;
    if (!cached(index)) { download(index); return; }
    auto* previous = m_loadProcess.data(); m_loadProcess = nullptr; stopProcess(previous);
    const auto id = index.data(TrackIdRole).toString();
    setStatus(tr("Checking cached Tidal track…"));
    m_loadProcess = runTransfer("cached", id, [](const QJsonObject&) {},
            [this, id, group](const QJsonObject& result, const QString& error) {
                m_loadProcess = nullptr;
                const auto path = verifiedPath(result, id);
                if (!error.isEmpty() || path.isEmpty()) {
                    updateTransfer(id, "not-cached", -1, error.isEmpty() ? tr("Cache changed. Select to download again.") : error);
                    setStatus(error.isEmpty() ? tr("Cache changed. Select to download again.") : error);
                    emit preparationFailed(); return;
                }
                setStatus({});
                emit prepared(path, group);
            });
}

QString TidalPlaylistLibrary::errorText(const QString& code) {
    if (code == "setup") return tr("Tidal is not set up on this device.");
    if (code == "request-timeout") return tr("Tidal download took too long. Select again to retry.");
    if (code == "network-timeout") return tr("Tidal timed out. Select again to retry.");
    if (code == "network") return tr("Tidal connection failed. Select again to retry.");
    if (code == "rate-limited") return tr("Tidal is busy. Wait a moment, then retry.");
    if (code == "access-denied") return tr("Tidal declined this download. Retry, or reconnect if it repeats.");
    if (code == "sign-in") return tr("Tidal sign-in expired. Choose Connect Tidal.");
    if (code == "cache-full") return tr("Tidal cache is full. Clear unused cached audio before retrying.");
    if (code == "cache-missing") return tr("Cached audio changed or is missing. Select again to download.");
    if (code == "unavailable") return tr("This track is unavailable on Tidal.");
    if (code == "audio") return tr("Tidal audio could not be verified. Select again to retry.");
    if (code == "profile-busy") return tr("Tidal sign-in is busy. Try again shortly.");
    return tr("Tidal request failed. Select again to retry.");
}

QString TidalPlaylistLibrary::verifiedPath(const QJsonObject& result, const QString& id) const {
    const QFileInfo file(result.value("path").toString());
    const auto cache = QFileInfo(m_cache).canonicalFilePath();
    const double duration = result.value("duration").toDouble();
    return result.value("event") == "prepared" && result.value("track").toString() == id &&
                    !cache.isEmpty() && file.isFile() && !file.isSymLink() && file.size() > 0 &&
                    file.suffix() == "flac" && file.canonicalFilePath().startsWith(cache + QDir::separator()) &&
                    std::isfinite(duration) && duration > 0 ? file.canonicalFilePath() : QString();
}

void TidalPlaylistLibrary::updateTransfer(const QString& id, const QString& state, int progress, const QString& detail) {
    m_transfers.insert(id, {state, detail, progress});
    for (int row = 0; row < m_tracks.rowCount(); ++row) {
        auto* item = m_tracks.item(row);
        if (item->data(TrackIdRole).toString() != id) continue;
        item->setData(state, CacheStateRole);
        item->setData(progress, ProgressRole);
        item->setData(detail, DetailRole);
    }
}

QJsonArray TidalPlaylistLibrary::downloads() const {
    QJsonArray result;
    for (auto it = m_transfers.cbegin(); it != m_transfers.cend(); ++it)
        result.append(QJsonObject{{"track", it.key()}, {"state", it->state},
                {"progress", it->progress}, {"detail", it->detail}});
    return result;
}

void TidalPlaylistLibrary::nextDownload() {
    if (m_downloadProcess || m_queue.isEmpty()) return;
    m_downloadId = m_queue.dequeue();
    const auto id = m_downloadId;
    updateTransfer(id, "downloading", 0);
    m_downloadProcess = runTransfer("prepare", id, [this, id](const QJsonObject& event) {
        if (event.value("event") == "progress") {
            const int total = event.value("total").toInt();
            if (total > 0) updateTransfer(id, "downloading", qBound(0, int(100.0 * event.value("current").toInt() / total), 100));
        } else if (event.value("event") == "status") updateTransfer(id, "checking");
    }, [this, id](const QJsonObject& result, const QString& error) {
        m_downloadProcess = nullptr;
        const auto path = verifiedPath(result, id);
        if (error.isEmpty() && !path.isEmpty()) updateTransfer(id, "cached", 100);
        else {
            const auto message = error.isEmpty() ? tr("Tidal audio could not be verified. Select again to retry.") : error;
            updateTransfer(id, "failed", -1, message);
            qWarning() << "Tidal download failed" << id << message;
        }
        m_downloadId.clear();
        QTimer::singleShot(0, this, [this] { nextDownload(); });
    });
}

QProcess* TidalPlaylistLibrary::runTransfer(const QString& operation, const QString& id,
        std::function<void(const QJsonObject&)> progress,
        std::function<void(const QJsonObject&, const QString&)> finished) {
    struct Output { QByteArray bytes; QJsonObject result; QString error; bool done = false; };
    auto output = std::make_shared<Output>();
    auto* process = new QProcess(this);
    auto* deadline = new QTimer(process); deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, process, [process, output, operation, id] {
        qWarning() << "Tidal helper deadline exceeded" << operation << id;
        output->error = errorText("request-timeout"); process->kill();
    });
    const auto consume = [process, output, progress] {
        output->bytes += process->readAllStandardOutput();
        if (output->bytes.size() > 8 * 1024 * 1024) { output->error = errorText({}); process->kill(); return; }
        while (output->bytes.contains('\n')) {
            const int end = output->bytes.indexOf('\n');
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(output->bytes.left(end), &parse);
            output->bytes.remove(0, end + 1);
            if (parse.error != QJsonParseError::NoError || !doc.isObject()) { output->error = errorText({}); process->kill(); return; }
            const auto object = doc.object();
            const auto event = object.value("event").toString();
            if (event == "prepared") output->result = object;
            else if (event == "done") output->done = true;
            else if (event == "error") output->error = errorText(object.value("code").toString());
            else progress(object);
        }
    };
    connect(process, &QProcess::readyReadStandardOutput, this, consume);
    connect(process, &QProcess::readyReadStandardError, process, [process] { process->readAllStandardError(); });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [process, output, consume, finished](int code, QProcess::ExitStatus status) {
                consume();
                if (output->error.isEmpty() && (code || status != QProcess::NormalExit || !output->done)) output->error = errorText({});
                finished(output->result, output->error);
                process->deleteLater();
            });
    connect(process, &QProcess::errorOccurred, this, [process, finished](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { finished({}, tr("Tidal is not set up on this device.")); process->deleteLater(); }
    });
    process->setProgram(m_program);
    auto args = m_helperArguments;
    args << "--profile" << m_profile << "--cache" << m_cache << "browser" << operation << id;
    process->setArguments(args);
    deadline->start(300000);
    process->start();
    return process;
}

void TidalPlaylistLibrary::start(const QString& operation, const QString& identity, const QString& group) {
    cancel();
    m_operation = operation;
    m_identity = identity;
    m_group = group;
    setStatus(operation == QStringLiteral("playlists") && m_hasPlaylists
                    ? tr("Refreshing Tidal playlists…") : tr("Connecting to Tidal…"));
    auto* process = new QProcess(this);
    m_process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    process->setProgram(m_program);
    QStringList arguments = m_helperArguments;
    arguments << "--profile" << m_profile << "--cache" << m_cache << "browser" << operation;
    if (!identity.isEmpty()) arguments << identity;
    process->setArguments(arguments);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] { consume(process); });
    // Never forward raw helper stderr, which may contain account diagnostics.
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (m_process == process && error == QProcess::FailedToStart) {
            fail(tr("Tidal is not set up on this device."));
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                if (m_process != process) return;
                consume(process);
                if (m_process != process) return;
                m_timeout.stop();
                m_process = nullptr;
                process->deleteLater();
                if (code != 0 || status != QProcess::NormalExit || !m_done) {
                    fail(tr("Tidal could not complete the request. Reopen the playlist or connect again."));
                    return;
                }
                applyResult();
            });
    m_timeout.start(operation == QStringLiteral("login") ? 360000 : 300000);
    process->start();
}

void TidalPlaylistLibrary::consume(QProcess* process) {
    if (m_process != process) return;
    m_output += process->readAllStandardOutput();
    if (m_output.size() > 8 * 1024 * 1024) {
        fail(tr("Tidal returned too much data. Try a smaller playlist."));
        return;
    }
    while (m_output.contains('\n')) {
        const auto end = m_output.indexOf('\n');
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(m_output.left(end), &error);
        m_output.remove(0, end + 1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(tr("Tidal returned an unreadable response."));
            return;
        }
        const auto object = document.object();
        const auto event = object.value("event").toString();
        if (event == QStringLiteral("done")) m_done = true;
        else if (event == QStringLiteral("error")) {
            // Friendly fixed UI text prevents credentials or URLs from being echoed.
            fail(errorText(object.value("code").toString()));
            return;
        } else if (event == QStringLiteral("login")) {
            setStatus(tr("On your phone: link.tidal.com\nDevice code: %1\nWaiting for sign-in…")
                              .arg(object.value("code").toString().left(12)));
        } else if (event == QStringLiteral("progress")) {
            const int total = object.value("total").toInt();
            const int current = object.value("current").toInt();
            if (total > 0) setStatus(tr("Preparing Tidal track: %1% · Back or Close to cancel")
                                            .arg(qBound(0, int(100.0 * current / total), 100)));
        } else if (event == QStringLiteral("status")) {
            setStatus(tr("Checking complete Tidal audio…"));
        } else if (event == QStringLiteral("playlists") || event == QStringLiteral("tracks") ||
                event == QStringLiteral("prepared")) m_result = object;
    }
}

void TidalPlaylistLibrary::fail(const QString& message) {
    const bool loading = m_operation == QStringLiteral("prepare");
    cancel();
    setStatus(m_operation == QStringLiteral("playlists") && m_hasPlaylists
                    ? tr("Showing saved playlists · %1").arg(message) : message);
    if (m_operation == QStringLiteral("playlists") || m_operation == QStringLiteral("login")) {
        if (!root()->rowCount()) addConnect();
    }
    if (loading) emit preparationFailed();
}

void TidalPlaylistLibrary::applyResult() {
    if (m_operation == QStringLiteral("login")) {
        start(QStringLiteral("playlists"));
        return;
    }
    if (m_result.value("event").toString() != (m_operation == QStringLiteral("prepare")
                    ? QStringLiteral("prepared") : m_operation)) {
        fail(tr("Tidal returned an incomplete response."));
        return;
    }
    if (m_operation == QStringLiteral("prepare")) {
        const QFileInfo file(m_result.value("path").toString());
        const QString cache = QFileInfo(m_cache).canonicalFilePath();
        const double duration = m_result.value("duration").toDouble();
        if (cache.isEmpty() || !file.isFile() || file.isSymLink() ||
                !file.canonicalFilePath().startsWith(cache + QDir::separator()) ||
                file.suffix() != QStringLiteral("flac") || file.size() <= 0 ||
                !std::isfinite(duration) || duration <= 0 || m_result.value("track").toString() != m_identity) {
            fail(tr("The Tidal track is not ready to load."));
            return;
        }
        setStatus({});
        emit prepared(file.canonicalFilePath(), m_group);
        return;
    }
    const auto items = m_result.value("items").toArray();
    if (items.size() > 10000) { fail(tr("This Tidal collection is too large.")); return; }
    if (m_operation == QStringLiteral("playlists")) {
        updatePlaylists(items);
        savePlaylistCache();
        setStatus(items.isEmpty() ? tr("No Tidal playlists yet. Create or save a playlist in Tidal.") : QString());
    } else {
        m_tracks.removeRows(0, m_tracks.rowCount());
        for (const auto& value : items) {
            const auto item = value.toObject();
            const auto id = item.value("id").toString();
            bool valid = false;
            id.toULongLong(&valid);
            if (!valid) continue;
            auto* title = new QStandardItem(item.value("title").toString());
            title->setData(id, TrackIdRole);
            title->setData(item.value("available").toBool(), AvailableRole);
            const auto existing = m_transfers.value(id);
            const bool active = existing.state == "queued" || existing.state == "downloading" || existing.state == "checking";
            const QString state = active ? existing.state : item.value("cached").toBool() ? QString("cached") : existing.state == "failed" ? QString("failed") : QString("not-cached");
            if (!active && m_transfers.contains(id)) m_transfers[id] = {state, existing.detail, state == "cached" ? 100 : -1};
            title->setData(item.value("available").toBool() ? state : QString("unavailable"), CacheStateRole);
            title->setData(existing.progress, ProgressRole);
            title->setData(existing.detail, DetailRole);
            m_tracks.appendRow({title, new QStandardItem(item.value("artist").toString()),
                    new QStandardItem, new QStandardItem, new QStandardItem,
                    new QStandardItem(item.value("cover").toString())});
            const double bpm = item.value("bpm").toDouble();
            if (std::isfinite(bpm) && bpm > 0) m_tracks.setData(m_tracks.index(m_tracks.rowCount() - 1, 4), bpm);
        }
        setStatus(m_tracks.rowCount() ? QString() : tr("Empty Tidal playlist"));
    }
}
