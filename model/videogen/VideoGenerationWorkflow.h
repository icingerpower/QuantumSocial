#ifndef VIDEOGENERATIONWORKFLOW_H
#define VIDEOGENERATIONWORKFLOW_H

#include <functional>

#include <QObject>
#include <QStringList>
#include <QVariantMap>

#include <QCoro/QCoroTask>

class AbstractCli;

// Runs one video generation to completion, however long it takes:
//
//   round:  try the prompt up to maxTriesPerPrompt times, each try with a
//           small variation ("add/remove a bit") — rejections by the backend
//           are expected and retried;
//   stuck:  after a full round of failures, the decision CLI chooses to
//           either rewrite the prompt or regenerate the source image, and a
//           new round starts — repeated as much as needed;
//   check:  once a video is downloaded, key frames are extracted (ffmpeg)
//           and the decision CLI judges whether the clip is on topic and
//           technically sound; a refused clip is set aside and generation
//           starts over — with the CLI's improved prompt for a prompt-side
//           issue, or a REGENERATED source image for an image-side one
//           (watermark/logo/text baked into the image: no prompt rewrite
//           can fix what the video backend is only animating, see
//           causedByImage/imageFix in the check's JSON contract) — backends
//           like the Gemini web app cannot continue the previous session,
//           each attempt is a fresh one, only the prompt/image carry over.
//           After kMaxContentCheckFailures refusals (2 — same threshold as
//           ImageGeneratorCli's content-check budget), the last rejected
//           clip is accepted anyway instead of looping indefinitely: two
//           failures against the SAME brief is treated as evidence the
//           prompt/image are about as good as they will get, not a signal
//           to keep spending browser-automation rounds chasing perfection.
//
// The only ways out: a validated video, cancel(), or a setup error.
// Progress is reported through signals so a progress dialog can follow.
class VideoGenerationWorkflow : public QObject
{
    Q_OBJECT

public:
    struct Request
    {
        QString generatorId;        // AbstractVideoGenerator id
        QString prompt;
        QString imagePath;          // source image; empty for text-only
        // Extra STATIC reference image(s) beyond imagePath (see
        // PaneGeneration's optional second project image) — included in
        // every generate() attempt for visual context, but NEVER touched by
        // the self-correction loop, which only ever regenerates imagePath.
        QStringList extraImagePaths;
        QString outputDir;          // the project's folder
        QString contextSummary;     // keyword/hook, given to the decision CLI
        // e.g. "vertical (9:16)" (DialogGenerationOptions::formatLabel),
        // used when regenerating the source image mid-run.
        QString videoFormatLabel;
        QVariantMap settings;       // TableGenerationSettings::settingsFor()
        AbstractCli *decisionCli = nullptr;  // rewrites prompts, judges frames
        AbstractCli *imageCli = nullptr;     // regenerates the source image
        // Concrete pitfalls recorded from past generations (see
        // PromptLessons) — prepended to the prompt so known mistakes aren't
        // repeated. Empty when there are none yet.
        QString knownPitfalls;
        // Called with a short, reusable takeaway whenever the content check
        // refuses a clip — the caller persists these and feeds them back
        // into future prompts (see knownPitfalls above).
        std::function<void(const QString &)> recordLesson;
    };

    explicit VideoGenerationWorkflow(QObject *parent = nullptr);

    bool isRunning() const;
    void start(const Request &request);
    // Takes effect between steps: the step in flight (one generation, one
    // CLI call) finishes first.
    void cancel();

signals:
    void progress(const QString &message);
    // videoPath is empty when cancelled or on a setup error (errorMessage).
    void finished(const QString &videoPath, const QString &errorMessage);

private:
    bool m_running = false;
    bool m_cancelled = false;
    QCoro::Task<void> m_task;

    QCoro::Task<void> _run(Request request);
    // Extracts up to 5 key frames into framesDir; returns the frame file
    // names, or empty with errorMessage set.
    QCoro::Task<QStringList> _extractKeyFrames(QString videoPath, QString framesDir,
                                               QString *errorMessage);
    // Asks imageCli to produce a new generation_source.png from currentImage
    // per instruction (free text — "make something different" when a whole
    // round of prompts got rejected, or "remove the watermark in the
    // bottom-right corner" when the check pinpointed an image-side defect).
    // Returns the new image's path, or empty if the CLI produced no file.
    QCoro::Task<QString> _regenerateImage(QString currentImage, QString outputDir,
                                          QString videoFormatLabel, QString instruction,
                                          AbstractCli *imageCli);
    void _finish(const QString &videoPath, const QString &errorMessage);
};

#endif // VIDEOGENERATIONWORKFLOW_H
