#pragma once

#include <QList>
#include <QObject>
#include <QUrl>
#include <QVariant>
#include <QTimer>
#include <vector>
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "track/track_decl.h"

#include "analyzer/trackanalysisscheduler.h"
#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "util/parented_ptr.h"

class DlgAnalysis;

class AnalysisFeature : public LibraryFeature {
    Q_OBJECT
  public:
    AnalysisFeature(Library* pLibrary,
                    UserSettingsPointer pConfig);
    ~AnalysisFeature() override = default;

    QVariant title() override {
        return m_title;
    }

    bool dropAccept(const QList<QUrl>& urls, QObject* pSource) override;
    bool dragMoveAccept(const QUrl& url) override;
    void bindLibraryWidget(WLibrary* libraryWidget,
                    KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;
    void refreshLibraryModels();

  signals:
    void analysisActive(bool bActive);
    void trackProgress(TrackId trackId, AnalyzerProgress progress);

  public slots:
    void activate() override;
    void analyzeTracks(const QList<AnalyzerScheduledTrack>& tracks);

    void suspendAnalysis();
    void resumeAnalysis();
    void stopAnalysis();

  private slots:
    void onTrackAnalysisSchedulerProgress(AnalyzerProgress currentTrackProgress, int currentTrackNumber, int totalTracksCount);
    void onTrackAnalysisSchedulerFinished();

  private:
    // Sets the title of this feature to the default name, given by
    // m_sAnalysisTitleName
    void resetTitle();

    // Sets the title of this feature to the default name followed by (x / y)
    // where x is the current track being analyzed and y is the total number of
    // tracks in the job
    void setTitleProgress(int currentTrackNumber, int totalTracksCount);

    void analyzePreparationTrack(bool replaceGrid);
    void changePreparationDownbeat(int beats, bool undo = false);
    void refreshPreparation();
    void configurePreparationSync();
    void startPreparationSync();
    void showPreparationSyncStatus();
    void showPreparationUsb();
    bool m_preparationSyncStarted = false;
    std::vector<std::unique_ptr<ControlPushButton>> m_preparationButtons;
    std::unique_ptr<ControlObject> m_preparationEnabled;
    std::unique_ptr<ControlObject> m_preparationDownbeat;
    std::unique_ptr<ControlObject> m_preparationConfirmed;
    std::unique_ptr<ControlObject> m_preparationAnalyzing;
    QTimer m_preparationTimer;
    TrackPointer m_preparationUndoTrack;
    std::vector<double> m_downbeatUndo;
    const QString m_baseTitle;

    TrackAnalysisScheduler::Pointer m_pTrackAnalysisScheduler;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    DlgAnalysis* m_pAnalysisView;

    // The title is dynamic and reflects the current progress
    QString m_title;
};
