#include <QVBoxLayout>
#include <QHBoxLayout>
#include "widget/browserpresentation.h"

#include <QFutureWatcher>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QtConcurrentRun>
#include <QItemSelectionModel>
#include <QApplication>
#include <QWindow>
#include <algorithm>
#include <cmath>

DeckIndicator::DeckIndicator(int deck, QWidget* parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("ActiveDeckIndicator"));
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(48, 64);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setDeck(deck);
}
void DeckIndicator::setDeck(int deck) {
    if (property("deck").toInt() == deck) return;
    setProperty("deck", deck);
    setText(QString::number(deck));
    setAccessibleName(tr("Active deck %1").arg(deck));
    setStyleSheet(QStringLiteral("text-align:right; color:%1; background:transparent; border:none; padding:0; margin:0; font-size:48px; font-weight:400;")
                          .arg(deck == 1 ? QStringLiteral("#e1c789") : QStringLiteral("#81cfce")));
}
void DeckIndicator::positionAtTopRight() {
    move(std::max(0, parentWidget()->width() - width() - 20), 12);
    show();
    raise();
}

BrowserHeader::BrowserHeader(Action back, Action close, Action switchView, QWidget* parent)
        : QLabel(parent), m_back(std::move(back)), m_close(std::move(close)),
          m_switchView(std::move(switchView)) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAccessibleName(tr("Browser navigation: Back, Close, or swipe right back, down close, double tap switch view"));
    m_backButton = new QPushButton(tr("Back"), this);
    m_closeButton = new QPushButton(tr("Close"), this);
    m_backButton->setObjectName(QStringLiteral("BrowserBack"));
    m_closeButton->setObjectName(QStringLiteral("BrowserClose"));
    for (auto* button : {m_backButton, m_closeButton}) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(QStringLiteral(
                "background:#20281f; color:#e1c789; border:1px solid #556047;"
                "border-radius:3px; font-size:22px; padding:0;"));
    }
    // Keep the existing deck indicator clear at the far right. These children
    // travel with the browser when the dual-screen host changes its destination.
    setContentsMargins(88, 0, 104, 0);
    connect(m_backButton, &QPushButton::clicked, this, [this] {
        m_tracking = false; m_lastTap.invalidate(); m_back();
    });
    connect(m_closeButton, &QPushButton::clicked, this, [this] {
        m_tracking = false; m_lastTap.invalidate(); m_close();
    });
}

void BrowserHeader::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    m_backButton->setGeometry(4, 4, 96, height() - 8);
    m_closeButton->setGeometry(width() - 184, 4, 96, height() - 8);
}

void BrowserHeader::finish(const QPointF& position) {
    if (!m_tracking) return;
    m_tracking = false;
    const auto delta = position - m_start;
    if (delta.x() >= 40 && delta.x() > std::abs(delta.y()) * 1.5) {
        m_lastTap.invalidate(); m_back();
    } else if (delta.y() >= 40 && delta.y() > std::abs(delta.x()) * 1.5) {
        m_lastTap.invalidate(); m_close();
    } else if (std::abs(delta.x()) < 15 && std::abs(delta.y()) < 15) {
        if (m_lastTap.isValid() && m_lastTap.elapsed() < 350 &&
                (position - m_lastTapPosition).manhattanLength() < 30) {
            m_lastTap.invalidate(); m_switchView();
        } else {
            m_lastTapPosition = position; m_lastTap.start();
        }
    } else {
        m_lastTap.invalidate();
    }
}

bool BrowserHeader::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        m_tracking = mouse->button() == Qt::LeftButton;
        m_start = mouse->globalPosition();
        return true;
    }
    case QEvent::MouseButtonRelease:
        finish(static_cast<QMouseEvent*>(event)->globalPosition()); return true;
    case QEvent::MouseButtonDblClick:
        m_tracking = false; m_lastTap.invalidate(); m_switchView(); return true;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (touch->points().size() != 1) {
            m_tracking = false; m_lastTap.invalidate();
        } else if (event->type() == QEvent::TouchBegin) {
            m_tracking = true; m_start = touch->points().first().globalPosition();
        } else if (event->type() == QEvent::TouchEnd) {
            finish(touch->points().first().globalPosition());
        }
        event->accept(); return true;
    }
    case QEvent::TouchCancel:
    case QEvent::Hide:
        m_tracking = false; m_lastTap.invalidate(); break;
    default: break;
    }
    return QLabel::event(event);
}

BrowserLoadConfirmation::BrowserLoadConfirmation(Action confirm, Action back, QWidget* parent)
        : QFrame(parent), m_confirm(std::move(confirm)), m_back(std::move(back)),
          m_title(new QLabel(this)), m_message(new QLabel(this)),
          m_load(new QPushButton(tr("Load track"), this)) {
    setObjectName(QStringLiteral("BrowserLoadConfirmation"));
    setAccessibleName(tr("Confirm track replacement"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->addStretch();
    m_title->setObjectName(QStringLiteral("BrowserConfirmTitle"));
    m_message->setObjectName(QStringLiteral("BrowserConfirmMessage"));
    for (auto* label : {m_title, m_message}) {
        label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
        layout->addWidget(label);
    }
    auto* hint = new QLabel(tr("Booth: tap to confirm · Double-tap to go back"), this);
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* buttons = new QHBoxLayout;
    auto* cancel = new QPushButton(tr("Back"), this);
    cancel->setObjectName(QStringLiteral("CancelTrackReplacement"));
    m_load->setObjectName(QStringLiteral("ConfirmTrackReplacement"));
    cancel->setMinimumHeight(64); m_load->setMinimumHeight(64);
    buttons->addWidget(cancel); buttons->addWidget(m_load); layout->addLayout(buttons);
    layout->addStretch();
    m_tap.setSingleShot(true);
    m_arm.setSingleShot(true);
    connect(&m_arm, &QTimer::timeout, this, [this] { if (isVisible()) m_load->setEnabled(true); });
    connect(&m_tap, &QTimer::timeout, this, [this] { if (isVisible()) m_confirm(); });
    connect(m_load, &QPushButton::clicked, this, [this] { tap(); });
    connect(cancel, &QPushButton::clicked, this, [this] { cancelPendingTap(); m_back(); });
    m_load->setAttribute(Qt::WA_AcceptTouchEvents);
    m_load->installEventFilter(this);
}

void BrowserLoadConfirmation::setTracks(int deck, const QString& current, const QString& replacement) {
    cancelPendingTap();
    m_load->setEnabled(false); m_arm.start(350); // the opening gesture cannot also confirm
    m_title->setText(tr("Record on deck %1 is moving").arg(deck));
    m_message->setText(tr("The needle is reading a moving record. Loading replaces its current track.\n\n%1\n→ %2").arg(current.left(100), replacement.left(100)));
}

void BrowserLoadConfirmation::cancelPendingTap() { m_tap.stop(); m_touchActive = false; }
void BrowserLoadConfirmation::tap() {
    if (!isVisible() || !ready()) return;
    if (m_tap.isActive()) { cancelPendingTap(); m_back(); }
    else m_tap.start(250);
}
void BrowserLoadConfirmation::hideEvent(QHideEvent* event) { cancelPendingTap(); m_arm.stop(); QFrame::hideEvent(event); }
bool BrowserLoadConfirmation::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_load) return QFrame::eventFilter(watched, event);
    if (event->type() == QEvent::MouseButtonDblClick) {
        cancelPendingTap(); m_load->setDown(false); m_back(); return true;
    }
    if (event->type() == QEvent::TouchCancel) { cancelPendingTap(); event->accept(); return true; }
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (!touch->points().isEmpty()) {
            const auto point = touch->points().first().position();
            if (event->type() == QEvent::TouchBegin) { m_touchActive = true; m_touchMoved = false; m_touchStart = point; }
            else if ((point - m_touchStart).manhattanLength() > 12) m_touchMoved = true;
            if (event->type() == QEvent::TouchEnd && m_touchActive) {
                m_touchActive = false;
                if (!m_touchMoved && m_load->rect().contains(point.toPoint())) tap();
            }
        }
        event->accept(); return true;
    }
    return QFrame::eventFilter(watched, event);
}

BrowserRowArt::BrowserRowArt(Loader loader, std::function<void()> changed, QObject* parent)
        : QObject(parent), m_loader(std::move(loader)), m_changed(std::move(changed)) {
    m_pool.setMaxThreadCount(2);
}

void BrowserRowArt::clear() {
    ++m_generation;
    m_cache.clear();
    // Keep pending paths until their jobs finish; the eight-job limit still
    // applies across rapid hide/show or folder changes.
}

QImage BrowserRowArt::image(const QString& location) {
    if (location.isEmpty()) return {};
    if (auto* cached = m_cache.object(location)) return *cached;
    if (m_pending.contains(location) || m_pending.size() >= 8) return {};
    m_pending.insert(location);
    auto* watcher = new QFutureWatcher<QImage>(this);
    const auto generation = m_generation;
    connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, location, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_pending.remove(location);
        if (generation == m_generation) {
            m_cache.insert(location, new QImage(result),
                    std::max(1, static_cast<int>(result.sizeInBytes() / 1024)));
        }
        m_changed();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [loader = m_loader, location] {
        auto image = loader(location);
        return image.isNull() ? image : image.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }));
    return {};
}

QImage BrowserRowArt::tidalCover(const QString& address, const QString& cacheDirectory) {
    // Only public, fixed-size album images; no account headers or redirects.
    const QUrl url(address);
    static const QRegularExpression path(QStringLiteral(
            "^/images/[0-9a-f]{8}/[0-9a-f]{4}/[0-9a-f]{4}/[0-9a-f]{4}/[0-9a-f]{12}/320x320\\.jpg$"));
    if (url.scheme() != QStringLiteral("https") || url.host() != QStringLiteral("resources.tidal.com") ||
            !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment() ||
            !path.match(url.path()).hasMatch()) return {};
    constexpr int maxBytes = 1024 * 1024;
    const auto name = QCryptographicHash::hash(address.toUtf8(), QCryptographicHash::Sha256).toHex();
    const auto filename = QDir(cacheDirectory).filePath(QString::fromLatin1(name) + QStringLiteral(".jpg"));
    auto decode = [](QByteArray bytes) {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "JPEG");
        const auto size = reader.size();
        if (!size.isValid() || size.width() > 1280 || size.height() > 1280) return QImage{};
        reader.setScaledSize(size.scaled(320, 320, Qt::KeepAspectRatio));
        return reader.read();
    };
    QFile cached(filename);
    if (!QFileInfo(filename).isSymLink() && cached.size() > 0 && cached.size() <= maxBytes && cached.open(QIODevice::ReadOnly)) {
        const auto image = decode(cached.readAll());
        if (!image.isNull()) return image;
    }
    QNetworkAccessManager network;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(5000);
    auto* reply = network.get(request);
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QByteArray bytes;
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        bytes += reply->readAll();
        if (bytes.size() > maxBytes) reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { reply->abort(); loop.quit(); });
    deadline.start(5000);
    loop.exec();
    bytes += reply->readAll();
    if (reply->error() != QNetworkReply::NoError || bytes.size() > maxBytes ||
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return {};
    const auto image = decode(bytes);
    if (image.isNull()) return {};
    // Disk caching is optional. A full cache still displays the received image.
    QDir directory(cacheDirectory);
    qint64 stored = 0;
    for (const auto& file : directory.entryInfoList({QStringLiteral("*.jpg")}, QDir::Files)) stored += file.size();
    if (stored + bytes.size() <= 64 * 1024 * 1024 && QDir().mkpath(cacheDirectory)) {
        QSaveFile output(filename);
        if (output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size()) output.commit();
    }
    return image;
}

int BrowserFader::update(double position, double seconds, bool active) {
    if (!active || position < 0 || position > 127) {
        m_armed = false; m_fraction = m_rate = 0; return 0;
    }
    const double offset = position - 63.5;
    if (std::abs(offset) <= 5) {
        m_armed = true; m_fraction = m_rate = 0; return 0;
    }
    if (!m_armed) return 0;
    // Two predictable speeds: a wider center stop zone, fast halfway out.
    const double rate = (offset < 0 ? -1 : 1) * (std::abs(offset) >= 32 ? 10.0 : 4.0);
    if (m_rate * rate < 0) m_fraction = 0;
    m_rate = rate;
    // A delayed GUI tick must not produce a large catch-up jump.
    m_fraction += rate * std::clamp(seconds, 0.0, 0.1);
    const int rows = static_cast<int>(m_fraction);
    m_fraction -= rows;
    return rows;
}

BrowserTrackList::BrowserTrackList(IsTrack isTrack, Load load, QWidget* parent)
        : QListView(parent), m_isTrack(std::move(isTrack)), m_load(std::move(load)) {
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    // Observe raw touch before QScroller consumes it, while leaving all
    // scrolling events intact. Synthetic mouse events never create a hold.
    qApp->installEventFilter(this);
    m_feedback.setInterval(25);
    connect(&m_feedback, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
}

void BrowserTrackList::setModel(QAbstractItemModel* model) {
    cancelHold();
    disconnect(m_resetConnection); disconnect(m_selectionConnection);
    QListView::setModel(model);
    if (model) {
        m_resetConnection = connect(model, &QAbstractItemModel::modelAboutToBeReset,
                this, &BrowserTrackList::cancelHold);
        m_selectionConnection = connect(selectionModel(), &QItemSelectionModel::currentChanged,
                this, [this](const QModelIndex& current) {
                    if (m_pressed.isValid() && current != m_pressed) cancelHold();
                });
    }
}

void BrowserTrackList::cancelHold() {
    m_pressed = QPersistentModelIndex(); m_holdClock.invalidate(); m_feedback.stop();
    viewport()->update();
}

void BrowserTrackList::startHold(const QPointF& position) {
    cancelHold();
    const auto index = indexAt(position.toPoint());
    if (!index.isValid() || !m_isTrack(index)) return;
    m_pressed = index; m_start = position; m_holdClock.start(); m_feedback.start();
    setCurrentIndex(index);
}

void BrowserTrackList::moveHold(const QPointF& position) {
    if ((position - m_start).manhattanLength() > 12) cancelHold();
}

QModelIndex BrowserTrackList::finishHold(const QPointF& position) {
    const auto candidate = m_pressed;
    const bool ready = candidate.isValid() && m_holdClock.isValid() &&
            m_holdClock.elapsed() >= 700 && currentIndex() == candidate &&
            indexAt(position.toPoint()) == candidate && m_isTrack(candidate);
    cancelHold();
    return ready ? QModelIndex(candidate) : QModelIndex();
}

double BrowserTrackList::holdProgress(const QModelIndex& index) const {
    return m_pressed == index && m_holdClock.isValid()
            ? std::min(1.0, m_holdClock.elapsed() / 700.0) : 0;
}

void BrowserTrackList::mousePressEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventNotSynthesized && event->button() == Qt::LeftButton)
        startHold(event->position());
    QListView::mousePressEvent(event);
}
void BrowserTrackList::mouseMoveEvent(QMouseEvent* event) {
    if (event->source() == Qt::MouseEventNotSynthesized) moveHold(event->position());
    QListView::mouseMoveEvent(event);
}
void BrowserTrackList::mouseReleaseEvent(QMouseEvent* event) {
    const auto ready = event->source() == Qt::MouseEventNotSynthesized && event->button() == Qt::LeftButton
            ? finishHold(event->position()) : QModelIndex();
    QListView::mouseReleaseEvent(event);
    if (ready.isValid()) m_load(ready);
}

bool BrowserTrackList::eventFilter(QObject* watched, QEvent* event) {
    const bool ownSurface = watched == viewport() ||
            (window()->windowHandle() && watched == window()->windowHandle());
    if (!ownSurface) return false;
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate ||
            event->type() == QEvent::TouchEnd) {
        auto* touch = static_cast<QTouchEvent*>(event);
        if (touch->points().size() != 1) {
            m_touchActive = false; cancelHold(); return false;
        }
        const QPointF position = viewport()->mapFromGlobal(touch->points().first().globalPosition());
        if (event->type() == QEvent::TouchBegin) {
            if (!m_touchActive && isVisible() && viewport()->rect().contains(position.toPoint())) {
                m_touchActive = true; startHold(position);
            }
        } else if (m_touchActive && event->type() == QEvent::TouchUpdate) moveHold(position);
        else if (m_touchActive && event->type() == QEvent::TouchEnd) {
            m_touchActive = false;
            const QPersistentModelIndex ready = finishHold(position);
            if (ready.isValid()) QTimer::singleShot(0, this, [this, ready] {
                if (isVisible() && ready.isValid() && currentIndex() == ready && m_isTrack(ready)) m_load(ready);
            });
        }
    } else if (event->type() == QEvent::TouchCancel) {
        m_touchActive = false; cancelHold();
    }
    return false;
}

bool BrowserTrackList::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ContextMenu) return true;
    return QListView::viewportEvent(event);
}

bool BrowserTrackList::event(QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) {
        m_touchActive = false; cancelHold();
    }
    return QListView::event(event);
}
