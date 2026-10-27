#pragma once

// Opt-in GUI-thread bridge. Audio stays in Mixxx; only the owning user may open
// this local socket. The separate HTTP process owns network access and assets.
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <memory>

#include "control/controlobject.h"
#include "library/simplelibrarymodel.h"
#include "library/tidalplaylistlibrary.h"
#include "mixer/playerinfo.h"
#include "track/track.h"
#include "track/beats.h"
#include "vinylcontrol/defs_vinylcontrol.h"
#include "waveform/waveform.h"
#include "widget/wsimplelibrary.h"

class QuietVinylRemoteBridge : public QObject {
  public:
    static void start(WSimpleLibrary* browser) {
        const auto path = qEnvironmentVariable("QUIETVINYL_REMOTE_SOCKET");
        if (path.isEmpty()) return;
        new QuietVinylRemoteBridge(browser, path);
    }

  private:
    WSimpleLibrary* m_browser;
    QLocalServer m_server;
    int m_revision = 1;
    QString m_jobState, m_jobError, m_jobId;
    int m_jobDeck = 0;
    QElapsedTimer m_jobClock;

    static QJsonObject refusal(const QString& reason) {
        return {{"ok", false}, {"reason", reason}};
    }
    static QString group(int deck) { return QStringLiteral("[Channel%1]").arg(deck); }
    static double read(int deck, const QString& key) {
        const ConfigKey control(group(deck), key);
        return ControlObject::exists(control) ? ControlObject::get(control) : 0;
    }
    static void set(int deck, const QString& key, double value) {
        const ConfigKey control(group(deck), key);
        if (ControlObject::exists(control)) ControlObject::set(control, value);
    }
    static void pulse(int deck, const QString& key) { set(deck, key, 1); set(deck, key, 0); }
    static QString identity(const TrackPointer& track) {
        if (!track) return {};
        return identity(track->getLocation());
    }
    static QString identity(const QString& location) {
        if (location.isEmpty()) return {};
        return QString::fromLatin1(QCryptographicHash::hash(location.toUtf8(), QCryptographicHash::Sha256).toHex());
    }
    explicit QuietVinylRemoteBridge(WSimpleLibrary* browser, const QString& path)
            : QObject(browser), m_browser(browser) {
        // Never unlink an existing server belonging to another running app.
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(8);
        if (!m_server.listen(path)) {
            qWarning() << "Browser bridge unavailable:" << m_server.errorString();
            deleteLater();
            return;
        }
        qInfo() << "Browser bridge listening:" << path;
        auto* model = browser->browserModel();
        connect(model, &QAbstractItemModel::modelReset, this, [this] { ++m_revision; });
        connect(model, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (!roles.contains(SimpleLibraryModel::CacheStateRole)) ++m_revision;
                });
        connect(model, &QAbstractItemModel::rowsInserted, this, [this] { ++m_revision; });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { ++m_revision; });
        connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, this,
                [this](const QString& loadedGroup, TrackPointer track, TrackPointer) {
                    if (m_jobState == "loading" && loadedGroup == group(m_jobDeck) && track) {
                        m_jobState = "complete";
                        m_browser->setProperty("quietvinylRemoteLoad", false);
                    }
                });
        connect(browser, &WSimpleLibrary::tidalLoadFailed, this, [this](const QString& message) {
            if (m_jobState != "loading") return;
            m_jobState = "failed"; m_jobError = message;
            m_browser->setProperty("quietvinylRemoteLoad", false);
        });
        auto* watchdog = new QTimer(this);
        connect(watchdog, &QTimer::timeout, this, [this] {
            if (m_jobState != "loading") return;
            const auto elapsed = m_jobClock.elapsed();
            if (elapsed > 600000 || (elapsed > 15000 && (!m_browser->remoteLoadBusy() || m_browser->remoteLoadStatus().isEmpty()))) {
                m_browser->cancelRemoteLoad();
                m_jobState = "failed";
                m_jobError = "Load did not complete; check the Pi browser";
            }
        });
        watchdog->start(1000);
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = m_server.nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(3000, socket, &QLocalSocket::disconnectFromServer);
                connect(socket, &QLocalSocket::readyRead, socket, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    if (bytes->size() > 16384) { socket->disconnectFromServer(); return; }
                    const int newline = bytes->indexOf('\n');
                    if (newline < 0) return;
                    QJsonParseError error;
                    const auto doc = QJsonDocument::fromJson(bytes->left(newline), &error);
                    const auto answer = error.error == QJsonParseError::NoError && doc.isObject()
                            ? request(doc.object()) : refusal("invalid-json");
                    socket->write(QJsonDocument(answer).toJson(QJsonDocument::Compact) + '\n');
                    socket->disconnectFromServer();
                });
            }
        });
    }

    QJsonObject waveform(const TrackPointer& track, double start, double span, int buckets, bool overview) {
        QJsonObject result{{"start", start}, {"span", span}, {"count", buckets}};
        const auto wave = overview ? track->getWaveformSummary() : track->getWaveform();
        if (!wave || wave->getAudioVisualRatio() <= 0 || span <= 0) return result;
        const int completed = std::min(wave->getCompletion(), wave->getDataSize());
        if (completed < 2) return result;
        const double sampleRate = track->getSampleRate().value();
        const double indicesPerSecond = sampleRate * 2 / wave->getAudioVisualRatio();
        QByteArray bytes(buckets * 4, 0);
        for (int bucket = 0; bucket < buckets; ++bucket) {
            const double from = start + span * bucket / buckets;
            const double to = start + span * (bucket + 1) / buckets;
            if (to <= 0 || from >= track->getDuration()) continue;
            const int first = std::max(0, int(std::floor(std::max(0.0, from) * indicesPerSecond)) / 2 * 2);
            const int last = std::min(completed, std::max(first + 2, int(std::ceil(to * indicesPerSecond))));
            unsigned char bands[4] = {};
            // At most 32 reads per output bin, including both source channels.
            const int step = std::max(2, (last - first) / 32 / 2 * 2);
            for (int i = first; i < last; i += step) {
                for (int channel = 0; channel < 2 && i + channel < completed; ++channel) {
                    const auto data = wave->get(i + channel).filtered;
                    bands[0] = std::max(bands[0], data.low);
                    bands[1] = std::max(bands[1], data.mid);
                    bands[2] = std::max(bands[2], data.high);
                    bands[3] = std::max(bands[3], data.all);
                }
            }
            for (int band = 0; band < 4; ++band) bytes[bucket * 4 + band] = char(bands[band]);
        }
        result.insert("data", QString::fromLatin1(bytes.toBase64()));
        return result;
    }

    QJsonObject snapshot() {
        if (m_jobState == "loading" && m_jobClock.elapsed() > 15000 && !m_browser->remoteLoadBusy()) {
            m_jobState = "failed";
            m_jobError = "Load did not complete; check the Pi browser";
            m_browser->setProperty("quietvinylRemoteLoad", false);
        }
        QJsonArray decks;
        for (int deck = 1; deck <= 2; ++deck) {
            const auto track = PlayerInfo::instance().getTrackInfo(group(deck));
            QJsonObject item{{"deck", deck}, {"id", identity(track)}, {"playing", read(deck, "play") > 0}, {"recordMoving", m_browser->recordMoving(deck)},
                    {"mode", read(deck, "vinylcontrol_mode") == 0 ? "Absolute" : "Relative"},
                    {"dvs", read(deck, "vinylcontrol_enabled") > 0}};
            if (track) {
                const double duration = track->getDuration();
                const double position = std::clamp(read(deck, "playposition"), 0.0, 1.0) * duration;
                const double sampleRate = track->getSampleRate().value();
                item.insert("title", track->getTitle());
                item.insert("artist", track->getArtist());
                item.insert("duration", duration);
                item.insert("position", position);
                item.insert("bpm", track->getBpm());
                item.insert("displayBpm", read(deck, "visual_bpm"));
                item.insert("key", track->getKeyText());
                item.insert("loopActive", read(deck, "loop_enabled") > 0);
                item.insert("loopBeats", read(deck, "beatloop_size"));
                item.insert("loopStart", sampleRate > 0 ? read(deck, "loop_start_position") / (sampleRate * 2) : 0);
                item.insert("loopEnd", sampleRate > 0 ? read(deck, "loop_end_position") / (sampleRate * 2) : 0);
                QJsonArray cues;
                for (int cue = 1; cue <= 8; ++cue) {
                    const auto prefix = QStringLiteral("hotcue_%1").arg(cue);
                    cues.append(QJsonObject{{"slot", cue}, {"enabled", read(deck, prefix + "_status") > 0},
                            {"position", sampleRate > 0 ? read(deck, prefix + "_position") / (sampleRate * 2) : 0},
                            {"label", QStringLiteral("Cue %1").arg(cue)}});
                }
                for (const auto& cue : track->getCuePoints()) {
                    const int slot = cue->getHotCue();
                    if (slot >= 0 && slot < cues.size() && !cue->getLabel().isEmpty()) {
                        auto value = cues[slot].toObject(); value.insert("label", cue->getLabel()); cues[slot] = value;
                    }
                }
                item.insert("cues", cues);
                item.insert("detail", waveform(track, position - 8, 16, 800, false));
                item.insert("overview", waveform(track, 0, duration, 400, true));
                QJsonArray beats;
                const auto grid = track->getBeats();
                if (grid && sampleRate > 0) {
                    auto beat = grid->findNextBeat(mixxx::audio::FramePos(std::max(0.0, position - 8) * sampleRate));
                    for (int count = 0; count < 128 && beat.isValid() && beat.value() < (position + 8) * sampleRate; ++count) {
                        beats.append(beat.value() / sampleRate);
                        beat = grid->findNextBeat(mixxx::audio::FramePos(beat.value() + 1));
                    }
                }
                item.insert("beats", beats);
            }
            decks.append(item);
        }
        return {{"ok", true}, {"decks", decks}, {"libraryRevision", m_revision},
                {"nativeBrowsing", m_browser->isVisible()}, {"busy", m_browser->remoteLoadBusy()},
                {"downloads", m_browser->tidalLibrary() ? m_browser->tidalLibrary()->downloads() : QJsonArray{}},
                {"job", QJsonObject{{"id", m_jobId}, {"state", m_jobState}, {"error", m_jobError},
                    {"status", m_browser->remoteLoadStatus()}}}};
    }

    QJsonObject request(const QJsonObject& req) {
        const auto op = req.value("op").toString();
        auto* model = m_browser->browserModel();
        if (op == "snapshot") return snapshot();
        if (op == "library") {
            QJsonArray rows;
            const int limit = std::clamp(req.value("limit").toInt(500), 1, 2000);
            const int offset = std::clamp(req.value("offset").toInt(), 0, model->rowCount());
            for (int row = offset; row < model->rowCount() && row < offset + limit; ++row) {
                const auto index = model->index(row, 0);
                rows.append(QJsonObject{{"row", row}, {"revision", m_revision},
                    {"id", identity(index.data(SimpleLibraryModel::TidalIdRole).toString().isEmpty()
                            ? index.data(SimpleLibraryModel::LocationRole).toString()
                            : QString("tidal:") + index.data(SimpleLibraryModel::TidalIdRole).toString())},
                    {"tidalId", index.data(SimpleLibraryModel::TidalIdRole).toString()},
                    {"cacheState", index.data(SimpleLibraryModel::CacheStateRole).toString()},
                    {"downloadProgress", index.data(SimpleLibraryModel::DownloadProgressRole).toInt()},
                    {"downloadDetail", index.data(SimpleLibraryModel::DownloadDetailRole).toString()},
                    {"title", index.data().toString()}, {"artist", index.data(SimpleLibraryModel::SubtitleRole).toString()},
                    {"bpm", index.data(SimpleLibraryModel::BpmRole).toDouble()},
                    {"folder", index.data(SimpleLibraryModel::NodeRole).toBool()}});
            }
            return {{"ok", true}, {"rows", rows}, {"revision", m_revision}, {"total", model->rowCount()},
                    {"breadcrumb", model->breadcrumb()}, {"canGoBack", model->canGoBack()}, {"emptyText", model->emptyText()}};
        }
        if (m_browser->isVisible() || QApplication::activeModalWidget()) return refusal("native-browser-or-dialog-active");
        if (op == "cancel") {
            if (m_browser->property("quietvinylRemoteLoad").toBool() && m_jobState == "loading") {
                if (m_browser->cancelRemoteLoad()) {
                    m_jobState = "cancelled";
                } else return refusal("core-load-in-progress");
            }
            return {{"ok", true}};
        }
        if (op == "back") {
            if (m_browser->remoteLoadBusy() || m_jobState == "loading") return refusal("loading");
            model->back();
            return {{"ok", true}};
        }
        if (op == "enter" || op == "load" || op == "download") {
            if (m_browser->remoteLoadBusy() || m_jobState == "loading") return refusal("loading");
            const int row = req.value("row").toInt(-1);
            if (req.value("revision").toInt(-1) != m_revision || row < 0 || row >= model->rowCount()) return refusal("stale-library-selection");
            const auto index = model->index(row, 0);
            const bool folder = index.data(SimpleLibraryModel::NodeRole).toBool();
            if (op == "enter") {
                if (!folder) return refusal("not-a-folder");
                model->selectRow(row); model->enter();
                return {{"ok", true}};
            }
            if (folder) return refusal("not-a-track");
            model->selectRow(row);
            const auto tidal = model->tidalSelection();
            if (op == "download" || (tidal.isValid() && !m_browser->tidalLibrary()->cached(tidal))) {
                if (!tidal.isValid() || !model->canLoadSelected()) return refusal("track-unavailable");
                m_browser->tidalLibrary()->download(tidal);
                return {{"ok", true}, {"download", true}};
            }
            const int deck = req.value("deck").toInt();
            if (deck != 1 && deck != 2) return refusal("invalid-deck");
            if (!model->canLoadSelected() && model->selectedRow() == row) return refusal("track-unavailable");
            if (req.value("expectedTrack").toString() != identity(PlayerInfo::instance().getTrackInfo(group(deck)))) return refusal("deck-track-changed");
            if (m_browser->needsLoadConfirmation(group(deck)) && !req.value("replacePlaying").toBool())
                return refusal("record-moving");
            model->selectRow(row);
            if (!model->canLoadSelected()) return refusal("track-unavailable");
            m_browser->setProperty("quietvinylRemoteLoad", true);
            m_jobDeck = deck; m_jobId = QString::number(QDateTime::currentMSecsSinceEpoch());
            m_jobState = "loading"; m_jobError.clear(); m_jobClock.start();
            m_browser->loadSelected(group(deck), false, req.value("replacePlaying").toBool());
            return {{"ok", true}, {"job", m_jobId}};
        }
        if (op != "command") return refusal("unknown-operation");
        const int deck = req.value("deck").toInt();
        if (deck != 1 && deck != 2) return refusal("invalid-deck");
        const auto track = PlayerInfo::instance().getTrackInfo(group(deck));
        if (!track || req.value("expectedTrack").toString() != identity(track)) return refusal("deck-track-changed");
        if ((m_browser->remoteLoadBusy() || m_jobState == "loading") && deck == m_jobDeck) return refusal("loading");
        const auto action = req.value("action").toString();
        if (action == "play") set(deck, "play", 1);
        else if (action == "pause") set(deck, "play", 0);
        else if (action == "off") { if (read(deck, "loop_enabled") > 0) pulse(deck, "reloop_toggle"); }
        else if (action == "loop") {
            const double beats = req.value("beats").toDouble();
            if (beats != 1 && beats != 2 && beats != 4 && beats != 8 && beats != 16) return refusal("invalid-loop-size");
            if (read(deck, "loop_enabled") > 0 && std::abs(read(deck, "beatloop_size") - beats) < .001) pulse(deck, "reloop_toggle");
            else { set(deck, "beatloop_size", beats); if (read(deck, "loop_enabled") <= 0) pulse(deck, "beatloop_activate"); }
        } else if (action == "cue") {
            const int slot = req.value("slot").toInt();
            if (slot < 1 || slot > 8 || read(deck, QStringLiteral("hotcue_%1_status").arg(slot)) <= 0) return refusal("cue-unassigned");
            set(deck, "vinylcontrol_mode", MIXXX_VCMODE_RELATIVE);
            pulse(deck, QStringLiteral("hotcue_%1_goto").arg(slot));
        } else if (action == "seek" || action == "start") {
            const double seconds = action == "start" ? 0 : req.value("seconds").toDouble(-1);
            if (!std::isfinite(seconds) || seconds < 0 || seconds > track->getDuration() || track->getDuration() <= 0) return refusal("invalid-position");
            if (action == "start") set(deck, "play", 0);
            if (read(deck, "loop_enabled") > 0) pulse(deck, "reloop_toggle");
            set(deck, "vinylcontrol_mode", MIXXX_VCMODE_RELATIVE);
            set(deck, "playposition", seconds / track->getDuration());
        } else return refusal("unknown-command");
        return {{"ok", true}};
    }
};
