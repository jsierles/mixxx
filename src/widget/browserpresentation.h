#pragma once

#include <QCache>
#include <QElapsedTimer>
#include <QImage>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QListView>
#include <QPersistentModelIndex>
#include <QTimer>
#include <QSet>
#include <QThreadPool>
#include <functional>

// One active-deck number; playback connects its button, browser leaves it passive.
class DeckIndicator : public QPushButton {
  public:
    explicit DeckIndicator(int deck, QWidget* parent);
    void setDeck(int deck);
    void positionAtTopRight();
};

// Visible navigation and a gesture surface share the same guarded browser actions.
class BrowserHeader : public QLabel {
  public:
    using Action = std::function<void()>;
    BrowserHeader(Action back, Action close, Action switchView, QWidget* parent = nullptr);
  protected:
    bool event(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
  private:
    QPushButton* m_backButton;
    QPushButton* m_closeButton;
    void finish(const QPointF& position);
    Action m_back, m_close, m_switchView;
    QPointF m_start, m_lastTapPosition;
    QElapsedTimer m_lastTap;
    bool m_tracking = false;
};

// A modal browser panel, preserving the list beneath it. A touch/mouse double
// tap cancels before a delayed single tap can confirm a replacement.
class BrowserLoadConfirmation : public QFrame {
  public:
    using Action = std::function<void()>;
    BrowserLoadConfirmation(Action confirm, Action back, QWidget* parent = nullptr);
    void setTracks(int deck, const QString& current, const QString& replacement);
    void cancelPendingTap();
    bool ready() const { return m_load->isEnabled(); }
  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
  private:
    void tap();
    Action m_confirm, m_back;
    QLabel* m_title;
    QLabel* m_message;
    QPushButton* m_load;
    QTimer m_tap, m_arm;
    QPointF m_touchStart;
    bool m_touchActive = false, m_touchMoved = false;
};

// Only small images live in this UI-thread cache. At most two workers and
// eight pending reads prevent fast scrolling from queuing the whole library.
class BrowserRowArt : public QObject {
  public:
    using Loader = std::function<QImage(const QString&)>;
    BrowserRowArt(Loader loader, std::function<void()> changed, QObject* parent = nullptr);
    QImage image(const QString& location);
    // Called by the bounded background workers, never the paint/audio thread.
    static QImage tidalCover(const QString& url, const QString& cacheDirectory);
    void clear();
  private:
    Loader m_loader;
    std::function<void()> m_changed;
    QCache<QString, QImage> m_cache{4096}; // KiB, includes cached missing covers
    QSet<QString> m_pending;
    quint64 m_generation = 0;
    QThreadPool m_pool;
};

class QListView;
// Pure rate integrator: re-center before browsing, no saved off-center motion.
class BrowserFader {
  public:
    int update(double position, double seconds, bool active);
    bool scrolling() const { return m_rate != 0; }
  private:
    bool m_armed = false;
    double m_fraction = 0, m_rate = 0;
};

// A song only loads on release after a stationary hold. Folder taps retain
// native QListView behavior; drag/scroll/model changes cancel a pending hold.
class BrowserTrackList : public QListView {
  public:
    using IsTrack = std::function<bool(const QModelIndex&)>;
    using Load = std::function<void(const QModelIndex&)>;
    BrowserTrackList(IsTrack isTrack, Load load, QWidget* parent = nullptr);
    void setModel(QAbstractItemModel* model) override;
    void cancelHold();
    double holdProgress(const QModelIndex& index) const;
  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool event(QEvent* event) override;
  private:
    void startHold(const QPointF& position);
    void moveHold(const QPointF& position);
    QModelIndex finishHold(const QPointF& position);
    bool m_touchActive = false;
    IsTrack m_isTrack;
    Load m_load;
    QPersistentModelIndex m_pressed;
    QPointF m_start;
    QElapsedTimer m_holdClock;
    QTimer m_feedback;
    QMetaObject::Connection m_resetConnection, m_selectionConnection;
};
