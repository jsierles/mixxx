#include "library/analysis/analysisfeature.h"

#include <QList>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QSaveFile>
#include <QUuid>
#include <QVBoxLayout>
#include "library/trackcollection.h"
#include <QtDebug>
#include "control/controlproxy.h"
#include "mixer/playerinfo.h"
#include "track/track.h"

#include "analyzer/analyzerscheduledtrack.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "library/analysis/dlganalysis.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_analysisfeature.cpp"
#include "sources/soundsourceproxy.h"
#include "util/logger.h"
#include "widget/wlibrary.h"
#include "preparationinbox.h"

namespace {

const mixxx::Logger kLogger("AnalysisFeature");

const QString kViewName = QStringLiteral("Analysis");

// Utilize all available cores for batch analysis of tracks
const int kNumberOfAnalyzerThreads = math_max(1, QThread::idealThreadCount());

inline
int numberOfAnalyzerThreads() {
    return kNumberOfAnalyzerThreads;
}

inline
AnalyzerModeFlags getAnalyzerModeFlags(
        const UserSettingsPointer& pConfig) {
    // Always enable at least BPM detection for batch analysis, even if disabled
    // in the config for ad-hoc analysis of tracks.
    // NOTE(uklotzde, 2018-12-26): The previous comment just states the status-quo
    // of the existing code. We should rethink the configuration of analyzers when
    // refactoring/redesigning the analyzer framework.
    int modeFlags = AnalyzerModeFlags::WithBeats | AnalyzerModeFlags::LowPriority;
    if (pConfig->getValue<bool>(ConfigKey("[Library]", "EnableWaveformGenerationWithAnalysis"), true)) {
        modeFlags |= AnalyzerModeFlags::WithWaveform;
    }
    return static_cast<AnalyzerModeFlags>(modeFlags);
}

} // anonymous namespace

AnalysisFeature::AnalysisFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("prepare")),
          m_baseTitle(tr("Analyze")),
          m_pTrackAnalysisScheduler(TrackAnalysisScheduler::NullPointer()),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pAnalysisView(nullptr),
          m_title(m_baseTitle) {
    m_preparationEnabled = std::make_unique<ControlObject>(ConfigKey("[Preparation]", "enabled"));
    m_preparationDownbeat = std::make_unique<ControlObject>(ConfigKey("[Channel1]", "prep_downbeat_position"));
    m_preparationDownbeat->set(-1);
    m_preparationConfirmed = std::make_unique<ControlObject>(ConfigKey("[Channel1]", "prep_downbeat_confirmed"));
    m_preparationAnalyzing = std::make_unique<ControlObject>(ConfigKey("[Channel1]", "prep_analyzing"));
    const auto addButton = [this](const QString& name, auto action) {
        auto button = std::make_unique<ControlPushButton>(ConfigKey("[Channel1]", name), false);
        connect(button.get(), &ControlObject::valueChanged, this, [this, action](double value) {
            if (value > 0 && m_preparationEnabled->get() > 0) {
                action();
            }
        });
        m_preparationButtons.push_back(std::move(button));
    };
    addButton("prep_analyze", [this] { analyzePreparationTrack(false); });
    addButton("prep_reanalyze", [this] { analyzePreparationTrack(true); });
    addButton("prep_downbeat_set", [this] { changePreparationDownbeat(0); });
    addButton("prep_downbeat_earlier", [this] { changePreparationDownbeat(-1); });
    addButton("prep_downbeat_later", [this] { changePreparationDownbeat(1); });
    addButton("prep_downbeat_undo", [this] { changePreparationDownbeat(0, true); });
    addButton("prep_sync_setup", [this] { configurePreparationSync(); });
    addButton("prep_sync_status", [this] { showPreparationSyncStatus(); });
    addButton("prep_usb", [this] { showPreparationUsb(); });
    connect(&m_preparationTimer, &QTimer::timeout, this, &AnalysisFeature::refreshPreparation);
    m_preparationTimer.start(1000);
}

void AnalysisFeature::resetTitle() {
    m_title = m_baseTitle;
    emit featureIsLoading(this, false);
}

void AnalysisFeature::setTitleProgress(int currentTrackNumber, int totalTracksCount) {
    m_title = QString("%1 (%2 / %3)")
                      .arg(m_baseTitle,
                              QString::number(currentTrackNumber),
                              QString::number(totalTracksCount));
    emit featureIsLoading(this, false);
}

void AnalysisFeature::bindLibraryWidget(WLibrary* libraryWidget,
                                 KeyboardEventFilter* keyboard) {
    m_pAnalysisView = new DlgAnalysis(libraryWidget,
                                      m_pConfig,
                                      m_pLibrary);
    connect(m_pAnalysisView,
            &DlgAnalysis::loadTrack,
            this,
            &AnalysisFeature::loadTrack);
    connect(m_pAnalysisView,
            &DlgAnalysis::loadTrackToPlayer,
            this,
            [=, this](TrackPointer track, const QString& group) {
                emit loadTrackToPlayer(track, group, false);
            });
    connect(m_pAnalysisView,
            &DlgAnalysis::analyzeTracks,
            this,
            &AnalysisFeature::analyzeTracks);
    connect(m_pAnalysisView,
            &DlgAnalysis::stopAnalysis,
            this,
            &AnalysisFeature::stopAnalysis);

    connect(m_pAnalysisView,
            &DlgAnalysis::trackSelected,
            this,
            &AnalysisFeature::trackSelected);

    connect(this,
            &AnalysisFeature::analysisActive,
            m_pAnalysisView,
            &DlgAnalysis::slotAnalysisActive);

    m_pAnalysisView->installEventFilter(keyboard);

    // Let the DlgAnalysis know whether or not analysis is active.
    emit analysisActive(static_cast<bool>(m_pTrackAnalysisScheduler));

    libraryWidget->registerView(kViewName, m_pAnalysisView);
}

TreeItemModel* AnalysisFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void AnalysisFeature::refreshLibraryModels() {
    if (m_pAnalysisView) {
        m_pAnalysisView->onShow();
    }
}

void AnalysisFeature::activate() {
    //qDebug() << "AnalysisFeature::activate()";
    emit switchToView(kViewName);
    if (m_pAnalysisView) {
        emit restoreSearch(m_pAnalysisView->currentSearch());
    }
    emit enableCoverArtDisplay(true);
}

void AnalysisFeature::analyzeTracks(const QList<AnalyzerScheduledTrack>& tracks) {
    if (!m_pTrackAnalysisScheduler) {
        const int numAnalyzerThreads = numberOfAnalyzerThreads();
        kLogger.info()
                << "Starting analysis using"
                << numAnalyzerThreads
                << "analyzer threads";
        m_pTrackAnalysisScheduler = m_pLibrary->createTrackAnalysisScheduler(
                numAnalyzerThreads,
                getAnalyzerModeFlags(m_pConfig));

        connect(m_pTrackAnalysisScheduler.get(),
                &TrackAnalysisScheduler::progress,
                m_pAnalysisView,
                &DlgAnalysis::onTrackAnalysisSchedulerProgress);
        connect(m_pTrackAnalysisScheduler.get(),
                &TrackAnalysisScheduler::finished,
                m_pAnalysisView,
                &DlgAnalysis::onTrackAnalysisSchedulerFinished);
        connect(m_pTrackAnalysisScheduler.get(),
                &TrackAnalysisScheduler::progress,
                this,
                &AnalysisFeature::onTrackAnalysisSchedulerProgress);
        connect(m_pTrackAnalysisScheduler.get(),
                &TrackAnalysisScheduler::finished,
                this,
                &AnalysisFeature::onTrackAnalysisSchedulerFinished);
        // Forward the signal to be picked up by Library.
        // Used by WOverview to render the re-analysis progress of loaded tracks.
        // This is the equivalent to PlayerManager's progress signal fired for
        // analysis triggered by loading a track.
        connect(m_pTrackAnalysisScheduler.get(),
                &TrackAnalysisScheduler::trackProgress,
                this,
                &AnalysisFeature::trackProgress);

        emit analysisActive(true);
    }

    if (m_pTrackAnalysisScheduler->scheduleTracks(tracks) > 0) {
        resumeAnalysis();
    }
}

void AnalysisFeature::suspendAnalysis() {
    if (!m_pTrackAnalysisScheduler) {
        return; // inactive
    }
    kLogger.info() << "Suspending analysis";
    m_pTrackAnalysisScheduler->suspend();
}

void AnalysisFeature::resumeAnalysis() {
    if (!m_pTrackAnalysisScheduler) {
        return; // inactive
    }
    kLogger.info() << "Resuming analysis";
    m_pTrackAnalysisScheduler->resume();
}

void AnalysisFeature::stopAnalysis() {
    if (!m_pTrackAnalysisScheduler) {
        return; // inactive
    }
    kLogger.info() << "Stopping analysis";
    m_pTrackAnalysisScheduler->stop();
}

void AnalysisFeature::onTrackAnalysisSchedulerProgress(
        AnalyzerProgress /*currentTrackProgress*/,
        int currentTrackNumber,
        int totalTracksCount) {
    // Ignore any delayed progress updates after the analysis
    // has already been stopped.
    if (!m_pTrackAnalysisScheduler) {
        return; // inactive
    }
    if (totalTracksCount > 0) {
        setTitleProgress(currentTrackNumber, totalTracksCount);
    } else {
        resetTitle();
    }
}

void AnalysisFeature::onTrackAnalysisSchedulerFinished() {
    if (!m_pTrackAnalysisScheduler) {
        return; // already inactive
    }
    kLogger.info() << "Finishing analysis";
    if (m_pTrackAnalysisScheduler) {
        // Free resources by abandoning the queue after the batch analysis
        // has completed. Batch analysis are not started very frequently
        // during a session and should be avoided while performing live.
        // If the user decides to start a new batch analysis the setup costs
        // for creating the queue with its worker threads are acceptable.
        m_pTrackAnalysisScheduler.reset();
    }
    resetTitle();
    emit analysisActive(false);
}

bool AnalysisFeature::dropAccept(const QList<QUrl>& urls, QObject* pSource) {
    const QList<TrackId> trackIds =
            m_pLibrary->trackCollectionManager()->resolveTrackIdsFromUrls(
                    urls,
                    !pSource);
    QList<AnalyzerScheduledTrack> tracks;
    for (auto trackId : trackIds) {
        tracks.append(trackId);
    }
    analyzeTracks(tracks);
    return tracks.size() > 0;
}

bool AnalysisFeature::dragMoveAccept(const QUrl& url) {
    return SoundSourceProxy::isUrlSupported(url);
}

// Preparation commands run on the library GUI thread. Audio rendering stays on
// the engine thread; saving metadata here is deferred from writing audio tags.
void AnalysisFeature::analyzePreparationTrack(bool replaceGrid) {
    const auto track = PlayerInfo::instance().getTrackInfo("[Channel1]");
    if (!track || !m_pAnalysisView || track->isBpmLocked() ||
            ControlProxy("[Channel1]", "play").get() > 0 || m_pTrackAnalysisScheduler) {
        return;
    }
    if (replaceGrid) {
        track->trySetBeats(mixxx::BeatsPointer());
        track->removeCuesOfType(mixxx::CueType::Downbeat);
        m_downbeatUndo.clear();
    }
    m_preparationAnalyzing->set(1);
    analyzeTracks({AnalyzerScheduledTrack(track->getId())});
}

void AnalysisFeature::changePreparationDownbeat(int beats, bool undo) {
    const auto track = PlayerInfo::instance().getTrackInfo("[Channel1]");
    if (!track || track->isBpmLocked() || !track->getBeats() ||
            ControlProxy("[Channel1]", "play").get() > 0) {
        return;
    }
    if (m_preparationUndoTrack != track) {
        m_preparationUndoTrack = track;
        m_downbeatUndo.clear();
    }
    auto cue = track->findCueByType(mixxx::CueType::Downbeat);
    mixxx::audio::FramePos position;
    if (undo) {
        if (m_downbeatUndo.empty()) {
            return;
        }
        const auto saved = m_downbeatUndo.back();
        m_downbeatUndo.pop_back();
        if (saved < 0) {
            track->removeCuesOfType(mixxx::CueType::Downbeat);
            refreshPreparation();
            return;
        }
        position = mixxx::audio::FramePos::fromEngineSamplePos(saved);
    } else {
        if (beats != 0 && !cue) {
            return; // Select a downbeat explicitly before shifting bar phase.
        }
        const auto current = ControlProxy("[Channel1]", "playposition").get() *
                track->getDuration() * track->getSampleRate();
        position = beats == 0 ? track->getBeats()->findClosestBeat(mixxx::audio::FramePos(current)) :
                track->getBeats()->findNBeatsFromPosition(cue->getPosition(), beats);
        if (!position.isValid() || position.value() < 0) {
            return;
        }
        m_downbeatUndo.push_back(cue ? cue->getPosition().toEngineSamplePosMaybeInvalid() : -1);
    }
    if (cue) {
        cue->setStartPosition(position);
    } else {
        cue = track->createAndAddCue(mixxx::CueType::Downbeat, -1, position,
                mixxx::audio::FramePos(), mixxx::RgbColor(0xF0E4B7));
        cue->setLabel("Downbeat (4/4)");
    }
    refreshPreparation();
}

void AnalysisFeature::refreshPreparation() {
    if (m_preparationEnabled->get() <= 0) {
        return;
    }
    startPreparationSync();
    receivePreparationInbox(m_pLibrary, m_pConfig);
    const auto track = PlayerInfo::instance().getTrackInfo("[Channel1]");
    auto cue = track ? track->findCueByType(mixxx::CueType::Downbeat) : CuePointer();
    if (cue && track->getBeats()) {
        // Retain bar phase across small timing/grid adjustments by snapping the
        // anchor to the nearest beat, without changing the grid itself.
        const auto snapped = track->getBeats()->findClosestBeat(cue->getPosition());
        if (snapped.isValid() && snapped != cue->getPosition()) {
            cue->setStartPosition(snapped);
        }
    }
    m_preparationDownbeat->set(cue ? cue->getPosition().toEngineSamplePosMaybeInvalid() : -1);
    m_preparationConfirmed->set(cue ? 1 : 0);
    m_preparationAnalyzing->set(m_pTrackAnalysisScheduler ? 1 : 0);
    if (track && track->isDirty()) {
        m_pLibrary->trackCollectionManager()->saveTrack(track);
    }
}

void AnalysisFeature::configurePreparationSync() {
    if (!m_pAnalysisView) return;
    const auto filename = QDir(m_pConfig->getSettingsPath()).filePath("quietvinyl-sync.json");
    QJsonObject config;
    QFile existing(filename);
    if (existing.open(QIODevice::ReadOnly)) config = QJsonDocument::fromJson(existing.readAll()).object();
    const auto selected = config.value("playlists").toArray();
    QDialog dialog(m_pAnalysisView);
    dialog.setWindowTitle(tr("Playlists to synchronize"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget(&dialog);
    for (const auto& playlist : m_pLibrary->trackCollectionManager()->internalCollection()->getPlaylistDAO().getPlaylists(PlaylistDAO::PLHT_NOT_HIDDEN)) {
        auto* item = new QListWidgetItem(playlist.second, list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(selected.contains(playlist.second) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    QJsonArray playlists;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) playlists.append(list->item(i)->text());
    }
    if (playlists.isEmpty()) return;
    const auto roots = m_pLibrary->trackCollectionManager()->internalCollection()->getDirectoryDAO().getRootDirStrings();
    QString musicRoot = config.value("music_root").toString();
    if (musicRoot.isEmpty() && roots.size() == 1) musicRoot = roots.first();
    if (musicRoot.isEmpty()) musicRoot = QFileDialog::getExistingDirectory(m_pAnalysisView, tr("Choose the music folder for these playlists"));
    if (musicRoot.isEmpty()) return;
    const auto resources = QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/quietvinyl-sync");
    config.insert("database", QDir(m_pConfig->getSettingsPath()).filePath("mixxxdb.sqlite"));
    config.insert("music_root", musicRoot);
    config.insert("playlists", playlists);
    if (!config.contains("source_id")) config.insert("source_id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!config.contains("output")) config.insert("output", QDir(m_pConfig->getSettingsPath()).filePath("SyncStaging"));
    for (const auto& name : {QString("engine_exporter"), QString("cdj_exporter")}) {
        const auto path = QDir(resources).filePath(name == "engine_exporter" ? "quietvinyl-engine-writer" : "baken");
        QFile executable(path);
        if (executable.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&executable);
            config.insert(name, path);
            config.insert(name + "_sha256", QString::fromLatin1(hash.result().toHex()));
        }
    }
    QSaveFile output(filename);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(config).toJson()) < 0 || !output.commit()) {
        QMessageBox::warning(m_pAnalysisView, tr("Synchronization"), tr("Could not save synchronization settings."));
        return;
    }
    startPreparationSync();
}

void AnalysisFeature::startPreparationSync() {
    if (m_preparationSyncStarted) return;
    const auto config = QDir(m_pConfig->getSettingsPath()).filePath("quietvinyl-sync.json");
    const auto resources = QDir(QCoreApplication::applicationDirPath()).filePath("../Resources/quietvinyl-sync");
    const auto python = QDir(resources).filePath("runtime/bin/python3");
    const auto script = QDir(resources).filePath("scripts/sync-mixxx-library.py");
    if (!QFile::exists(config) || !QFile::exists(python) || !QFile::exists(script)) return;
    QProcess process;
    process.setProgram(python);
    process.setArguments({script, "--config", config, "--watch"});
    process.setWorkingDirectory(resources);
    process.setStandardOutputFile(QDir(m_pConfig->getSettingsPath()).filePath("sync-observer.log"), QIODevice::Append);
    process.setStandardErrorFile(QDir(m_pConfig->getSettingsPath()).filePath("sync-observer-error.log"), QIODevice::Append);
    m_preparationSyncStarted = process.startDetached();
}

void AnalysisFeature::showPreparationSyncStatus() {
    QFile settings(QDir(m_pConfig->getSettingsPath()).filePath("quietvinyl-sync.json"));
    if (!settings.open(QIODevice::ReadOnly)) {
        QMessageBox::information(m_pAnalysisView, tr("Synchronization"), tr("Choose Sync playlists first."));
        return;
    }
    const auto config = QJsonDocument::fromJson(settings.readAll()).object();
    QFile status(QDir(config.value("output").toString()).filePath("status.json"));
    if (!status.open(QIODevice::ReadOnly)) {
        QMessageBox::information(m_pAnalysisView, tr("Synchronization"), tr("Waiting for the first saved preparation."));
        return;
    }
    const auto state = QJsonDocument::fromJson(status.readAll()).object();
    QString message = tr("State: %1\nTracks: %2\nPlaylists: %3")
            .arg(state.value("state").toString()).arg(state.value("tracks").toInt()).arg(state.value("playlists").toInt());
    if (state.contains("error")) message += "\n\n" + state.value("error").toString();
    for (const auto& warning : state.value("warnings").toArray()) message += "\n" + warning.toString();
    QFile transfer(QDir(config.value("output").toString()).filePath("pi-transfer-status.json"));
    if (transfer.open(QIODevice::ReadOnly)) {
        const auto progress = QJsonDocument::fromJson(transfer.readAll()).object();
        message += "\n\nPi transfer: " + progress.value("state").toString();
        if (progress.contains("error")) message += "\n" + progress.value("error").toString();
    }
    QMessageBox::information(m_pAnalysisView, tr("Synchronization"), message);
}

void AnalysisFeature::showPreparationUsb() {
    QFile settings(QDir(m_pConfig->getSettingsPath()).filePath("quietvinyl-sync.json"));
    if (settings.open(QIODevice::ReadOnly)) {
        const auto config = QJsonDocument::fromJson(settings.readAll()).object();
        const QUrl url(config.value("pi_helper_url").toString());
        if (url.scheme() == "http" && url.host() == config.value("pi_host").toString() && !url.fragment().isEmpty()) {
            QDesktopServices::openUrl(url);
            return;
        }
    }
    QMessageBox::information(m_pAnalysisView, tr("USB on Pi"),
            tr("The Pi USB helper has not been connected on this Mac yet. Its setup must complete before a drive can be synchronized."));
}
