#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScroller>
#include <QTimer>
#include <QTouchEvent>
#include <QWidget>
#include <array>
#include <cmath>
#include <functional>
#include <memory>

// An opt-in local GUI bridge. USB, compression and frame scheduling belong to
// the separate display process, never to Mixxx's audio callback.
class QuietVinylDisplayBridge : public QObject {
  public:
    using NativeImage = std::function<QImage(QWidget*, qreal)>;
    static constexpr int Width = 1280;
    static constexpr int Height = 800;

    QuietVinylDisplayBridge(const QString& path, const std::array<QWidget*, 2>& roots,
            NativeImage nativeImage = {}, QObject* parent = nullptr,
            std::function<void()> synchronize = {})
            : QObject(parent), m_nativeImage(std::move(nativeImage)),
              m_synchronize(std::move(synchronize)) {
        for (int i = 0; i < 2; ++i) m_roots[i] = roots[i];
        if (!QDir::isAbsolutePath(path)) return;
        m_lock = std::make_unique<QLockFile>(path + ".lock");
        if (!m_lock->tryLock(0) || QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
            qWarning() << "Denon display bridge path is already owned:" << path;
            return;
        }
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(4);
        // Refuse an occupied path; do not unlink another process's listener.
        if (!m_server.listen(path)) {
            qWarning() << "Denon display bridge unavailable:" << m_server.errorString();
            return;
        }
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = m_server.nextPendingConnection()) {
                auto client = std::make_shared<Client>();
                client->socket = socket;
                client->seen.start();
                m_clients.append(client);
                connect(socket, &QLocalSocket::disconnected, this, [this, client] {
                    cancel(*client);
                    m_clients.removeAll(client);
                    client->socket->deleteLater();
                });
                connect(socket, &QLocalSocket::readyRead, this, [this, client] {
                    client->seen.restart();
                    client->buffer += client->socket->readAll();
                    if (client->buffer.size() > 4096) { client->socket->abort(); return; }
                    while (client->buffer.contains('\n')) {
                        const int end = client->buffer.indexOf('\n');
                        const auto bytes = client->buffer.left(end);
                        client->buffer.remove(0, end + 1);
                        QJsonParseError error;
                        const auto doc = QJsonDocument::fromJson(bytes, &error);
                        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                            reply(*client, failure("invalid-request"));
                            continue;
                        }
                        request(*client, doc.object());
                        if (!client->socket || client->socket->state() != QLocalSocket::ConnectedState) break;
                    }
                });
            }
        });
        connect(&m_watchdog, &QTimer::timeout, this, [this] {
            const auto clients = m_clients;
            for (auto client : clients) {
                if (client->seen.elapsed() > 1500) {
                    cancel(*client);
                    client->socket->abort();
                } else if (client->pressed && !validView(*client)) cancel(*client);
            }
        });
        m_watchdog.start(100);
        qInfo() << "Denon display bridge listening:" << path;
    }

    ~QuietVinylDisplayBridge() override {
        for (const auto& client : m_clients) cancel(*client);
    }
    bool listening() const { return m_server.isListening(); }

    // Paint in widget stacking order, replacing native GL containers with an
    // explicit framebuffer image. This keeps normal sibling overlays on top.
    static QImage captureTree(QWidget* root, const NativeImage& nativeImage = {}, qreal pixelRatio = 1) {
        if (!root || root->size().isEmpty()) return {};
        QImage image(root->size() * pixelRatio, QImage::Format_RGB32);
        image.setDevicePixelRatio(pixelRatio);
        image.fill(Qt::black);
        QPainter painter(&image);
        std::function<void(QWidget*, QPoint)> paint = [&](QWidget* widget, QPoint offset) {
            if (widget != root && (!widget->isVisibleTo(root) || widget->isWindow())) return;
            painter.save();
            painter.setClipRect(QRect(offset, widget->size()), Qt::IntersectClip);
            const QImage native = nativeImage ? nativeImage(widget, pixelRatio) : QImage();
            if (!native.isNull()) {
                painter.drawImage(QRect(offset, widget->size()), native);
            } else {
                widget->render(&painter, offset, QRegion(widget->rect()),
                        widget == root ? QWidget::DrawWindowBackground : QWidget::RenderFlags{});
                for (auto* object : widget->children()) {
                    if (auto* child = qobject_cast<QWidget*>(object)) paint(child, offset + child->pos());
                }
            }
            painter.restore();
        };
        paint(root, {});
        painter.end();
        return image;
    }

  private:
    struct Client {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
        int deck = -1;
        QElapsedTimer seen, frameClock;
        QSize sourceSize;
        QRect imageRect;
        QString epoch;
        int sequence = 0;
        QPointer<QWidget> pressed;
    };
    std::array<QPointer<QWidget>, 2> m_roots;
    QList<std::shared_ptr<Client>> m_clients;
    QLocalServer m_server;
    QTimer m_watchdog;
    NativeImage m_nativeImage;
    std::function<void()> m_synchronize;
    std::unique_ptr<QLockFile> m_lock;

    static QJsonObject failure(const char* reason) {
        return {{"ok", false}, {"reason", reason}, {"bytes", 0}};
    }
    static void reply(Client& client, QJsonObject result, const QByteArray& pixels = {}) {
        if (!client.socket) return;
        result.insert("bytes", pixels.size());
        if (client.socket->bytesToWrite() + pixels.size() > 4 * 1024 * 1024) {
            client.socket->abort(); return;
        }
        client.socket->write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
        if (!pixels.isEmpty()) client.socket->write(pixels);
    }
    QString epoch(const Client& client) const {
        auto* root = client.deck >= 0 ? m_roots[client.deck].data() : nullptr;
        return root ? root->property("quietvinylDisplayEpoch").toString() : QString();
    }
    bool validView(const Client& client) const {
        auto* root = client.deck >= 0 ? m_roots[client.deck].data() : nullptr;
        return root && root->isVisible() && client.sourceSize == root->size() &&
                client.epoch == epoch(client) && !QApplication::activeModalWidget() &&
                !QApplication::activePopupWidget();
    }
    static void cancel(Client& client) {
        if (!client.pressed) return;
        // TouchCancel reaches the native gesture/hold filters without a release
        // that could accidentally click, seek or select after disconnect.
        for (QWidget* widget = client.pressed; widget; widget = widget->parentWidget()) {
            QTouchEvent cancelEvent(QEvent::TouchCancel);
            QApplication::sendEvent(widget, &cancelEvent);
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) button->setDown(false);
            if (QScroller::hasScroller(widget)) QScroller::scroller(widget)->stop();
        }
        client.pressed.clear();
    }
    void request(Client& client, const QJsonObject& req) {
        const auto op = req.value("op").toString();
        if (op == "bind") {
            const int deck = req.value("deck").toInt() - 1;
            if (client.deck >= 0 || deck < 0 || deck > 1 || !m_roots[deck]) {
                reply(client, failure("invalid-deck")); return;
            }
            for (const auto& other : m_clients) {
                if (other.get() != &client && other->deck == deck) {
                    reply(client, failure("deck-already-bound")); return;
                }
            }
            client.deck = deck;
            reply(client, {{"ok", true}, {"version", 1}, {"width", Width}, {"height", Height}});
            return;
        }
        if (client.deck < 0 || !m_roots[client.deck]) { reply(client, failure("not-bound")); return; }
        // Apply external MIDI/view changes now, rather than waiting for the
        // presentation's next 20 ms timer tick before validating a touch.
        if (m_synchronize) m_synchronize();
        auto* root = m_roots[client.deck].data();
        if (op == "cancel") { cancel(client); reply(client, {{"ok", true}}); return; }
        if (op == "frame") {
            if (client.frameClock.isValid() && client.frameClock.elapsed() < 100) {
                reply(client, failure("frame-rate")); return;
            }
            if (!root->isVisible() || QApplication::activeModalWidget() || QApplication::activePopupWidget()) {
                cancel(client); reply(client, failure("display-unavailable")); return;
            }
            if (client.pressed && !validView(client)) cancel(client);
            // Rasterize directly at player density instead of enlarging an
            // already-rasterized HDMI screenshot.
            const qreal pixelRatio = qMin(qreal(Width) / root->width(), qreal(Height) / root->height());
            const auto native = captureTree(root, m_nativeImage, pixelRatio);
            if (native.isNull()) { reply(client, failure("capture-failed")); return; }
            const QSize size = native.size().scaled(Width, Height, Qt::KeepAspectRatio);
            client.sourceSize = root->size();
            client.imageRect = QRect(QPoint((Width - size.width()) / 2, (Height - size.height()) / 2), size);
            client.epoch = epoch(client);
            QImage canvas(Width, Height, QImage::Format_RGB888);
            canvas.fill(Qt::black);
            QPainter painter(&canvas);
            painter.drawImage(client.imageRect, native);
            painter.end();
            client.frameClock.start();
            const QByteArray pixels(reinterpret_cast<const char*>(canvas.constBits()), canvas.sizeInBytes());
            reply(client, {{"ok", true}, {"width", Width}, {"height", Height},
                    {"epoch", client.epoch}, {"format", "RGB"}}, pixels);
            return;
        }
        if (op != "touch") { reply(client, failure("unknown-operation")); return; }
        const int sequence = req.value("sequence").toInt();
        if (sequence <= client.sequence) { cancel(client); reply(client, failure("stale-sequence")); return; }
        client.sequence = sequence;
        if (!validView(client) || req.value("epoch").toString() != client.epoch) {
            cancel(client); reply(client, failure("stale-view")); return;
        }
        const double x = req.value("x").toDouble(-1), y = req.value("y").toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || !client.imageRect.contains(QPointF(x, y).toPoint())) {
            cancel(client); reply(client, failure("outside-image")); return;
        }
        const QPointF point((x - client.imageRect.x()) * client.sourceSize.width() / client.imageRect.width(),
                (y - client.imageRect.y()) * client.sourceSize.height() / client.imageRect.height());
        const auto phase = req.value("phase").toString();
        if (phase == "down") {
            cancel(client);
            client.pressed = root->childAt(point.toPoint());
            if (!client.pressed) client.pressed = root;
            if (!client.pressed->isEnabled()) { client.pressed.clear(); reply(client, failure("disabled")); return; }
        } else if ((phase != "move" && phase != "up") || !client.pressed) {
            reply(client, failure("invalid-gesture")); return;
        }
        auto* target = client.pressed.data();
        const QPointF local = target->mapFrom(root, point);
        const QPointF global = root->mapToGlobal(point);
        const auto type = phase == "down" ? QEvent::MouseButtonPress :
                phase == "up" ? QEvent::MouseButtonRelease : QEvent::MouseMove;
        QMouseEvent mouse(type, local, global,
                phase == "move" ? Qt::NoButton : Qt::LeftButton,
                phase == "up" ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &mouse);
        if (phase == "up") client.pressed.clear();
        reply(client, {{"ok", true}});
    }
};
