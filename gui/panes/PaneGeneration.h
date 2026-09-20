#ifndef PANEGENERATION_H
#define PANEGENERATION_H

#include <QCoro/QCoroTask>

#include <string>
#include <unordered_map>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QPair>
#include <QUuid>
#include <QWidget>

#include "ProgressDialog.h"

#include "model/properties/PropertySampler.h"

#include "../DialogGenerationOptions.h"

QT_BEGIN_NAMESPACE
namespace Ui { class PaneGeneration; }
QT_END_NAMESPACE

class QDialog;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QStackedWidget;
class QTreeWidgetItem;
class QVideoWidget;

class AbstractCli;
class QDataWidgetMapper;
class QFileSystemModel;
class QModelIndex;
class QStandardItemModel;
class TreeProperties;
class TableGenerationSettings;
class TableProjects;
class TableVideos;
class PreferredHashtags;
class PromptLessons;
class FavoriteHooks;
class FavoriteVideoPrompts;
class SavedPrompts;
class VideoGenerationWorkflow;
class VideoPlanProxy;

// Left: the project list (add through DialogNewProject, remove deletes the
// project folder after confirmation). Right: the Source page shows the
// selected project's keyword/hook (edited in place through a
// QDataWidgetMapper) and its source image; the Generation page hosts the
// property tree, the per-project Generations tree (one row per past
// generation — expand for its A/B properties, select for its hook/
// description and an in-app preview), and the Generate flow:
// DialogGenerationOptions (image handling + image CLI), then the pane's CLI
// combo suggests prompts/hooks (DialogGenerationPlan / DialogHooks).
// m_planProxy is ready to be set on a view and bound to a video with
// setVideo().
//
// On disk, each generation gets its own folder
// (projects/<id>/generations/<shortCode>/): its top level holds only the
// video/images and hook-description.txt (what matters to publish); a
// "temp" subfolder holds the rest (source-image variant, prompts,
// suggestions/hooks JSON, rejected takes, extracted frames).
class PaneGeneration : public QWidget
{
    Q_OBJECT

public:
    explicit PaneGeneration(TreeProperties *properties, TreeProperties *archive,
                            TableVideos *videos,
                            TableGenerationSettings *generationSettings,
                            PreferredHashtags *hashtags,
                            PromptLessons *promptLessons,
                            FavoriteHooks *favoriteHooks,
                            FavoriteVideoPrompts *favoriteVideoPrompts,
                            SavedPrompts *savedPrompts,
                            QWidget *parent = nullptr);
    ~PaneGeneration();

    // Fills the prompt-suggestion CLI combo (top of the Generation page).
    // The image-step CLI is chosen separately in DialogGenerationOptions.
    void setAvailableClis(const QList<AbstractCli *> &clis);

private slots:
    void _addProject();
    void _removeProject();
    void _currentProjectChanged(const QModelIndex &current);
    void _generate();
    // Reopens the hook/description picker for the generation selected in
    // treeViewGenerations (or the project's latest one) — copy-paste at any
    // time later.
    void _openHooksDialog();
    // Manage the A/B property catalog (add property/value, archive, delete)
    // from the properties tree.
    void _propertiesContextMenu(const QPoint &position);
    // Optional cold-start step: analyzes user-picked reference images with
    // the prompt CLI and creates the A/B properties/values it finds, so the
    // very first real generation already has real diversity to sample from
    // instead of PropertySampler having only ever ONE value per property.
    void _bootstrapPropertiesFromImages();
    // Deletes the generated artifacts of the selected project (every past
    // generation's folder + staging) for a clean re-run. Keeps the original
    // source image and the VideoRecord statistics history.
    void _deleteGenerated();
    // Quick peek at the project's current source image (what the image/
    // video steps start from) — separate from the Generations tree preview,
    // which shows past OUTPUT, not the input.
    void _viewSourceImage();
    // A row of treeViewGenerations was selected: shows that generation's
    // properties (children), hook/description and content preview.
    void _onGenerationSelected(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void _togglePreviewPlayback();
    void _previewPrevImage();
    void _previewNextImage();
    // Saves m_currentPreviewHook into the favorite-hooks list — the
    // Generation page's "harvest from top-ranked videos" workflow: browse
    // past generations (cross-checking performance in the Statistics tab),
    // click this on the ones worth keeping as inspiration.
    void _favoriteCurrentHook();
    // Same, for m_currentPreviewVideoPrompt — harvests the prompt of a
    // video generation that actually rendered like a wanted viral-style
    // result, into the favorite-video-prompts list.
    void _favoriteCurrentVideoPrompt();
    void _generateAgain();
    void _togglePublishSelectedVideo();

private:
    void _applyGenerationPublishedStyle(class QTreeWidgetItem *topItem, bool published);
    Ui::PaneGeneration *ui;
    TreeProperties *m_properties;
    TreeProperties *m_archive;
    TableVideos *m_videos;
    VideoPlanProxy *m_planProxy;
    TableProjects *m_projects;
    QDataWidgetMapper *m_projectMapper;
    TableGenerationSettings *m_generationSettings;
    PreferredHashtags *m_hashtags;
    PromptLessons *m_promptLessons;
    FavoriteHooks *m_favoriteHooks;
    FavoriteVideoPrompts *m_favoriteVideoPrompts;
    SavedPrompts *m_savedPrompts;
    // The hook text currently shown in m_labelGenerationHook — kept so the
    // "Favorite this hook" button knows what to save without re-reading the
    // label's placeholder-vs-real-text state.
    QString m_currentPreviewHook;
    // The video prompt (generation_prompt.txt) of the currently selected
    // generation, when it is a video with one on disk — empty otherwise, in
    // which case the "Favorite this video prompt" button stays disabled.
    QString m_currentPreviewVideoPrompt;
    // Content of the selected project, shown in the Generation page: the
    // project folder's files (live, so generated videos/images appear as
    // soon as they land) and the suggested hooks/descriptions.
    QFileSystemModel *m_filesModel;
    QStandardItemModel *m_hooksModel;
    // In-app content preview (built in code into ui->widgetPreview): image
    // label or embedded video player, whichever the selected generation has.
    QStackedWidget *m_stackedPreview;
    QLabel *m_labelPreviewEmpty;
    QLabel *m_labelPreviewImage;
    QVideoWidget *m_videoPreview;
    QMediaPlayer *m_mediaPlayer;
    // Slideshow browsing: every image of the currently previewed generation
    // (empty/single-entry for a plain "one image" generation, in which case
    // the prev/next controls stay hidden), and which one is on screen.
    QStringList m_previewImageFiles;
    int m_previewImageIndex = 0;
    QPushButton *m_buttonPrevImage = nullptr;
    QPushButton *m_buttonNextImage = nullptr;
    QLabel *m_labelImageNav = nullptr;
    QLabel *m_labelGenerationHook;
    QLabel *m_labelGenerationDescription;
    // Lets the user harvest a hook straight from a past generation while
    // browsing it here, instead of retyping it in Settings — enabled only
    // when the selected generation actually has a chosen hook.
    QPushButton *m_buttonFavoriteHook = nullptr;
    // Same idea for video prompts — enabled only when the selected
    // generation is a video with its generation_prompt.txt still on disk.
    QPushButton *m_buttonFavoriteVideoPrompt = nullptr;
    QList<AbstractCli *> m_availableClis;
    // One progress-with-log dialog (common's ProgressDialog, same as
    // AmazonTemplate3) spans the whole Generate flow: image step, prompt
    // suggestion, then the video workflow. Recreated per run.
    QPointer<QDialog> m_progressDlg;
    ProgressDlgHandles m_progress;
    // The image the current Generate run works from — decided by the image
    // mode (original source / previously regenerated / freshly generated),
    // so downstream steps never guess. Lives in the run's staging folder
    // until the generation succeeds and everything is moved into its
    // final per-generation folder.
    QString m_runImagePath;
    // The project's optional SECOND source image (a different angle/
    // reference of the same product) — always the raw project file, NEVER
    // itself regenerated/bootstrapped by the image step (unlike
    // m_runImagePath); rides along unchanged as extra reference material for
    // every job. Empty when the project has no second image.
    QString m_runImagePath2;
    // A/B test state of the current run: one independently stat-weighted
    // sample PER SUGGESTED PROMPT (index-aligned with the 3 prompts offered
    // in DialogGenerationPlan) — so choosing a different prompt also means
    // comparing a genuinely different background/mood/etc. recipe, instead
    // of all 3 prompts sharing one identical draw. Which values end up
    // confirmed is a per-JOB decision now (GenerationJob::keptValueIds,
    // read from whichever prompt slot that job's option selected), not a
    // single plan-wide list — two ticked options can pick different slots.
    QList<QList<PropertySampler::SampledValue>> m_runSampledVariants;
    QString m_runShortCode;
    // Every generation successfully produced by the current Generate click
    // (one ticked option = one independent generation/VideoRecord), paired
    // with its OWN short code — several can come out of one click (a video
    // AND a slideshow), each needing its own recipe tag, never a shared one.
    QList<QPair<QDir, QString>> m_runGenerations;
    QString m_runVideoFormatLabel;

    // One ticked option from the plan dialog = one independent job. Jobs
    // are grouped by the external resource they'd contend for — a specific
    // video backend, or a specific CLI executable — via _jobGroupKey():
    // jobs in DIFFERENT groups run CONCURRENTLY (a video via Gemini's
    // browser and a slideshow via an AI CLI are independent processes with
    // nothing to fight over); jobs in the SAME group (two ticked options
    // both wanting the same CLI, say) run one after another so they never
    // race for one session. A job failure aborts only its OWN group's
    // remaining jobs — independent groups already running keep going.
    struct GenerationJob
    {
        enum Kind { Image, Slideshow, Video } kind;
        QString prompt;
        QString generatorId;   // Video only: AbstractVideoGenerator id
        AbstractCli *cli = nullptr;  // Image/Slideshow only
        // The A/B values confirmed for the SPECIFIC prompt slot this job's
        // option selected — read off DialogGenerationPlan::Plan at dispatch
        // time; two ticked options can point at different slots, so this
        // must travel with the job, not live on a plan-wide member.
        QList<QUuid> keptValueIds;
        bool repeatUnchanged = false;
        QVariantMap settings;
        QStringList sourceImageFiles; // repeat jobs: relative to their own staging directory
    };
    QHash<QString, QList<GenerationJob>> m_jobGroups;
    int m_jobCounter = 0;
    bool m_batchCancelled = false;
    void _queueJobs(int row, const QUuid &projectId, const QList<GenerationJob> &jobs);
    // One in-flight coroutine per group currently running an image/slideshow
    // job (kept alive by this map — a QCoro::Task must not be destroyed
    // before the coroutine it represents completes). std::unordered_map,
    // not QHash: QCoro::Task is move-only and QHash's rehash path requires
    // copyable values.
    std::unordered_map<std::string, QCoro::Task<void>> m_imageJobTasks;
    // Snapshot of the staging folder's contents taken ONCE, right before any
    // job starts — every job gets an ISOLATED copy of this (see
    // _prepareJobStaging), so concurrent jobs never see each other's
    // in-progress files.
    QList<QFileInfo> m_baseStagingEntries;

    AbstractCli *_promptCli() const;
    // The image-capable CLI last used in DialogGenerationOptions (or the
    // first available one) — reused when the workflow's decision CLI asks
    // for a new source image mid-generation.
    AbstractCli *_imageCli() const;
    // Runs the full generate-until-validated workflow for one video backend,
    // following it with a cancellable progress dialog. jobStaging is this
    // job's own isolated working folder (see _prepareJobStaging).
    void _startVideoGeneration(int row, const QUuid &projectId, const QString &groupKey,
                               const GenerationJob &job, const QDir &jobStaging);
    // Step 1 of the Generate flow when the image mode needs a CLI
    // (regenerate/bootstrap): loops on ordinary failures (an image CLI can
    // be flaky) — only stops ("paused") for login/quota errors
    // (classifyError), or as a safety net if the same failure repeats many
    // times in a row.
    void _runImageStep(const QUuid &projectId, DialogGenerationOptions::ImageMode mode,
                       AbstractCli *imageCli, const QString &imageRef,
                       const QString &videoFormatLabel, bool whiteBackgroundProduct,
                       const QString &outputFileName = QStringLiteral("generation_source.png"),
                       const QString &nextImageRef = QString{},
                       int attempt = 1);
    void _openProgress();
    void _logProgress(const QString &message);
    // Final log line + stops the spinner, enables Close, hides Cancel.
    void _finishProgress(const QString &message);
    // Step 2 of the Generate flow: the pane's CLI preselects/filters the A/B
    // properties and suggests 10 hook/description pairs (strict JSON) — it
    // no longer writes any content prompt text (see DialogGenerationPlan,
    // where the user writes the prompt directly); the picks are stored on
    // the project once DialogGenerationPlan is accepted.
    // videoFormatLabel comes from DialogGenerationOptions::formatLabel().
    // Perseveres on its own: an invalid/truncated JSON reply or an ordinary
    // failure is retried (up to 3 attempts, with a corrective instruction);
    // only login/quota errors (classifyError) stop immediately, with the
    // real reason displayed.
    void _suggestPlan(const QUuid &projectId, const QString &videoFormatLabel,
                      int attempt = 1);
    // Runs (and, on an invalid/truncated reply, retries up to 3 times) the
    // actual analysis call for _bootstrapPropertiesFromImages(); bootstrapDir
    // is a throwaway folder (holds only copies of the picked images plus the
    // CLI's reply file) removed once this finishes either way.
    void _runPropertyBootstrap(const QStringList &fileNames, const QDir &bootstrapDir,
                               int attempt = 1);
    // Saves a hook/description pick (project columns + hook-description.txt,
    // one per generation with ITS OWN recipe tag appended) — reached
    // manually via the "Hooks..." button/double-click (_openHooksDialog()),
    // never auto-popped up right after generation: picking a hook needs
    // seeing the actual generated result first.
    void _saveChosenHookDescription(int row, const QList<QPair<QDir, QString>> &generations,
                                    const QString &hook, const QString &description);
    // Reloads the top hooks table (the just-suggested 10 hooks) from disk.
    void _refreshHooksView(const QDir &hooksTempDir);
    // Rebuilds treeViewGenerations for the given project row: one top-level
    // row per VideoRecord (newest first), its A/B property values as
    // children.
    void _refreshGenerationsView(int row);
    // Finds the most-recently-generated image for this project, wherever it
    // ended up (new per-generation temp folder, or the legacy project-root
    // location from before this layout existed) — used by "reuse previous".
    QString _latestGeneratedImage(int row) const;
    QString _latestGeneratedImage2(int row) const;
    // On a successful generation: creates the VideoRecord, moves this job's
    // isolated staging folder into its final generations/<shortCode>/ home
    // (every path in outputPaths to the top level, everything else to
    // temp/). Returns the new generation folder; *outShortCode (if given)
    // receives its short code — read that, not m_runShortCode, when several
    // jobs may be finishing around the same time.
    QDir _finalizeGeneration(int row, const QUuid &projectId,
                             const QStringList &outputPaths, const QDir &jobStaging,
                             const QList<QUuid> &keptValueIds,
                             QString *outShortCode = nullptr);
    static void _copyDirRecursively(const QString &sourcePath, const QString &targetPath);
    // Shows the given generation's video/image(s) at the top level of its
    // folder in the embedded preview, or the empty page if none exists yet.
    // A slideshow's several images are all loaded into m_previewImageFiles;
    // only the one at m_previewImageIndex is displayed at a time, browsed
    // with the prev/next controls (hidden for a single-image generation).
    void _showPreview(const QDir &generationDir);
    // Displays m_previewImageFiles[index] (clamped) and updates the "n / N"
    // label.
    void _showPreviewImage(int index);
    // "Image" / "Slideshow (N)" / "Video" / "—" (nothing produced yet),
    // inferred from the generation folder's own file listing — the kind
    // isn't stored on VideoRecord, and the files at the top level are
    // exactly what publishing uses, so they are the source of truth.
    static QString _generationTypeLabel(const QDir &generationDir);
    // Pops and runs the next job of one group, or — once every group has
    // emptied out (finished or aborted after its own failure) — wraps up
    // (staging cleanup, hooks step).
    void _runNextInGroup(int row, const QUuid &projectId, QString groupKey);
    // Copies m_baseStagingEntries into a fresh, isolated subfolder of
    // staging for one job — so concurrent jobs never share working files.
    QDir _prepareJobStaging(int row, const QString &jobTag);
    // Ensures the run's reference image (if any) exists inside jobStaging,
    // copying it in if it isn't already there; "" when there is none.
    QString _resolveJobImage(const QDir &jobStaging) const;
    // Same, for the project's optional second source image (m_runImagePath2)
    // — always the raw project file, never regenerated/bootstrapped.
    QString _resolveSecondaryImage(const QDir &jobStaging) const;
    // The external resource a job would contend for — same key = must run
    // sequentially, different key = safe to run concurrently.
    static QString _jobGroupKey(const GenerationJob &job);
    // Human-readable tag for progress-log lines ("Video — Gemini (browser)",
    // "Slideshow — Claude") so concurrent jobs' interleaved output stays
    // readable.
    static QString _jobLabel(const GenerationJob &job);
    QCoro::Task<void> _runImageGenerationJob(int row, QUuid projectId, QString groupKey,
                                             GenerationJob job, QDir jobStaging);
};

#endif // PANEGENERATION_H
