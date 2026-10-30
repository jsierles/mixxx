#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <functional>
#include <memory>

// Owner-only, bounded recovery endpoint. The independent desktop process owns
// evidence capture, supervision and the UI; this only reads controls or exits.
class QuietVinylRecoveryBridge : public QObject {
  public:
    using Read = std::function<double(const QString&, const QString&)>;
    QuietVinylRecoveryBridge(const QString& path, Read read,
            std::function<void()> quit, QObject* parent = nullptr)
            : QObject(parent), m_read(std::move(read)), m_quit(std::move(quit)) {
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(100)) return; // Preserve another live owner.
        QLocalServer::removeServer(path); // A forced exit can leave a stale socket.
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(4);
        if (!m_server.listen(path)) return;
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = m_server.nextPendingConnection()) {
                auto data = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(1000, socket, &QLocalSocket::disconnectFromServer);
                connect(socket, &QLocalSocket::readyRead, this, [this, socket, data] {
                    data->append(socket->readAll());
                    if (data->size() > 1024) { socket->disconnectFromServer(); return; }
                    if (!data->contains('\n')) return;
                    const auto request = QJsonDocument::fromJson(data->left(data->indexOf('\n'))).object();
                    const QString op = request.value("op").toString();
                    QJsonObject reply{{"ok", false}};
                    if (op == "status") {
                        QJsonArray decks;
                        for (int deck = 1; deck <= 2; ++deck) {
                            QJsonObject state;
                            for (const char* key : {"vinylcontrol_enabled", "vinylcontrol_mode",
                                         "vinylbrowse_valid", "vinylbrowse_frame", "vinylbrowse_capture",
                                         "vinylcontrol_rate", "vinylcontrol_pitch_algorithm", "vinylcontrol_pitch_algorithm_active", "vinylcontrol_pitch_algorithm_switching", "vinylcontrol_pitch_algorithm_count", "play", "playposition"}) {
                                state.insert(key, m_read(QStringLiteral("[Channel%1]").arg(deck), key));
                            }
                            decks.append(state);
                        }
                        QJsonObject audio;
                        for (const char* key : {"audio_latency_usage", "audio_latency_overload_count",
                                     "audio_latency_overload"}) {
                            audio.insert(key, m_read("[App]", key));
                        }
                        reply = {{"ok", true}, {"decks", decks}, {"audio", audio}};
                    } else if (op == "quit" && !m_quitting) {
                        m_quitting = true;
                        reply = {{"ok", true}};
                        QTimer::singleShot(50, this, [this] { m_quit(); });
                    }
                    socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    socket->disconnectFromServer();
                });
            }
        });
    }
    bool listening() const { return m_server.isListening(); }

  private:
    QLocalServer m_server;
    Read m_read;
    std::function<void()> m_quit;
    bool m_quitting = false;
};
