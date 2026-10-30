#include "widget/touchdeckpanel.h"
#include "widget/browserpresentation.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QPixmap>
#include <QIcon>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStyle>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QString loopText(double bars) {
    if (bars == 0.25) return QStringLiteral("¼");
    return bars == 0.5 ? QStringLiteral("½") : QString::number(bars, 'g', 3);
}
QIcon viewToggleIcon(bool expand, const QColor& color) {
    QPixmap image(64, 64);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.translate(16, 16);
    const QPolygonF corner = expand
            ? QPolygonF{QPointF(-4, -12), QPointF(-12, -12), QPointF(-12, -4)}
            : QPolygonF{QPointF(-12, -4), QPointF(-4, -4), QPointF(-4, -12)};
    for (int i = 0; i < 4; ++i) {
        painter.drawPolyline(corner);
        painter.rotate(90);
    }
    return QIcon(image);
}

}

TouchDeckPanel::TouchDeckPanel(int deck, QWidget* waveform, Read read,
        Write write, TrackText trackText, CueLabels cueLabels, QWidget* parent, bool minimalView)
        : QWidget(parent), m_minimal(minimalView), m_deck(deck),
          m_group(QStringLiteral("[Channel%1]").arg(deck)),
          m_read(std::move(read)), m_write(std::move(write)),
          m_trackText(std::move(trackText)), m_cueLabels(std::move(cueLabels)) {
    m_bpmClock.start();
    // Keep the many ordinary skin widgets alien when waveform containers and
    // the small overlays create native windows. Otherwise Qt promotes sibling
    // stack pages/controls too, breaking their stacking and visibility.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    setObjectName(QStringLiteral("TouchDeckPage"));
    setProperty("quietvinylDeck", deck);
    setProperty("quietvinylMinimal", minimalView);
    m_reconnect = new QPushButton(tr("No audio device\nReconnect mixer"), this);
    m_reconnect->setObjectName("ReconnectMixer");
    m_reconnect->setFocusPolicy(Qt::NoFocus);
    m_reconnect->setAccessibleName(tr("Reconnect mixer: reload QuietVinyl to discover audio and MIDI devices"));
    m_reconnect->setStyleSheet("QPushButton { background:#e1c789; color:#101813; border:2px solid #f3dfa8; border-radius:6px; font-size:22px; padding:8px; }");
    m_reconnect->hide();
    connect(m_reconnect, &QPushButton::clicked, this, [this] {
        // Recheck at the action boundary; never interrupt a connected engine.
        if (m_read("[SoundManager]", "hardware_output_connected") > 0 ||
                m_read("[SoundManager]", "status") == 1 ||
                m_read("[QuietVinyl]", "reconnect_available") <= 0) return;
        m_reconnect->setEnabled(false);
        pulse("[QuietVinyl]", "reconnect_devices");
    });
    m_deckIndicator = new DeckIndicator(deck, this);
    m_deckIndicator->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_deckIndicator->setAccessibleName(tr("Show deck %1").arg(deck == 1 ? 2 : 1));
    m_deckIndicator->setToolTip(tr("Tap to select deck; hold two seconds for Recovery"));
    m_deckIndicator->setHoldAction([this] {
        if (m_read("[QuietVinyl]", "recovery_available") > 0) pulse("[QuietVinyl]", "recovery_open");
    });
    connect(m_deckIndicator, &QPushButton::clicked, this, [this] {
        if (m_deckIndicator->holdTriggered()) return;
        const bool fixed = property("quietvinylFixedDeck").toBool();
        if (fixed && m_read("[Skin]", "show_maximized_library") > 0) {
            pulse("[Library]", "BrowseClose");
            if (m_read("[Skin]", "show_maximized_library") > 0) return;
        }
        m_write("[QuietVinyl]", "deck_page", fixed ? m_deck - 1 : (m_deck == 1 ? 1 : 0));
    });
    m_viewToggle = new QPushButton(this);
    m_viewToggle->setObjectName(m_minimal ? "MinimalViewToggle" : "TouchMinimal");
    m_viewToggle->setFixedSize(64, 64);
    m_viewToggle->setFocusPolicy(Qt::NoFocus);
    m_viewToggle->setIconSize(QSize(32, 32));
    m_viewToggle->setIcon(viewToggleIcon(m_minimal, deck == 1 ? QColor("#e1c789") : QColor("#81cfce")));
    m_viewToggle->setStyleSheet("background:transparent; border:none; padding:0;");
    const QString toggleName = m_minimal ? tr("Switch to full view") : tr("Switch to minimal view");
    m_viewToggle->setAccessibleName(toggleName);
    m_viewToggle->setToolTip(toggleName);
    connect(m_viewToggle, &QPushButton::clicked, this, [this] {
        m_write("[QuietVinyl]", "minimal_mode", m_minimal ? 0 : 1);
    });
    m_dvsMode = new QPushButton(this);
    m_dvsMode->setObjectName("TouchDvsMode");
    m_dvsMode->setFocusPolicy(Qt::NoFocus);
    m_dvsMode->setFixedSize(64, 64);
    connect(m_dvsMode, &QPushButton::clicked, this, [this] {
        // Recheck at the action boundary: transport can change between polls.
        const double mode = m_read(m_group, "vinylcontrol_mode");
        if (m_read(m_group, "track_loaded") <= 0 ||
                m_read(m_group, "vinylcontrol_enabled") <= 0) return;
        if (mode == 0) m_write(m_group, "vinylcontrol_mode", 1);
        else if (mode == 1 && m_read(m_group, "play") <= 0 &&
                m_read(m_group, "loop_enabled") <= 0) {
            m_write(m_group, "vinylcontrol_mode", 0);
        }
        refresh();
    });
    m_keylock = new QPushButton(QStringLiteral("♪"), this);
    m_keylock->setObjectName("TouchKeylock");
    m_keylock->setFixedSize(64, 64);
    m_keylock->setFocusPolicy(Qt::NoFocus);
    m_keylock->setCheckable(true);
    connect(m_keylock, &QPushButton::clicked, this, [this] {
        // Read the engine again so a hardware change between polls is respected.
        m_write(m_group, "keylock", m_read(m_group, "keylock") > 0 ? 0 : 1);
        refresh();
    });
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 12, 20, 12);
    layout->setSpacing(8);
    if (m_minimal) {
        setObjectName("MinimalPlaybackPage");
        delete layout;
        auto* columns = new QHBoxLayout(this);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(0);
        // Minimal's skin-provided content is the native asynchronous cover-art
        // widget. Give it the full height; at 800x480 this is a 480px square.
        waveform->setObjectName("MinimalArtwork");
        waveform->setMinimumSize(0, 0);
        waveform->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        columns->addWidget(waveform, 3);
        auto* data = new QWidget(this);
        data->setObjectName("MinimalData");
        data->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        auto* details = new QVBoxLayout(data);
        details->setContentsMargins(24, 84, 24, 32);
        details->setSpacing(24);
        columns->addWidget(data, 2);
        auto* identity = new QWidget(data);
        auto* identityLayout = new QVBoxLayout(identity);
        identityLayout->setContentsMargins(0, 0, 0, 0);
        identityLayout->setSpacing(8);
        identityLayout->addStretch();
        m_title = new QLabel(identity);
        m_title->setObjectName("MinimalSong");
        m_title->setTextFormat(Qt::PlainText);
        m_title->setWordWrap(true);
        m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        identityLayout->addWidget(m_title);
        m_artist = new QLabel(identity);
        m_artist->setObjectName("MinimalArtist");
        m_artist->setTextFormat(Qt::PlainText);
        m_artist->setWordWrap(true);
        m_artist->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_artist->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        identityLayout->addWidget(m_artist);
        identityLayout->addStretch();
        details->addWidget(identity, 1);
        // Countdown positions always match the physical decks. The left one
        // overlays the artwork; the right one stays in the data column.
        for (int i = 0; i < 2; ++i) {
            m_remaining[i] = new QLabel(this);
            m_remaining[i]->setObjectName(QStringLiteral("MinimalRemaining%1").arg(i + 1));
            m_remaining[i]->setProperty("current", i == m_deck - 1);
            m_remaining[i]->setAlignment((i == 0 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
            m_remaining[i]->setMinimumWidth(0);
            m_remaining[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_remaining[i]->setFixedHeight(96);
        }
        details->addWidget(m_remaining[1]);
        m_loopStatus = new QLabel(this);
        m_loopStatus->setObjectName("MinimalLoopStatus");
        m_loopStatus->setAlignment((deck == 1 ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
        m_loopStatus->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* browse = m_browse = new QPushButton(tr("Browse"), this);
        browse->setObjectName("MinimalBrowse");
        browse->setAccessibleName(tr("Browse for deck %1").arg(deck));
        browse->setFocusPolicy(Qt::NoFocus);
        browse->setFixedHeight(64);
        connect(browse, &QPushButton::clicked, this, [this] {
            if (!property("quietvinylFixedDeck").toBool()) m_write("[QuietVinyl]", "deck_page", m_deck - 1);
            pulse("[Library]", QStringLiteral("BrowseDeck%1Activate").arg(m_deck));
        });
        details->addWidget(browse);
        // The quiet page has no navigation buttons: swipe its background or
        // text to change the selected deck. Browse remains an ordinary tap.
        QList<QWidget*> surfaces = {this, data, identity, m_title, m_artist, m_remaining[0], m_remaining[1], waveform};
        surfaces.append(waveform->findChildren<QWidget*>());
        for (auto* surface : surfaces) {
            surface->setAcceptDrops(false);
            surface->setAttribute(Qt::WA_AcceptTouchEvents);
            surface->installEventFilter(this);
        }
        m_timer.setInterval(100);
        connect(&m_timer, &QTimer::timeout, this, &TouchDeckPanel::refresh);
        refresh();
        return;
    }
    delete layout;
    // Preserve the plain header and small corner overlays above the waveform,
    // which reaches the performance controls.
    m_waveform = waveform;
    waveform->setParent(this);
    waveform->setMinimumSize(0, 0);
    m_controls = new QWidget(this);
    m_controls->setObjectName("TouchPerformanceControls");
    m_header = new QWidget(this);
    m_header->setObjectName("TouchTrackHeader");
    auto* header = new QHBoxLayout(m_header);
    header->setContentsMargins(20, 12, 20, 8);
    header->addStrut(64);
    auto* metadata = new QVBoxLayout;
    metadata->setSpacing(2);
    m_title = new QLabel(m_header);
    m_title->setObjectName("TouchTrackTitle");
    m_artist = new QLabel(m_header);
    m_artist->setObjectName("TouchTrackArtist");
    for (auto* label : {m_title, m_artist}) {
        label->setTextFormat(Qt::PlainText);
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        metadata->addWidget(label);
    }
    header->addLayout(metadata, 1);
    header->addSpacing(300);
    auto button = [this](const QString& text, const QString& name,
                          const QString& accessible) {
        auto* b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setAccessibleName(accessible);
        b->setMinimumHeight(48);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    // Swipes belong only to this read-only waveform area, never to buttons or
    // browser rows. Consume the original events before waveform seek/scratch.
    const auto children = waveform->findChildren<QWidget*>();
    waveform->setAttribute(Qt::WA_AcceptTouchEvents);
    waveform->installEventFilter(this);
    for (auto* child : children) {
        child->setAttribute(Qt::WA_AcceptTouchEvents);
        child->installEventFilter(this);
    }
    m_time = new QLabel(this);
    m_time->setObjectName("TouchTime");
    m_time->setFixedHeight(48);
    m_otherRemaining = new QLabel(this);
    m_otherRemaining->setObjectName("TouchOtherRemaining");
    m_otherRemaining->setFixedHeight(34);
    m_bpm = button({}, "TouchBpm", {});
    m_bpm->setFixedHeight(48);
    connect(m_bpm, &QPushButton::clicked, this, [this] {
        m_write("[QuietVinyl]", setting("show_bpm"),
                m_read("[QuietVinyl]", setting("show_bpm")) > 0 ? 0 : 1);
        refresh();
    });
    // Mixxx's OpenGL waveform uses a native child window. Native siblings are
    // needed above it on Wayland/X11; ordinary alien widgets paint underneath.
    const QList<QWidget*> overlays = {m_time, m_bpm, m_otherRemaining, m_reconnect};
    for (auto* overlay : overlays) {
        if (QGuiApplication::platformName() != "offscreen") {
            overlay->setAttribute(Qt::WA_DontCreateNativeAncestors);
            overlay->setAttribute(Qt::WA_NativeWindow);
        }
    }
    for (auto* label : {m_time, m_otherRemaining}) {
        label->setAttribute(Qt::WA_AcceptTouchEvents);
        label->installEventFilter(this);
    }

    auto* bottom = new QHBoxLayout(m_controls);
    bottom->setContentsMargins(8, 8, 8, 8);
    bottom->setSpacing(8);
    auto* navigation = new QVBoxLayout;
    navigation->setSpacing(8);
    auto* browse = m_browse = button("B", "TouchBrowse", tr("Browse for deck %1").arg(deck));
    m_loopTab = button("L", "TouchLoopTab", tr("Show loop controls"));
    m_cueTab = button("C", "TouchCueTab", tr("Show hot cues"));
    m_loopTab->setCheckable(true);
    m_cueTab->setCheckable(true);
    for (auto* b : {browse, m_loopTab, m_cueTab}) {
        b->setFixedWidth(64);
        b->setMinimumHeight(0);
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        navigation->addWidget(b, 1);
    }
    connect(browse, &QPushButton::clicked, this, [this] {
        if (!property("quietvinylFixedDeck").toBool()) m_write("[QuietVinyl]", "deck_page", m_deck - 1);
        pulse("[Library]", QStringLiteral("BrowseDeck%1Activate").arg(m_deck));
    });
    connect(m_loopTab, &QPushButton::clicked, this, [this] {
        m_write("[QuietVinyl]", setting("panel"), 0); refresh();
    });
    connect(m_cueTab, &QPushButton::clicked, this, [this] {
        m_write("[QuietVinyl]", setting("panel"), 1); refresh();
    });
    bottom->addLayout(navigation);
    m_panels = new QStackedWidget(this);
    auto* loops = new QWidget(this);
    auto* sizes = new QHBoxLayout(loops);
    sizes->setContentsMargins(0, 0, 0, 0);
    sizes->setSpacing(8);
    m_off = button(tr("Off"), "TouchLoopOff", tr("Turn loop off"));
    m_off->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_off->setMinimumHeight(0);
    connect(m_off, &QPushButton::clicked, this, [this] {
        m_write(m_group, "loop_enabled", 0); refresh();
    });
    for (int i = 0; i < 4; ++i) {
        m_sizes[i] = button({}, QStringLiteral("TouchLoopSlot%1").arg(i + 1), {});
        m_sizes[i]->setMinimumWidth(0);
        m_sizes[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        m_sizes[i]->setCheckable(true);
        connect(m_sizes[i], &QPushButton::clicked, this, [this, i] { selectLoop(i); });
        sizes->addWidget(m_sizes[i], 1);
    }
    m_panels->addWidget(loops);
    auto* cues = new QWidget(this);
    auto* cueLayout = new QGridLayout(cues);
    cueLayout->setContentsMargins(0, 0, 0, 0);
    cueLayout->setSpacing(8);
    for (int i = 0; i < 8; ++i) {
        m_cues[i] = button({}, QStringLiteral("TouchCue%1").arg(i + 1), {});
        m_cues[i]->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        m_cues[i]->setMinimumWidth(0);
        connect(m_cues[i], &QPushButton::clicked, this, [this, i] {
            const QString prefix = QStringLiteral("hotcue_%1").arg(i + 1);
            if (m_read(m_group, "track_loaded") > 0 && m_read(m_group, prefix + "_status") > 0) {
                // A cue is a deliberate digital position change. Keep that
                // offset when the control record starts from a stopped deck,
                // too; the engine's seek fallback only handles playing decks.
                if (m_read(m_group, "vinylcontrol_enabled") > 0 &&
                        m_read(m_group, "vinylcontrol_mode") == 0) {
                    m_write(m_group, "vinylcontrol_mode", 1); // Absolute -> Relative
                }
                pulse(m_group, prefix + "_goto");
            }
        });
        cueLayout->addWidget(m_cues[i], i / 4, i % 4);
    }
    m_panels->addWidget(cues);
    bottom->addWidget(m_panels, 1);
    m_layers = new QWidget(this);
    m_layers->setFixedWidth(64);
    auto* layerLayout = new QVBoxLayout(m_layers);
    layerLayout->setContentsMargins(0, 0, 0, 0);
    layerLayout->setSpacing(8);
    for (int i = 0; i < 2; ++i) {
        const QString name = i == 0 ? "A" : "B";
        m_layerButtons[i] = button(name, "TouchLayer" + name, tr("Loop layer %1").arg(name));
        m_layerButtons[i]->setCheckable(true);
        m_layerButtons[i]->setMinimumHeight(0);
        m_layerButtons[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(m_layerButtons[i], &QPushButton::clicked, this, [this, i] {
            m_write("[QuietVinyl]", setting("loop_layer"), i); refresh();
        });
        layerLayout->addWidget(m_layerButtons[i], 1);
    }
    layerLayout->addWidget(m_off, 1);
    bottom->addWidget(m_layers);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &TouchDeckPanel::refresh);
    refresh();
}

QString TouchDeckPanel::setting(const QString& name) const {
    return QStringLiteral("deck%1_%2").arg(m_deck).arg(name);
}

double TouchDeckPanel::bars(int slot) const {
    static constexpr double layers[2][4] = {{0.25, 0.5, 1, 2}, {4, 8, 16, 32}};
    const int layer = m_read("[QuietVinyl]", setting("loop_layer")) > 0 ? 1 : 0;
    return layers[layer][slot];
}

void TouchDeckPanel::pulse(const QString& group, const QString& item) {
    m_write(group, item, 1);
    m_write(group, item, 0);
}

void TouchDeckPanel::selectLoop(int slot) {
    if (m_read(m_group, "track_loaded") <= 0) return;
    const double beats = bars(slot) * 4;
    const bool active = m_read(m_group, "loop_enabled") > 0;
    if (active && std::abs(m_read(m_group, "beatloop_size") - beats) < 0.001) {
        m_write(m_group, "loop_enabled", 0);
    } else {
        m_write(m_group, "beatloop_size", beats);
        if (!active) pulse(m_group, "beatloop_activate");
    }
    refresh();
}

void TouchDeckPanel::refresh() {
    positionViewControls();
    const bool canReconnect = m_read("[QuietVinyl]", "reconnect_available") > 0 &&
            m_read("[SoundManager]", "hardware_output_connected") <= 0 &&
            m_read("[SoundManager]", "status") != 1;
    m_reconnect->setGeometry(std::max(0, (width() - 300) / 2),
            std::max(0, (height() - 96) / 2), std::min(300, width()), 96);
    m_reconnect->setEnabled(canReconnect);
    m_reconnect->setVisible(canReconnect);
    const bool mixerSelected = m_read("[QuietVinyl]", "mixer_deck") == m_deck - 1;
    if (!m_browse->property("mixerSelected").isValid() ||
            m_browse->property("mixerSelected").toBool() != mixerSelected) {
        m_browse->setProperty("mixerSelected", mixerSelected);
        m_browse->style()->unpolish(m_browse);
        m_browse->style()->polish(m_browse);
        m_browse->update();
        m_browse->setAccessibleName(mixerSelected
                ? tr("Browse for deck %1, selected for mixer buttons").arg(m_deck)
                : tr("Browse for deck %1").arg(m_deck));
    }
    const bool fixed = property("quietvinylFixedDeck").toBool();
    m_deckIndicator->setAccessibleName(fixed ? tr("Use deck %1 for mixer and Denon controls").arg(m_deck)
                                            : tr("Show deck %1").arg(m_deck == 1 ? 2 : 1));
    const bool loaded = m_read(m_group, "track_loaded") > 0;
    const double mode = m_read(m_group, "vinylcontrol_mode");
    const bool dvs = m_read(m_group, "vinylcontrol_enabled") > 0;
    const bool canReturn = m_read(m_group, "play") <= 0 && m_read(m_group, "loop_enabled") <= 0;
    m_dvsMode->setText(mode == 0 ? "A" : mode == 1 ? "R" : "—");
    m_dvsMode->setEnabled(loaded && dvs && (mode == 0 || (mode == 1 && canReturn)));
    const QString modeHelp = !dvs ? tr("Vinyl control is off")
            : mode == 0 ? tr("Absolute mode: tap for Relative")
            : canReturn ? tr("Relative mode: tap for Absolute")
                        : tr("Relative mode: stop the record and exit the loop to select Absolute");
    m_dvsMode->setAccessibleName(modeHelp);
    m_dvsMode->setToolTip(modeHelp);
    const bool keylock = m_read(m_group, "keylock") > 0;
    m_keylock->setChecked(keylock);
    const QString keylockHelp = keylock ? tr("Keylock on: tap to turn off")
                                      : tr("Keylock off: tap to turn on");
    m_keylock->setAccessibleName(keylockHelp);
    m_keylock->setToolTip(keylockHelp);
    const auto text = m_trackText();
    m_title->setText(text.first.isEmpty() ? tr("No track loaded") : text.first);
    if (m_minimal) {
        m_artist->setText(loaded ? text.second : QString());
        m_artist->setVisible(!m_artist->text().isEmpty());
        for (int i = 0; i < 2; ++i) {
            const QString group = QStringLiteral("[Channel%1]").arg(i + 1);
            const double duration = std::max(0.0, m_read(group, "duration"));
            const double position = std::clamp(m_read(group, "playposition"), 0.0, 1.0);
            const int seconds = static_cast<int>(std::ceil(duration * (1 - position)));
            const bool loaded = m_read(group, "track_loaded") > 0;
            m_remaining[i]->setText(loaded ? QStringLiteral("−%1:%2")
                            .arg(seconds / 60, 2, 10, QChar('0'))
                            .arg(seconds % 60, 2, 10, QChar('0')) : QStringLiteral("—"));
            m_remaining[i]->setAccessibleName(tr("Deck %1 remaining time: %2")
                            .arg(i + 1).arg(m_remaining[i]->text()));
        }
        const double beats = m_read(m_group, "beatloop_size");
        const bool looping = loaded && m_read(m_group, "loop_enabled") > 0 &&
                std::isfinite(beats) && beats > 0;
        m_loopStatus->setText(looping ? QStringLiteral("L%1").arg(loopText(beats / 4)) : QString());
        m_loopStatus->setAccessibleName(looping
                ? tr("Deck %1: %2 bar loop active").arg(m_deck).arg(loopText(beats / 4)) : QString());
        m_loopStatus->setVisible(looping);
        positionLeftCountdown();
        if (canReconnect) m_reconnect->raise();
        return;
    }
    m_artist->setText(text.second);
    const bool showBpm = m_read("[QuietVinyl]", setting("show_bpm")) > 0;
    const double trackSamples = m_read(m_group, "track_samples");
    if (text != m_bpmTrackText || trackSamples != m_bpmTrackSamples) {
        m_bpmDisplay.reset();
        m_bpmTrackText = text;
        m_bpmTrackSamples = trackSamples;
    }
    const double bpm = m_bpmDisplay.update(m_read(m_group, "visual_bpm"),
            m_bpmClock.restart() / 1000.0, loaded);
    m_bpm->setText(!showBpm ? tr("BPM") : bpm > 0
                    ? tr("%1 BPM").arg(bpm, 0, 'f', 1) : QStringLiteral("— BPM"));
    m_bpm->setAccessibleName(showBpm ? tr("Hide BPM value") : tr("Show BPM value"));
    const double duration = std::max(0.0, m_read(m_group, "duration"));
    const double position = std::clamp(m_read(m_group, "playposition"), 0.0, 1.0);
    const int seconds = static_cast<int>(std::ceil(duration * (1 - position)));
    m_time->setText(m_read(m_group, "track_loaded") > 0
            ? QStringLiteral("−%1:%2").arg(seconds / 60, 2, 10, QChar('0'))
                    .arg(seconds % 60, 2, 10, QChar('0')) : QStringLiteral("—"));
    m_time->setAccessibleName(tr("Deck %1 remaining time: %2").arg(m_deck).arg(m_time->text()));
    const int otherDeck = m_deck == 1 ? 2 : 1;
    const QString otherGroup = QStringLiteral("[Channel%1]").arg(otherDeck);
    const double otherDuration = std::max(0.0, m_read(otherGroup, "duration"));
    const double otherPosition = std::clamp(m_read(otherGroup, "playposition"), 0.0, 1.0);
    const int remainingSeconds = static_cast<int>(std::ceil(otherDuration * (1 - otherPosition)));
    const QString otherTime = m_read(otherGroup, "track_loaded") > 0
            ? QStringLiteral("−%1:%2").arg(remainingSeconds / 60, 2, 10, QChar('0'))
                    .arg(remainingSeconds % 60, 2, 10, QChar('0'))
            : QStringLiteral("—");
    m_otherRemaining->setText(otherTime);
    m_otherRemaining->setAccessibleName(tr("Deck %1 remaining time: %2").arg(otherDeck).arg(otherTime));
    const bool active = m_read(m_group, "loop_enabled") > 0;
    const double beats = m_read(m_group, "beatloop_size");
    m_off->setVisible(active);
    const auto cues = m_cueLabels ? m_cueLabels() : QList<CueText>();
    for (int i = 0; i < 8; ++i) {
        const auto cue = cues.value(i);
        const bool available = loaded && cue.seconds >= 0 &&
                m_read(m_group, QStringLiteral("hotcue_%1_status").arg(i + 1)) > 0;
        const QString fullLabel = cue.label.isEmpty() ? tr("Cue %1").arg(i + 1) : cue.label;
        const auto label = m_cues[i]->fontMetrics().elidedText(fullLabel, Qt::ElideRight,
                std::max(20, m_cues[i]->width() - 24));
        const int seconds = available ? static_cast<int>(cue.seconds) : 0;
        const QString time = available ? QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QChar('0'))
                .arg(seconds % 60, 2, 10, QChar('0')) : tr("Empty");
        m_cues[i]->setText(QStringLiteral("%1 · %2\n%3").arg(i + 1).arg(label, time));
        m_cues[i]->setAccessibleName(available ? tr("Jump to cue %1: %2").arg(i + 1).arg(fullLabel)
                                                             : tr("Cue %1 empty").arg(i + 1));
        m_cues[i]->setToolTip(available ? fullLabel : QString());
        m_cues[i]->setEnabled(available);
        m_cues[i]->setStyleSheet(available && cue.color.isValid()
                ? QStringLiteral("border-left:4px solid %1;").arg(cue.color.name()) : QString());
    }
    for (int i = 0; i < 4; ++i) {
        const double size = bars(i);
        m_sizes[i]->setText(loopText(size));
        m_sizes[i]->setAccessibleName(tr("%1 bar loop").arg(loopText(size)));
        m_sizes[i]->setChecked(active && std::abs(beats - size * 4) < 0.001);
        m_sizes[i]->setEnabled(loaded);
    }
    const int panel = m_read("[QuietVinyl]", setting("panel")) > 0 ? 1 : 0;
    m_panels->setCurrentIndex(panel);
    m_loopTab->setChecked(panel == 0);
    m_cueTab->setChecked(panel == 1);
    m_layers->setVisible(panel == 0);
    for (int i = 0; i < 2; ++i) {
        m_layerButtons[i]->setChecked((m_read("[QuietVinyl]", setting("loop_layer")) > 0 ? 1 : 0) == i);
    }
    positionOverlays();
    if (canReconnect) m_reconnect->raise();
}

void TouchDeckPanel::finishSwipe(const QPointF& position) {
    if (!m_swiping) return;
    m_swiping = false;
    const QPointF delta = position - m_swipeStart;
    if (!property("quietvinylFixedDeck").toBool() && std::abs(delta.x()) >= 60 && std::abs(delta.x()) > std::abs(delta.y()) * 1.5) {
        m_write("[QuietVinyl]", "deck_page", delta.x() < 0 ? 1 : 0);
    }
}

bool TouchDeckPanel::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto* touch = static_cast<QTouchEvent*>(event);
        // Minimal's background accepts swipes. Let button taps fall through
        // to Qt's normal touch-to-mouse synthesis instead of swallowing them.
        if (event->type() == QEvent::TouchBegin && !touch->points().isEmpty() &&
                qobject_cast<QPushButton*>(childAt(mapFromGlobal(
                        touch->points().first().globalPosition().toPoint())))) {
            m_swiping = false;
            event->ignore();
            return false;
        }
        if (touch->points().size() != 1) m_swiping = false;
        else if (event->type() == QEvent::TouchBegin) {
            m_swipeStart = touch->points().first().globalPosition();
            m_swiping = true;
        } else if (event->type() == QEvent::TouchEnd) {
            finishSwipe(touch->points().first().globalPosition());
        }
        event->accept();
        return true;
    }
    case QEvent::TouchCancel:
        m_swiping = false;
        return true;
    case QEvent::MouseButtonPress: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return true;
        m_swipeStart = mouse->globalPosition();
        m_swiping = true;
        return true;
    }
    case QEvent::MouseButtonRelease:
        finishSwipe(static_cast<QMouseEvent*>(event)->globalPosition());
        return true;
    case QEvent::MouseMove:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
        return true;
    default:
        return QWidget::eventFilter(watched, event);
    }
}

void TouchDeckPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    m_timer.start();
}

void TouchDeckPanel::hideEvent(QHideEvent* event) {
    m_timer.stop();
    m_swiping = false;
    QWidget::hideEvent(event);
}

void TouchDeckPanel::positionLeftCountdown() {
    if (!m_minimal || !m_remaining[0]) return;
    // Align vertically with the 96px countdown above Browse in the right
    // column. A compact dark backing keeps text readable over any artwork.
    const int availableWidth = std::max(0, width() * 3 / 5 - 48);
    m_remaining[0]->setGeometry(24, std::max(0, height() - 216),
            std::min(availableWidth, m_remaining[0]->sizeHint().width()), 96);
    m_remaining[0]->raise();
    // Use the existing gap below this deck's countdown, without moving the
    // title, artist, timers or Browse button as loop state changes.
    auto* countdown = m_remaining[m_deck - 1];
    const QPoint origin = countdown->mapTo(this, QPoint());
    m_loopStatus->setGeometry(origin.x(), origin.y() + countdown->height(), countdown->width(), 24);
    m_loopStatus->raise();
}

void TouchDeckPanel::positionViewControls() {
    m_deckIndicator->positionAtTopRight();
    int nextX = m_deckIndicator->x() - 12;
    if (m_dvsMode) {
        m_dvsMode->move(nextX - m_dvsMode->width(), 12);
        m_dvsMode->raise();
        nextX = m_dvsMode->x() - 12;
    }
    m_keylock->move(nextX - m_keylock->width(), 12);
    m_keylock->raise();
    nextX = m_keylock->x() - 12;
    m_viewToggle->move(nextX - m_viewToggle->width(), 12);
    m_viewToggle->show();
    m_viewToggle->raise();
}

void TouchDeckPanel::positionOverlays() {
    if (m_minimal || !m_controls) return;
    const int half = height() / 2;
    const int headerHeight = 84;
    m_header->setGeometry(0, 0, width(), headerHeight);
    m_waveform->setGeometry(0, headerHeight, width(), std::max(0, half - headerHeight));
    m_controls->setGeometry(0, half, width(), height() - half);
    const int textWidth = std::max(40, m_viewToggle->x() - 28);
    const auto text = m_trackText();
    m_title->setText(m_title->fontMetrics().elidedText(
            text.first.isEmpty() ? tr("No track loaded") : text.first,
            Qt::ElideRight, textWidth - 12));
    m_artist->setText(m_artist->fontMetrics().elidedText(text.second, Qt::ElideRight, textWidth - 12));
    m_artist->setVisible(!text.second.isEmpty());
    auto place = [](QWidget* widget, int x, int y, int maxWidth) {
        const QSize size = widget->sizeHint();
        widget->setGeometry(x, y, std::min(maxWidth, size.width()), size.height());
        widget->raise();
    };
    const int valueY = half - 48 - 8;
    place(m_time, 20, valueY, width() / 2 - 24);
    const int bpmWidth = m_bpm->sizeHint().width();
    place(m_bpm, width() - 20 - bpmWidth,
            valueY, width() / 2 - 24);
    const int otherX = m_time->x() + m_time->width() + 12;
    auto baseline = [](QLabel* label) {
        const auto metrics = label->fontMetrics();
        return (label->height() - metrics.height()) / 2 + metrics.ascent();
    };
    const int otherY = valueY + baseline(m_time) - baseline(m_otherRemaining);
    place(m_otherRemaining, otherX, otherY, std::max(0, m_bpm->x() - 12 - otherX));
    positionViewControls();
}

void TouchDeckPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    positionLeftCountdown();
    positionViewControls();
    positionOverlays();
}
