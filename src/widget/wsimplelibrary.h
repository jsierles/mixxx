#pragma once

#include <QWidget>
#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointer>
#include <array>
#include "vinylcontrol/recordmotion.h"
#include "library/vinylbrowsegesture.h"
#include "widget/wbasewidget.h"
#include "track/track_decl.h"
#include "widget/browserpresentation.h"

class QLabel;
class BrowserRowArt;
class QListView;
class SimpleLibraryModel;
class TidalPlaylistLibrary;

class WSimpleLibrary : public QWidget, public WBaseWidget {
    Q_OBJECT
  public:
    explicit WSimpleLibrary(QWidget* parent);
    ~WSimpleLibrary() override;
    SimpleLibraryModel* browserModel() const { return m_model; }
    QListView* listView() const { return m_list; }
    void moveSelection(int steps);
    void enter();
    void setControllerSelectionPending(bool pending);
    void confirmControllerSelection();
    void closeBrowser();
    void back();
    void resumeBrowsing();
    void activateForDeck(int deck);
    void loadSelected(const QString& group, bool play, bool replacementApproved = false);
    bool needsLoadConfirmation(const QString& group);
    bool recordMoving(int deck) const { return deck >= 1 && deck <= 2 && m_recordMotion[deck - 1].active(); }
    bool confirmingLoad() const { return !m_confirmationGroup.isEmpty(); }
    void setTargetDeck(int deck);
    int targetDeck() const { return m_targetDeck; }
    QImage rowArtwork(const QModelIndex& index);
    double holdProgress(const QModelIndex& index) const;
    void setTidalLibrary(TidalPlaylistLibrary* library);
    TidalPlaylistLibrary* tidalLibrary() const;
    bool remoteLoadBusy() const { return m_loadingVinyl || m_loadingTidal || confirmingLoad(); }
    QString remoteLoadStatus() const { return m_loadingTidal ? m_tidalStatus : QString(); }
    bool cancelRemoteLoad() {
        if (!property("quietvinylRemoteLoad").toBool()) return false;
        if (m_loadingVinyl && m_loadClock.elapsed() < 10000) return false;
        cancelTidal(); cancelVinyl(); setProperty("quietvinylRemoteLoad", false); return true;
    }

  signals:
    void loadTidalTrack(const QString& path, const QString& group);
    void tidalLoadFailed(const QString& message);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    BrowserLoadConfirmation* m_confirmation;
    QString m_confirmationGroup;
    bool m_confirmationPlay = false;
    QPersistentModelIndex m_confirmationSelection, m_loadSelection;
    TrackPointer m_confirmationDestination, m_loadDestination;
    bool m_loadApproved = false;
    QElapsedTimer m_openedClock;
    bool justOpened() const { return m_openedClock.isValid() && m_openedClock.elapsed() < 350; }
    void requestSelectedLoad(const QString& group, bool play, bool replacementApproved = false);
    void confirmReplacement();
    void cancelLoadConfirmation();
    std::array<RecordMotion, 2> m_recordMotion;
    QElapsedTimer m_motionClock;
    void updateRecordMotion();
    QPersistentModelIndex m_controllerSelection;
    int m_controllerSelectionDeck = 0;
    BrowserRowArt* m_art;
    SimpleLibraryModel* m_model;
    QLabel* m_breadcrumb;
    DeckIndicator* m_deckIndicator;
    QLabel* m_empty;
    QLabel* m_vinylHint;
    BrowserTrackList* m_list;
    BrowserFader m_fader;
    QString m_breadcrumbText;
    int m_targetDeck = 1;
    void refreshDeckColors();
    void refreshBreadcrumb();
    void pollVinyl();
    void updateVinyl();
    void cancelVinyl();
    void finishVinylLoad(const QString& group);
    QTimer m_vinylTimer;
    QElapsedTimer m_vinylClock;
    QElapsedTimer m_loadClock;
    VinylBrowseLatch m_gesture;
    int m_capturedDeck = 0;
    bool m_loadingVinyl = false;
    QPointer<TidalPlaylistLibrary> m_tidalLibrary;
    bool m_loadingTidal = false;
    QString m_tidalStatus;
    void cancelTidal();
    double m_lastVinylFrame = -1;
};
