#include "widget/wsimplelibrary.h"

#include <algorithm>
#include <cmath>
#include <QApplication>
#include <QDebug>
#include <QItemSelectionModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QScroller>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "widget/browserpresentation.h"
#include <QStandardPaths>
#include "library/coverartutils.h"
#include "library/coverart.h"
#include "util/fileaccess.h"
#include "library/simplelibrarymodel.h"
#include "library/tidalplaylistlibrary.h"
#include "mixer/playerinfo.h"
#include "vinylcontrol/defs_vinylcontrol.h"
#include "widget/quietvinylremotebridge.h"
#include "moc_wsimplelibrary.cpp"

namespace {
bool minimalMode() {
    const ConfigKey key("[QuietVinyl]", "minimal_mode");
    return ControlObject::exists(key) && ControlObject::toBool(key);
}

class BrowserDelegate : public QStyledItemDelegate {
  public:
    BrowserDelegate(WSimpleLibrary* browser, QObject* parent)
            : QStyledItemDelegate(parent), m_browser(browser) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(200, 88);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        painter->save();
        const bool selected = option.state & QStyle::State_Selected;
        const bool folder = index.data(SimpleLibraryModel::NodeRole).toBool();
        const QColor accent(m_browser->targetDeck() == 1 ? "#e1c789" : "#81cfce");
        const QColor foreground(selected ? "#111713" : accent);
        painter->fillRect(option.rect, selected ? accent : QColor("#111713"));
        const QRect artRect(option.rect.left() + 8, option.rect.top() + 8, 72, 72);
        if (!folder) {
            const auto art = m_browser->rowArtwork(index);
            painter->fillRect(artRect, QColor("#26312b"));
            if (!art.isNull()) {
                const QRect target(QPoint(artRect.center().x() - art.width() / 2,
                        artRect.center().y() - art.height() / 2), art.size());
                painter->drawImage(target, art);
            } else {
                painter->setPen(QColor("#718176"));
                painter->drawEllipse(artRect.adjusted(18, 18, -18, -18));
            }
        }
        QRect text = option.rect.adjusted(folder ? 16 : 96, 0, folder ? -30 : -132, 0);
        QFont font = option.font;
        font.setPixelSize(20);
        font.setBold(selected);
        painter->setFont(font);
        painter->setPen(foreground);
        const QString subtitle = index.data(SimpleLibraryModel::SubtitleRole).toString();
        const QString title = painter->fontMetrics().elidedText(
                index.data().toString(), Qt::ElideRight, text.width());
        if (subtitle.isEmpty()) {
            painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, title);
        } else {
            painter->drawText(text.adjusted(0, 12, 0, -38), Qt::AlignVCenter | Qt::AlignLeft, title);
            font.setPixelSize(14);
            font.setBold(false);
            painter->setFont(font);
            painter->setPen(QColor(selected ? "#51452d" : "#aeb59d"));
            painter->drawText(text.adjusted(0, 48, 0, -12), Qt::AlignVCenter | Qt::AlignLeft,
                    painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, text.width()));
        }
        if (!folder) {
            font.setPixelSize(20);
            font.setBold(selected);
            painter->setFont(font);
            painter->setPen(foreground);
            const auto bpm = index.data(SimpleLibraryModel::BpmRole);
            const QString label = bpm.isValid() ? tr("%1 BPM").arg(bpm.toDouble(), 0, 'f', 1)
                                                : QStringLiteral("—");
            const auto state = index.data(SimpleLibraryModel::CacheStateRole).toString();
            painter->drawText(option.rect.adjusted(option.rect.width() - 128, state.isEmpty() ? 0 : -24, -12, 0),
                    Qt::AlignVCenter | Qt::AlignRight, label);
            if (!state.isEmpty()) {
                QString status = tr("Not cached");
                if (state == "unavailable") status = tr("Unavailable");
                else if (state == "cached") status = tr("Cached");
                else if (state == "queued") status = tr("Queued");
                else if (state == "checking") status = tr("Checking audio…");
                else if (state == "failed") status = tr("Failed · retry");
                else if (state == "downloading") status = tr("Downloading %1%").arg(index.data(SimpleLibraryModel::DownloadProgressRole).toInt());
                font.setPixelSize(11); font.setBold(false); painter->setFont(font);
                painter->drawText(option.rect.adjusted(option.rect.width() - 128, 34, -12, 0),
                        Qt::AlignVCenter | Qt::AlignRight, status);
                const int progress = index.data(SimpleLibraryModel::DownloadProgressRole).toInt();
                if (state == "downloading" && progress >= 0)
                    painter->fillRect(QRect(option.rect.left() + 96, option.rect.bottom() - 2,
                            int((option.rect.width() - 108) * progress / 100.0), 2), foreground);
            }
        }
        const double progress = m_browser->holdProgress(index);
        if (progress > 0) {
            painter->fillRect(QRect(option.rect.left() + 96, option.rect.bottom() - 5,
                    static_cast<int>((option.rect.width() - 108) * progress), 3), foreground);
        }
        if (folder) {
            painter->setPen(foreground);
            painter->drawText(option.rect.adjusted(0, 0, -12, 0),
                    Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("›"));
        }
        painter->restore();
    }
  private:
    WSimpleLibrary* m_browser;
};
} // namespace

WSimpleLibrary::WSimpleLibrary(QWidget* parent)
        : QWidget(parent), WBaseWidget(this),
          m_model(new SimpleLibraryModel(this)),
          m_breadcrumb(new BrowserHeader([this] {
                      if (m_model->canGoBack()) back(); else closeBrowser();
                  }, [this] { closeBrowser(); }, [this] {
                      if (m_loadingVinyl) return;
                      cancelTidal();
                      const ConfigKey mode("[QuietVinyl]", "minimal_mode");
                      if (ControlObject::exists(mode)) ControlObject::set(mode, minimalMode() ? 0 : 1);
                      closeBrowser();
                  }, this)), m_empty(new QLabel(tr("Empty"), this)),
          m_list(new BrowserTrackList([](const QModelIndex& index) {
                      return !index.data(SimpleLibraryModel::NodeRole).toBool();
                  }, [this](const QModelIndex& index) {
                      if (m_loadingVinyl || !index.isValid()) return;
                      m_model->selectRow(index.row()); enter();
                  }, this)) {
    m_deckIndicator = new DeckIndicator(m_targetDeck, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(4);
    m_breadcrumb->setObjectName(QStringLiteral("BrowserBreadcrumb"));
    m_breadcrumb->setFixedHeight(64);
    m_breadcrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_breadcrumb->setStyleSheet(QStringLiteral(
            "color:#aeb59d; background:#111713; padding-left:12px; font-size:12px;"));
    m_list->setObjectName(QStringLiteral("BrowserList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new BrowserDelegate(this, m_list));
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    QScroller::grabGesture(m_list->viewport(), QScroller::TouchGesture);
    connect(QScroller::scroller(m_list->viewport()), &QScroller::stateChanged, m_list,
            [this](QScroller::State state) {
                if (state == QScroller::Dragging || state == QScroller::Scrolling) m_list->cancelHold();
            });
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setStyleSheet(QStringLiteral("QListView { border:0; background:#111713; }"));
    m_list->setAccessibleName(tr("Music browser"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet(QStringLiteral("color:#aeb59d; font-size:16px;"));
    layout->addWidget(m_breadcrumb);
    m_vinylHint = new QLabel(this);
    m_vinylHint->setFixedHeight(20);
    m_vinylHint->setStyleSheet(QStringLiteral(
            "color:#aeb59d; background:#111713; padding-left:12px; font-size:11px;"));
    layout->addWidget(m_vinylHint);
    layout->addWidget(m_list, 1);
    m_confirmation = new BrowserLoadConfirmation([this] { confirmReplacement(); },
            [this] { cancelLoadConfirmation(); }, this);
    layout->addWidget(m_confirmation, 1);
    m_confirmation->hide();
    layout->addWidget(m_empty, 1);
    setFocusProxy(m_list);
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex& index) {
                if (index.isValid() && index.row() != m_model->selectedRow()) {
                    m_model->selectRow(index.row());
                }
            });
    connect(m_list, &QListView::clicked, this, [this](const QModelIndex& index) {
        if (!index.isValid() || m_loadingVinyl) return;
        m_model->selectRow(index.row());
        if (index.data(SimpleLibraryModel::NodeRole).toBool()) enter();
        else if (m_tidalLibrary && m_model->tidalSelection().isValid())
            m_tidalLibrary->download(m_model->tidalSelection());
    });
    connect(m_model, &SimpleLibraryModel::breadcrumbChanged, this, [this](const QString& text) {
        m_breadcrumbText = text;
        refreshBreadcrumb();
    });
    connect(m_model, &SimpleLibraryModel::selectionChanged, this, [this](int row) {
        const auto index = m_model->index(row, 0);
        // Let the selection model notify QListView, so the old row is repainted.
        if (index.isValid()) {
            m_list->selectionModel()->setCurrentIndex(
                    index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        } else {
            m_list->selectionModel()->clearSelection();
            m_list->selectionModel()->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
        }
        if (index.isValid()) m_list->scrollTo(index, QAbstractItemView::EnsureVisible);
        m_empty->setText(m_model->emptyText());
        m_empty->setVisible(row < 0);
        // Keep the focused list present even when empty, so keyboard release
        // events still reset Enter/Back controls after navigation.
    });
    m_art = new BrowserRowArt([](const QString& location) {
        if (location.startsWith(QStringLiteral("https://")))
            return BrowserRowArt::tidalCover(location,
                    QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tidal/artwork"));
        const mixxx::FileInfo file(location);
        auto image = CoverArtUtils::extractEmbeddedCover(mixxx::FileAccess(file));
        if (image.isNull()) {
            const auto covers = CoverArtUtils::findPossibleCoversInFolder(file.locationPath());
            const auto info = CoverArtUtils::selectCoverArtForTrack(file, QString(), covers);
            image = CoverInfo(info, location).loadImage().image;
        }
        return image;
    }, [this] { m_list->viewport()->update(); }, this);
    refreshDeckColors();
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset,
            this, [this] { m_gesture.reset(); cancelLoadConfirmation(); });
    connect(m_model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                if (!confirmingLoad() || roles.contains(SimpleLibraryModel::CacheStateRole)) return;
                if (m_confirmationSelection.row() >= first.row() && m_confirmationSelection.row() <= last.row())
                    cancelLoadConfirmation();
            });
    connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, this,
            [this](const QString& group, TrackPointer track, TrackPointer) {
                if (confirmingLoad() && group == m_confirmationGroup) cancelLoadConfirmation();
                if (track) finishVinylLoad(group);
            });
    m_motionClock.start();
    m_vinylClock.start();
    m_vinylTimer.setInterval(20);
    connect(&m_vinylTimer, &QTimer::timeout, this, &WSimpleLibrary::pollVinyl);
    m_vinylTimer.start();
    QuietVinylRemoteBridge::start(this);
}

WSimpleLibrary::~WSimpleLibrary() { cancelTidal(); cancelVinyl(); }

void WSimpleLibrary::cancelTidal() {
    if (m_tidalLibrary) m_tidalLibrary->cancel();
    m_loadingTidal = false;
    m_loadSelection = QPersistentModelIndex{};
    m_loadDestination.reset();
    m_loadApproved = false;
    m_list->setEnabled(true);
    m_tidalStatus.clear();
}

TidalPlaylistLibrary* WSimpleLibrary::tidalLibrary() const { return m_tidalLibrary; }

void WSimpleLibrary::setTidalLibrary(TidalPlaylistLibrary* library) {
    m_tidalLibrary = library;
    m_model->setTidalLibrary(library);
    connect(library, &TidalPlaylistLibrary::statusChanged, this, [this](const QString& text) {
        m_tidalStatus = text;
        m_empty->setText(m_model->emptyText());
    });
    connect(library, &TidalPlaylistLibrary::preparationFailed, this, [this] {
        emit tidalLoadFailed(m_tidalLibrary->status());
        m_loadingTidal = false;
        m_list->setEnabled(true);
    });
    connect(library, &TidalPlaylistLibrary::prepared, this, [this](const QString& path, const QString& group) {
        const QString expected = QStringLiteral("[Channel%1]").arg(m_targetDeck);
        if (!m_loadingTidal || (!isVisible() && !property("quietvinylRemoteLoad").toBool()) || group != expected) return;
        const ConfigKey page("[QuietVinyl]", "deck_page");
        if (!property("quietvinylRemoteLoad").toBool() && ControlObject::exists(page) && int(ControlObject::get(page)) != m_targetDeck - 1) {
            cancelTidal();
            return;
        }
        if (!m_loadSelection.isValid() || m_loadSelection != m_model->index(m_model->selectedRow(), 0) ||
                PlayerInfo::instance().getTrackInfo(group) != m_loadDestination) {
            cancelTidal(); emit tidalLoadFailed(tr("Selection or destination changed. Select again.")); return;
        }
        if (!m_loadApproved && needsLoadConfirmation(group)) {
            const bool remote = property("quietvinylRemoteLoad").toBool();
            cancelTidal();
            if (remote) emit tidalLoadFailed(tr("Record started moving. Select again to confirm replacement."));
            else requestSelectedLoad(group, false);
            return;
        }
        cancelTidal();
        // Downloading never stops the old song. Only the final, verified file
        // enters the normal DVS handoff after the target deck is rechecked.
        if (ControlObject::exists(ConfigKey(group, "vinylbrowse_capture")) &&
                ControlObject::toBool(ConfigKey(group, "vinylcontrol_enabled"))) {
            m_capturedDeck = m_targetDeck;
            ControlObject::set(ConfigKey(group, "vinylbrowse_capture"), 1.0);
            ControlObject::set(ConfigKey(group, "play"), 0.0);
            m_loadingVinyl = true;
            m_loadClock.start();
        }
        emit loadTidalTrack(path, group);
        if (!m_loadingVinyl) ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0.0);
    });
}

void WSimpleLibrary::cancelVinyl() {
    if (m_capturedDeck) {
        qInfo() << "Vinyl browse hold ended on deck" << m_capturedDeck;
        ControlObject::set(ConfigKey(QStringLiteral("[Channel%1]").arg(m_capturedDeck),
                "vinylbrowse_capture"), 0.0);
    }
    m_capturedDeck = 0;
    m_loadingVinyl = false;
    m_gesture.reset();
    m_lastVinylFrame = -1;
}

void WSimpleLibrary::finishVinylLoad(const QString& group) {
    if (!m_loadingVinyl || group != QStringLiteral("[Channel%1]").arg(m_capturedDeck)) return;
    qInfo() << "Vinyl browse load complete" << group;
    // PlayerInfo reports completion after Mixxx has restored the new track's cues.
    // Hold transport until that point; do not release a previously loaded song.
    // Keep play stopped until the decoder accepts Absolute; Mixxx rejects
    // switching from Relative to Absolute while play is set. The timecode
    // decoder will position and start the new song from the needle signal.
    ControlObject::set(ConfigKey(group, "play"), 0.0);
    ControlObject::set(ConfigKey(group, "vinylcontrol_mode"), MIXXX_VCMODE_ABSOLUTE);
    cancelVinyl();
    window()->setWindowTitle(tr("QuietVinyl · C/D: scroll · Booth: select/back/close · B: browse this deck"));
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0.0);
}

void WSimpleLibrary::pollVinyl() {
    updateRecordMotion();
    // Stopping/lifting the record dismisses the prompt, never approves a load.
    if (confirmingLoad() && !needsLoadConfirmation(m_confirmationGroup)) cancelLoadConfirmation();
    updateVinyl();
    if (confirmingLoad()) { m_vinylHint->setText(tr("Record moving · Tap to replace · Stop/lift to return to the list")); return; }
    if (m_model->isTidalContext()) {
        if (!m_tidalStatus.isEmpty()) m_vinylHint->setText(m_tidalStatus);
        else if (minimalMode()) m_vinylHint->setText(tr("Tap to download · Hold a Cached song 0.7s to load · Header → back / ↓ close"));
    }
}

void WSimpleLibrary::updateVinyl() {
    const double seconds = m_vinylClock.restart() / 1000.0;
    const ConfigKey fader("[QuietVinyl]", "browse_fader");
    const bool active = isVisible() && !confirmingLoad() && !m_loadingVinyl && !m_loadingTidal && !QApplication::activeModalWidget() &&
            !QApplication::activePopupWidget();
    const bool selecting = m_controllerSelection.isValid();
    const ConfigKey held("[QuietVinyl]", "browse_buttons_active");
    const bool buttonScrolling = ControlObject::exists(held) && ControlObject::toBool(held);
    const int rows = selecting || buttonScrolling ? 0 : m_fader.update(ControlObject::exists(fader) ? ControlObject::get(fader) : -1,
            seconds, active);
    if (rows) moveSelection(rows);
    if (!isVisible()) {
        if (!property("quietvinylRemoteLoad").toBool()) cancelVinyl();
        return;
    }
    if (confirmingLoad()) { m_gesture.reset(); return; }
    if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) {
        m_gesture.reset();
        return;
    }
    if (m_loadingVinyl) {
        m_vinylHint->setText(m_loadClock.elapsed() < 10000 ? tr("Loading selected track…")
                : tr("Load did not finish · Back to cancel, or use the encoder"));
        return;
    }
    if (m_loadingTidal) {
        const ConfigKey page("[QuietVinyl]", "deck_page");
        if (ControlObject::exists(page) && int(ControlObject::get(page)) != m_targetDeck - 1) {
            cancelTidal();
            return;
        }
        m_vinylHint->setText(m_tidalStatus);
        return;
    }
    // Buttons own the highlight while held; Booth freezes it until its gesture resolves.
    if (selecting || buttonScrolling) return;
    if (minimalMode()) {
        // Browsing must not take the record away from transport in minimal mode.
        cancelVinyl();
        m_vinylHint->setText(tr("Tap folders · Hold song 0.7s, release to load · Header → back / ↓ close / double-tap view"));
        return;
    }
    const auto selected = m_model->index(m_model->selectedRow(), 0);
    if (!selected.isValid()) {
        m_gesture.reset();
        m_lastVinylFrame = -1;
        m_vinylHint->setText(tr("Empty folder · Booth double-tap/Back: up"));
        return;
    }
    const QString group = QStringLiteral("[Channel%1]").arg(m_targetDeck);
    if (!ControlObject::exists(ConfigKey(group, "vinylbrowse_frame")) ||
            !ControlObject::toBool(ConfigKey(group, "vinylcontrol_enabled"))) {
        cancelVinyl();
        m_vinylHint->setText(tr("Connect control vinyl · Encoder available"));
        return;
    }
    if (!m_capturedDeck) {
        m_capturedDeck = m_targetDeck;
        qInfo() << "Vinyl browse motion captured; playback preserved" << group;
        ControlObject::set(ConfigKey(group, "vinylbrowse_capture"), 2.0);
    }
    if (m_fader.scrolling()) {
        m_gesture.reset();
        m_vinylHint->setText(tr("Crossfader browsing · Center to stop · Hold song and release to load"));
        return;
    }
    const double frame = ControlObject::get(ConfigKey(group, "vinylbrowse_frame"));
    const double validity = ControlObject::get(ConfigKey(group, "vinylbrowse_valid"));
    const bool valid = frame != m_lastVinylFrame && validity > 0;
    m_lastVinylFrame = frame;
    const bool wasLocked = m_gesture.locked();
    const auto action = m_gesture.update(
            ControlObject::get(ConfigKey(group, "vinylbrowse_pitch")), valid, seconds); // Track selection remains explicit.
    if (wasLocked != m_gesture.locked()) {
        qInfo() << "Vinyl browse highlight" << (m_gesture.locked() ? "locked" : "unlocked") << group;
    }
    m_vinylHint->setText(!valid ? tr("Waiting for timecode · Booth tap or encoder selects")
            : m_gesture.locked() ? tr("Selection locked · Booth tap selects · Move vinyl backwards to browse")
                                 : tr("Move vinyl to browse · Release to lock · Booth tap selects"));
    if (action.rows) m_model->moveSelection(action.rows);
}

void WSimpleLibrary::setTargetDeck(int deck) {
    if (m_loadingVinyl || (deck != 1 && deck != 2)) return;
    if (deck != m_targetDeck) { cancelLoadConfirmation(); cancelTidal(); }
    const ConfigKey page("[QuietVinyl]", "deck_page");
    if (ControlObject::exists(page)) ControlObject::set(page, deck - 1);
    if (m_targetDeck == deck) return;
    cancelVinyl();
    m_targetDeck = deck;
    refreshDeckColors();
}

void WSimpleLibrary::refreshDeckColors() {
    m_deckIndicator->setDeck(m_targetDeck);
    m_deckIndicator->positionAtTopRight();
    const QString accent = m_targetDeck == 1 ? QStringLiteral("#e1c789")
                                             : QStringLiteral("#81cfce");
    m_breadcrumb->setStyleSheet(QStringLiteral(
            "color:%1; background:#111713; padding-left:16px; padding-right:88px; font-size:12px; font-weight:bold;")
                    .arg(accent));
    m_list->viewport()->update();
    refreshBreadcrumb();
}

void WSimpleLibrary::refreshBreadcrumb() {
    const QString label = m_breadcrumbText;
    m_breadcrumb->setText(m_breadcrumb->fontMetrics().elidedText(
            label, Qt::ElideMiddle, std::max(0, m_breadcrumb->contentsRect().width())));
    m_breadcrumb->setToolTip(tr("Deck %1 · %2").arg(m_targetDeck).arg(label));
}

void WSimpleLibrary::moveSelection(int steps) {
    if (m_loadingVinyl || m_loadingTidal || confirmingLoad()) return;
    m_tidalStatus.clear();
    m_gesture.reset();
    m_model->moveSelection(steps);
}
void WSimpleLibrary::setControllerSelectionPending(bool pending) {
    if (pending && confirmingLoad()) m_confirmation->cancelPendingTap();
    m_controllerSelection = pending && isVisible() && (!justOpened() || confirmingLoad()) &&
            (!confirmingLoad() || m_confirmation->ready()) && !m_loadingVinyl && !m_loadingTidal
            ? m_model->index(m_model->selectedRow(), 0) : QModelIndex{};
    m_controllerSelectionDeck = m_targetDeck;
}
void WSimpleLibrary::confirmControllerSelection() {
    const auto selected = m_controllerSelection;
    m_controllerSelection = QPersistentModelIndex{};
    if (isVisible() && selected.isValid() && m_controllerSelectionDeck == m_targetDeck &&
            selected == m_model->index(m_model->selectedRow(), 0)) enter();
}
void WSimpleLibrary::updateRecordMotion() {
    const double seconds = m_motionClock.restart() / 1000.0;
    for (int deck = 1; deck <= 2; ++deck) {
        const QString group = QStringLiteral("[Channel%1]").arg(deck);
        const bool enabled = ControlObject::exists(ConfigKey(group, "vinylcontrol_record_moving")) &&
                ControlObject::exists(ConfigKey(group, "vinylbrowse_frame")) &&
                ControlObject::toBool(ConfigKey(group, "vinylcontrol_enabled"));
        if (!enabled) { m_recordMotion[deck - 1].reset(); continue; }
        m_recordMotion[deck - 1].update(true,
                ControlObject::get(ConfigKey(group, "vinylbrowse_frame")),
                ControlObject::toBool(ConfigKey(group, "vinylcontrol_record_moving")), seconds);
    }
}

bool WSimpleLibrary::needsLoadConfirmation(const QString& group) {
    updateRecordMotion();
    const int deck = group == "[Channel1]" ? 1 : group == "[Channel2]" ? 2 : 0;
    return PlayerInfo::instance().getTrackInfo(group) && recordMoving(deck);
}

void WSimpleLibrary::cancelLoadConfirmation() {
    if (!confirmingLoad()) return;
    m_confirmationGroup.clear();
    m_confirmationSelection = QPersistentModelIndex{};
    m_confirmationDestination.reset();
    m_controllerSelection = QPersistentModelIndex{};
    m_confirmation->hide();
    m_list->show();
    m_list->setFocus(Qt::OtherFocusReason);
}

void WSimpleLibrary::confirmReplacement() {
    if (!confirmingLoad() || !m_confirmation->ready()) return;
    const auto selection = m_confirmationSelection;
    const auto destination = m_confirmationDestination;
    const auto group = m_confirmationGroup;
    const bool play = m_confirmationPlay;
    const bool valid = selection.isValid() && selection == m_model->index(m_model->selectedRow(), 0) &&
            group == QStringLiteral("[Channel%1]").arg(m_targetDeck) &&
            destination == PlayerInfo::instance().getTrackInfo(group);
    cancelLoadConfirmation();
    if (valid) requestSelectedLoad(group, play, true);
}

void WSimpleLibrary::enter() {
    if (confirmingLoad()) { confirmReplacement(); return; }
    if (m_loadingVinyl || m_loadingTidal) return;
    const auto selected = m_model->index(m_model->selectedRow(), 0);
    if (!selected.isValid()) return;
    const QString group = QStringLiteral("[Channel%1]").arg(m_targetDeck);
    if (selected.data(SimpleLibraryModel::NodeRole).toBool()) { m_model->enter(group); return; }
    requestSelectedLoad(group, false);
}

void WSimpleLibrary::requestSelectedLoad(const QString& group, bool play, bool replacementApproved) {
    if (m_loadingVinyl || m_loadingTidal || confirmingLoad() || !m_model->canLoadSelected()) return;
    if (justOpened() && !property("quietvinylRemoteLoad").toBool() && !replacementApproved) return;
    const auto selected = m_model->index(m_model->selectedRow(), 0);
    const auto tidal = m_model->tidalSelection();
    if (tidal.isValid() && !m_tidalLibrary->cached(tidal)) {
        m_model->loadSelected(group, false); return; // downloads never replace audio
    }
    if (!replacementApproved && needsLoadConfirmation(group)) {
        if (property("quietvinylRemoteLoad").toBool()) {
            emit tidalLoadFailed(tr("Confirm replacement before loading this deck.")); return;
        }
        if (!isVisible()) resumeBrowsing();
        m_confirmationGroup = group;
        m_confirmationPlay = play;
        m_confirmationSelection = selected;
        m_confirmationDestination = PlayerInfo::instance().getTrackInfo(group);
        m_controllerSelection = QPersistentModelIndex{};
        m_list->cancelHold(); m_gesture.reset(); m_tidalStatus.clear();
        m_confirmation->setTracks(m_targetDeck, m_confirmationDestination->getTitle(), selected.data().toString());
        m_list->hide(); m_confirmation->show();
        return;
    }
    m_loadSelection = selected;
    m_loadDestination = PlayerInfo::instance().getTrackInfo(group);
    m_loadApproved = replacementApproved;
    if (tidal.isValid()) {
        m_loadingTidal = true;
        m_list->cancelHold();
        m_model->loadSelected(group, false);
        return;
    }
    if (ControlObject::exists(ConfigKey(group, "vinylbrowse_capture")) &&
            ControlObject::toBool(ConfigKey(group, "vinylcontrol_enabled"))) {
        m_capturedDeck = m_targetDeck;
        ControlObject::set(ConfigKey(group, "vinylbrowse_capture"), 1.0);
        ControlObject::set(ConfigKey(group, "play"), 0.0);
        m_gesture.reset(); m_loadingVinyl = true; m_loadClock.start();
        m_model->loadSelected(group, false);
        return;
    }
    cancelVinyl();
    m_model->loadSelected(group, play);
    if (!property("quietvinylRemoteLoad").toBool())
        ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0.0);
}
void WSimpleLibrary::back() {
    if (confirmingLoad()) { cancelLoadConfirmation(); return; }
    cancelTidal();
    if (m_loadingVinyl) cancelVinyl();
    m_gesture.reset();
    m_model->back();
    resumeBrowsing();
}
void WSimpleLibrary::activateForDeck(int deck) {
    if ((deck != 1 && deck != 2) || m_loadingVinyl) return;
    if (isVisible() && m_targetDeck == deck) {
        if (justOpened()) return;
        enter();
        return;
    }
    setTargetDeck(deck);
    resumeBrowsing();
}
void WSimpleLibrary::resumeBrowsing() {
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 1.0);
    // Run after the browser's showEvent has cleared the previous gesture.
    QTimer::singleShot(0, this, [this] {
        if (!isVisible()) return;
        m_gesture.reset();
        m_lastVinylFrame = -1;
        const QString group = QStringLiteral("[Channel%1]").arg(m_targetDeck);
        if (!minimalMode() && ControlObject::exists(ConfigKey(group, "vinylbrowse_capture")) &&
                ControlObject::toBool(ConfigKey(group, "vinylcontrol_enabled"))) {
            m_capturedDeck = m_targetDeck;
            ControlObject::set(ConfigKey(group, "vinylbrowse_capture"), 2.0);
        }
        window()->setWindowTitle(tr("QuietVinyl"));
    });
}
void WSimpleLibrary::loadSelected(const QString& group, bool play, bool replacementApproved) {
    if ((group != "[Channel1]" && group != "[Channel2]") || m_loadingVinyl || m_loadingTidal || confirmingLoad()) return;
    setTargetDeck(group == QStringLiteral("[Channel2]") ? 2 : 1);
    requestSelectedLoad(group, play, replacementApproved);
}
void WSimpleLibrary::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    m_openedClock.start();
    m_deckIndicator->positionAtTopRight();
    cancelVinyl();
    m_art->clear();
    QTimer::singleShot(0, m_list, [this] { m_list->setFocus(Qt::OtherFocusReason); });
}
void WSimpleLibrary::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshBreadcrumb();
    m_deckIndicator->positionAtTopRight();
}

void WSimpleLibrary::hideEvent(QHideEvent* event) {
    cancelLoadConfirmation();
    cancelTidal();
    m_controllerSelection = QPersistentModelIndex{};
    cancelVinyl();
    m_fader.update(-1, 0, false);
    m_list->cancelHold();
    QWidget::hideEvent(event);
}

QImage WSimpleLibrary::rowArtwork(const QModelIndex& index) {
    const auto cover = index.data(SimpleLibraryModel::ArtworkRole).toString();
    return m_art->image(cover.isEmpty() ? index.data(SimpleLibraryModel::LocationRole).toString() : cover);
}

void WSimpleLibrary::closeBrowser() {
    if (m_loadingVinyl) return;
    cancelLoadConfirmation();
    cancelTidal();
    cancelVinyl();
    ControlObject::set(ConfigKey("[Skin]", "show_maximized_library"), 0.0);
}

double WSimpleLibrary::holdProgress(const QModelIndex& index) const {
    return m_list->holdProgress(index);
}
