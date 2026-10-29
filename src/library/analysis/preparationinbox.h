#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

// Only Mixxx writes its active database and emits the normal library/playlist
// notifications. The companion supplies verified local requests, never SQL.
inline void receivePreparationInbox(Library* library, const UserSettingsPointer& settings) {
    const QDir profile(settings->getSettingsPath());
    QFile configFile(profile.filePath("quietvinyl-sync.json"));
    if (!configFile.open(QIODevice::ReadOnly) || configFile.size() > 65536) return;
    const auto config = QJsonDocument::fromJson(configFile.readAll()).object();
    if (config.value("mode") != "collective" || !config.value("inbox_receive_enabled").toBool()) return;
    QFile registrationFile(profile.filePath("quietvinyl-collective.json"));
    if (!registrationFile.open(QIODevice::ReadOnly)) return;
    const auto registration = QJsonDocument::fromJson(registrationFile.readAll()).object();
    const auto root = QFileInfo(registration.value("library_root").toString()).canonicalFilePath();
    if (root.isEmpty()) return;
    const QDir requests(profile.filePath("Inbox/requests"));
    static const QRegularExpression hash("^[0-9a-f]{64}$");
    for (const auto& file : requests.entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        const auto id = file.completeBaseName();
        if (!hash.match(id).hasMatch() || file.isSymLink() || file.size() > 65536) continue;
        const auto ackPath = profile.filePath("Inbox/acks/" + id + ".json");
        if (QFile::exists(ackPath)) continue;
        QFile input(file.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) continue;
        const auto request = QJsonDocument::fromJson(input.readAll()).object();
        const auto fingerprint = request.value("sha256").toString();
        const auto relative = request.value("audio_path").toString();
        const QString expectedPrefix = "Music/" + fingerprint.left(2) + "/" + fingerprint + ".";
        if (request.value("version").toInt() != 1 || request.value("id") != id ||
                request.value("library_id") != registration.value("library_id") ||
                !hash.match(fingerprint).hasMatch() || !relative.startsWith(expectedPrefix) ||
                relative.contains("..") || relative.contains('\\') || relative.count('/') != 2) continue;
        const QFileInfo audio(QDir(root).filePath(relative));
        if (!audio.isFile() || audio.isSymLink() || audio.size() != request.value("bytes").toDouble() ||
                !audio.canonicalFilePath().startsWith(root + "/Music/")) continue;
        QFile catalogue(QDir(root).filePath("Catalogue/Tracks/" + fingerprint + ".json"));
        if (!catalogue.open(QIODevice::ReadOnly) || catalogue.size() > 65536) continue;
        const auto asset = QJsonDocument::fromJson(catalogue.readAll()).object();
        if (asset.value("sha256") != fingerprint || asset.value("audio_path") != relative ||
                asset.value("bytes") != request.value("bytes")) continue;
        auto* manager = library->trackCollectionManager();
        const auto ids = manager->resolveTrackIdsFromUrls({QUrl::fromLocalFile(audio.absoluteFilePath())}, true);
        if (ids.size() != 1 || !ids.first().isValid()) continue;
        const auto track = manager->getTrackById(ids.first());
        if (!track) continue;
        // Normal library import has already read embedded tags/artwork. Existing
        // edits are preserved; a fallback never exposes the hashed stem.
        if (track->getTitle().isEmpty() || track->getTitle() == fingerprint) {
            track->setTitle(request.value("title").toString());
        }
        if (manager->saveTrack(track) == TrackCollectionManager::SaveTrackResult::Failed) continue;
        auto& playlists = manager->internalCollection()->getPlaylistDAO();
        const QString name("Inbox / New arrivals");
        auto playlist = playlists.getPlaylistIdFromName(name);
        if (playlist < 0) playlist = playlists.createPlaylist(name);
        if (playlist < 0 || (!playlists.isTrackInPlaylist(ids.first(), playlist) &&
                !playlists.appendTrackToPlaylist(ids.first(), playlist))) continue;
        QDir().mkpath(QFileInfo(ackPath).absolutePath());
        QSaveFile ack(ackPath);
        const QJsonObject result{{"id", id}, {"sha256", fingerprint}, {"state", "complete"}, {"track_id", ids.first().toVariant().toInt()}};
        if (ack.open(QIODevice::WriteOnly) && ack.write(QJsonDocument(result).toJson()) >= 0) ack.commit();
        return; // Bound main-thread work to one arrival per timer tick.
    }
}
