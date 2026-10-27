#pragma once

#include "widget/bpmdisplayfilter.h"

#include <QElapsedTimer>
#include <QPointF>
#include <QTimer>
#include <QWidget>
#include <QColor>
#include <array>
#include <functional>

class DeckIndicator;
class QLabel;
class QPushButton;
class QStackedWidget;

// Native presentation around a skin-provided Mixxx waveform. The injected
// control boundary also lets the touch interactions run in Qt tests.
class TouchDeckPanel : public QWidget {
  public:
    using Read = std::function<double(const QString&, const QString&)>;
    using Write = std::function<void(const QString&, const QString&, double)>;
    using TrackText = std::function<QPair<QString, QString>()>;
    struct CueText { QString label; double seconds = -1; QColor color; };
    using CueLabels = std::function<QList<CueText>()>;
    TouchDeckPanel(int deck, QWidget* waveform, Read read, Write write,
            TrackText trackText, CueLabels cueLabels = {}, QWidget* parent = nullptr,
            bool minimal = false);
    void refresh();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void positionLeftCountdown();
    void positionViewControls();
    void positionOverlays();
    void pulse(const QString& group, const QString& item);
    void selectLoop(int slot);
    void finishSwipe(const QPointF& position);
    QString setting(const QString& name) const;
    double bars(int slot) const;
    bool m_minimal;
    int m_deck;
    QString m_group;
    Read m_read;
    Write m_write;
    TrackText m_trackText;
    CueLabels m_cueLabels;
    QTimer m_timer;
    QElapsedTimer m_bpmClock;
    BpmDisplayFilter m_bpmDisplay;
    QPair<QString, QString> m_bpmTrackText;
    double m_bpmTrackSamples = -1;
    DeckIndicator* m_deckIndicator;
    QPushButton* m_viewToggle;
    QPushButton* m_dvsMode = nullptr;
    QPushButton* m_keylock;
    QPushButton* m_browse;
    QPushButton* m_reconnect;
    QWidget* m_waveform = nullptr;
    QWidget* m_controls = nullptr;
    QWidget* m_header = nullptr;
    QLabel *m_title, *m_artist, *m_time;
    QLabel* m_otherRemaining = nullptr;
    QLabel* m_loopStatus = nullptr;
    std::array<QLabel*, 2> m_remaining{{nullptr, nullptr}};
    QPushButton *m_bpm, *m_off, *m_loopTab, *m_cueTab;
    QStackedWidget* m_panels;
    QWidget* m_layers;
    std::array<QPushButton*, 2> m_layerButtons;
    std::array<QPushButton*, 4> m_sizes;
    std::array<QPushButton*, 8> m_cues;
    QPointF m_swipeStart;
    bool m_swiping = false;
};
