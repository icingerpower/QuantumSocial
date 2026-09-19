// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "VideoGenerationWorkflow.h"

#include <chrono>

#include <QCoro/QCoroTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include "AbstractCli.h"

#include "AbstractVideoGenerator.h"

namespace {

// Small "add/remove a bit" variations tried within one round: odd attempts
// append a softening clause, even attempts also drop the prompt's last
// sentence. Deterministic on purpose — attempt N always produces the same
// variant, so logs stay reproducible.
const QStringList SOFTENERS{
    QStringLiteral(" Tasteful and brand-safe."),
    QStringLiteral(" Family-friendly styling."),
    QStringLiteral(" Soft, elegant studio lighting."),
    QStringLiteral(" Classy fashion-editorial look."),
    QStringLiteral(" Keep it subtle and refined."),
};

QString perturbPrompt(const QString &base, int attempt)
{
    if (attempt == 0)
    {
        return base;
    }
    const QString trimmed = base.trimmed();
    if (attempt % 2 == 1)
    {
        return trimmed + SOFTENERS[(attempt / 2) % SOFTENERS.size()];
    }
    // Remove a bit: drop the last sentence when the prompt is long enough
    // for that to leave something meaningful.
    const qsizetype cut = trimmed.lastIndexOf(QLatin1Char('.'), trimmed.size() - 2);
    const QString shortened = cut > 40 ? trimmed.left(cut + 1) : trimmed;
    return shortened + SOFTENERS[(attempt / 2 + 2) % SOFTENERS.size()];
}

// CLIs are asked for strict JSON but may wrap it anyway: parse between the
// first '{' and the last '}'.
QJsonObject parseJsonReply(const QString &raw)
{
    const qsizetype first = raw.indexOf(QLatin1Char('{'));
    const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
    if (first < 0 || last <= first)
    {
        return QJsonObject{};
    }
    return QJsonDocument::fromJson(raw.mid(first, last - first + 1).toUtf8()).object();
}

QString fileReference(const QString &path, const QString &workingDir)
{
    return path.startsWith(workingDir) ? QFileInfo{path}.fileName() : path;
}

// Gemini's aspect-ratio UI selector only reacts to the literal ratio digits
// (e.g. "9:16") appearing in the prompt (see generate_video_gemini.py's
// _select_aspect()). The INITIAL prompt already has this ensured by the
// caller (PaneGeneration's ensureVideoFormatStated) — but the decision CLI
// can wholesale-REPLACE the prompt after a failed round ("action": "prompt"
// below) with no guarantee the replacement still mentions it, so this must
// be re-checked every time the prompt changes, not just once at the start.
QString ensureFormatStated(const QString &prompt, const QString &videoFormatLabel)
{
    if (videoFormatLabel.isEmpty())
    {
        return prompt;
    }
    const int openParen = videoFormatLabel.indexOf(QLatin1Char('('));
    const int closeParen = videoFormatLabel.indexOf(QLatin1Char(')'), openParen + 1);
    const QString ratio = (openParen >= 0 && closeParen > openParen)
        ? videoFormatLabel.mid(openParen + 1, closeParen - openParen - 1)
        : videoFormatLabel;
    if (!ratio.isEmpty() && prompt.contains(ratio, Qt::CaseInsensitive))
    {
        return prompt;
    }
    return prompt.trimmed() + QStringLiteral(" Video format: %1.").arg(videoFormatLabel);
}

// result.errorMessage/_emit() (see generate_video_gemini.py) appends
// " [debug log: /tmp/.../gemini_<timestamp>.log]" to every error — a NEW
// path every single process launch. Comparing raw errorMessage strings to
// detect "the same technical error repeating" therefore NEVER matched, even
// when the substantive error was byte-for-byte identical every time: this
// silently disabled the circuit breaker below and was root-caused live
// after a UI-selector error ("Could not find Gemini's video (Veo) tool
// button") repeated 60+ rounds over 4+ hours, needlessly asking the CLI to
// rewrite the prompt each time even though no prompt could ever fix it.
// Stripping the noisy per-run suffix before comparing is the fix.
QString withoutDebugLogSuffix(const QString &message)
{
    const qsizetype idx = message.indexOf(QStringLiteral(" [debug log:"));
    return idx >= 0 ? message.left(idx) : message;
}

// After this many content-check refusals, the last rejected clip is
// accepted anyway rather than looping indefinitely — matches
// ImageGeneratorCli's kMaxContentCheckRounds so both pipelines apply the
// same "two strikes and it's as good as it'll get" rule.
constexpr int kMaxContentCheckFailures = 2;

} // namespace

VideoGenerationWorkflow::VideoGenerationWorkflow(QObject *parent)
    : QObject(parent)
{
}

bool VideoGenerationWorkflow::isRunning() const
{
    return m_running;
}

void VideoGenerationWorkflow::start(const Request &request)
{
    if (m_running)
    {
        return;
    }
    m_running = true;
    m_cancelled = false;
    m_task = _run(request);
}

void VideoGenerationWorkflow::cancel()
{
    m_cancelled = true;
    emit progress(tr("Cancelling after the current step..."));
}

void VideoGenerationWorkflow::_finish(const QString &videoPath, const QString &errorMessage)
{
    m_running = false;
    emit finished(videoPath, errorMessage);
}

QCoro::Task<void> VideoGenerationWorkflow::_run(Request request)
{
    AbstractVideoGenerator *generator
        = AbstractVideoGenerator::ALL_VIDEO_GENERATORS().value(request.generatorId);
    if (!generator)
    {
        _finish({}, tr("Unknown video backend \"%1\".").arg(request.generatorId));
        co_return;
    }
    if (request.repeatUnchanged)
    {
        QStringList images = request.extraImagePaths;
        if (!request.imagePath.isEmpty())
        {
            images.prepend(request.imagePath);
        }
        QVariantMap settings = request.settings;
        settings.insert(QStringLiteral("preservePrompt"), true);
        emit progress(tr("Generating another video with the saved prompt and configuration..."));
        AbstractVideoGenerator::Result result;
        constexpr int maxBrowserRecoveries = 2;
        for (int recovery = 0; recovery <= maxBrowserRecoveries; ++recovery)
        {
            if (m_cancelled)
            {
                _finish({}, tr("Cancelled."));
                co_return;
            }
            result = co_await generator->generate(request.prompt, images, request.outputDir, settings);
            if (!result.videoPath.isEmpty() || !result.browserLost || !result.retryable
                || result.rejected || recovery == maxBrowserRecoveries || m_cancelled)
            {
                break;
            }
            emit progress(tr("Browser connection lost. Relaunching and retrying the same "
                             "prompt and images (%1/%2)...")
                .arg(recovery + 1).arg(maxBrowserRecoveries));
            co_await QCoro::sleepFor(std::chrono::milliseconds(500));
        }
        // Preserve a completed take even if Cancel was clicked while it ran;
        // the pane stops the remaining jobs in the batch.
        _finish(result.videoPath, result.errorMessage);
        co_return;
    }
    if (!request.decisionCli)
    {
        _finish({}, tr("No CLI available to drive the generation workflow."));
        co_return;
    }
    int maxTries = request.settings.value(QStringLiteral("maxTriesPerPrompt"), 10).toInt();
    if (maxTries < 1)
    {
        maxTries = 10;
    }

    QString currentPrompt = request.prompt;
    QString currentImage = request.imagePath;
    int round = 0;
    int contentCheckFailures = 0;
    // Re-prepended at every use rather than folded once into currentPrompt:
    // the decision CLI can wholesale-replace currentPrompt with a rewritten
    // one later (see "action": "prompt" below), which would otherwise lose
    // this prefix entirely from that round onward.
    const QString pitfallsPrefix = request.knownPitfalls.isEmpty()
        ? QString{} : request.knownPitfalls + QStringLiteral("\n");

    while (true)
    {
        ++round;

        // --- One round: up to maxTries attempts with small variations. ---
        QString videoPath;
        QString lastError;
        int sameTechnicalErrors = 0;
        for (int attempt = 0; attempt < maxTries && !m_cancelled; ++attempt)
        {
            const QString perturbed = perturbPrompt(currentPrompt, attempt);
            // The format line is re-ensured HERE, on the final text actually
            // sent, rather than living inside currentPrompt — perturbPrompt()
            // deliberately DROPS THE LAST SENTENCE on even attempts, which
            // silently ate the format line every other try (observed live:
            // "...slit midi dress. Classy fashion-editorial look." with the
            // "Video format: vertical (9:16)." gone). Keeping it out of
            // currentPrompt also means the perturbation varies real content
            // instead of chewing on boilerplate.
            const QString variantPrompt = pitfallsPrefix
                + ensureFormatStated(perturbed, request.videoFormatLabel);
            emit progress(tr("Round %1 — try %2/%3 with %4...")
                .arg(round).arg(attempt + 1).arg(maxTries).arg(generator->getName()));

            QStringList images;
            if (!currentImage.isEmpty() && QFileInfo::exists(currentImage))
            {
                images << currentImage;
            }
            // Static extra reference image(s) — always included, never
            // subject to the self-correction loop above (which only ever
            // regenerates currentImage).
            images << request.extraImagePaths;
            const QString outputDirLocal = request.outputDir;
            const QVariantMap settingsLocal = request.settings;
            AbstractVideoGenerator::Result result;
            result = co_await generator->generate(variantPrompt, images,
                                                  outputDirLocal, settingsLocal);
            if (!result.videoPath.isEmpty())
            {
                videoPath = result.videoPath;
                // Without the pitfalls prefix — it is re-added from
                // pitfallsPrefix on every future use, never folded in here,
                // so it can never end up duplicated across rounds.
                currentPrompt = perturbed;
                break;
            }
            if (!result.retryable)
            {
                _finish({}, tr("Video generator setup error: %1")
                    .arg(result.errorMessage));
                co_return;
            }
            // A technical failure repeating identically is deterministic
            // (broken selector, missing dependency...) — retrying or asking
            // the CLI for a new prompt cannot fix it: stop and report.
            // Rejections are prompt-dependent and keep retrying.
            if (!result.rejected
                && withoutDebugLogSuffix(result.errorMessage) == withoutDebugLogSuffix(lastError))
            {
                if (++sameTechnicalErrors >= 2)
                {
                    _finish({}, tr("Stopped: the same technical error repeats "
                        "— it will not fix itself. %1").arg(result.errorMessage));
                    co_return;
                }
            }
            else if (!result.rejected)
            {
                sameTechnicalErrors = 0;
            }
            lastError = result.errorMessage;
            emit progress((result.rejected
                ? tr("Rejected by %1: %2") : tr("Failed: %2 (%1)"))
                .arg(generator->getName(), result.errorMessage));
        }
        if (m_cancelled)
        {
            _finish({}, tr("Cancelled."));
            co_return;
        }

        // --- Whole round failed: the CLI decides prompt vs source image. ---
        if (videoPath.isEmpty())
        {
            emit progress(tr("%1 tries failed — asking %2 how to unblock...")
                .arg(maxTries).arg(request.decisionCli->getName()));

            QString decisionPrompt = tr(
                "The video generator (%1) rejected or failed our prompt %2 times "
                "in a row (last error: %3).\n"
                "Video brief: %4\n"
                "Current prompt:\n%5\n\n"
                "Decide how to unblock generation. Reply with STRICT JSON only:\n"
                "{\"action\": \"prompt\", \"prompt\": \"<a rewritten prompt that "
                "keeps the intent but avoids the rejection>\"}")
                .arg(generator->getName()).arg(maxTries)
                .arg(lastError, request.contextSummary, currentPrompt);
            const bool imagePossible = request.imageCli
                && !currentImage.isEmpty() && QFileInfo::exists(currentImage);
            if (imagePossible)
            {
                decisionPrompt += tr("\nor {\"action\": \"image\"} to regenerate the "
                    "source image instead (the prompt is kept).");
            }

            const QString outputDirLocal = request.outputDir;
            CliRunResult decision;
            decision = co_await request.decisionCli->runPrompt(decisionPrompt,
                                                               outputDirLocal);
            if (m_cancelled)
            {
                _finish({}, tr("Cancelled."));
                co_return;
            }

            const QJsonObject reply = parseJsonReply(decision.output);
            if (imagePossible
                && reply.value(QStringLiteral("action")).toString()
                    == QLatin1String("image"))
            {
                emit progress(tr("Regenerating the source image with %1...")
                    .arg(request.imageCli->getName()));
                const QString instruction = tr(
                    "Generate a NEW image that is close to it but clearly "
                    "different: same subject, theme, mood and overall style, but "
                    "a different scene, angle or composition, with no social "
                    "network icons, watermarks or text.");
                // Named local, assigned (not copy-initialized) from the
                // co_await expression: GCC 13 ICEs on temporaries owned by a
                // co_await call.
                QString newImage;
                newImage = co_await _regenerateImage(currentImage, outputDirLocal,
                    request.videoFormatLabel, instruction, request.imageCli);
                if (!newImage.isEmpty())
                {
                    currentImage = newImage;
                }
                else
                {
                    emit progress(tr("Image regeneration produced no file — "
                                     "keeping the current image."));
                }
            }
            else
            {
                const QString newPrompt
                    = reply.value(QStringLiteral("prompt")).toString().trimmed();
                if (!newPrompt.isEmpty())
                {
                    // No format line folded in here: it is re-ensured on the
                    // final text at every use (see variantPrompt above), so
                    // a rewrite can never drop it either.
                    currentPrompt = newPrompt;
                    emit progress(tr("Prompt rewritten by the CLI."));
                }
                else
                {
                    emit progress(tr("Unusable CLI reply — keeping the prompt "
                                     "(variations will differ anyway)."));
                }
            }
            continue; // next round
        }

        // --- A video exists: extract key frames and judge the clip. ---
        emit progress(tr("Video downloaded — extracting key frames..."));
        const QString framesDir = QDir(request.outputDir).absoluteFilePath(
            QStringLiteral("frames_check"));
        QString frameError;
        QStringList frames;
        frames = co_await _extractKeyFrames(videoPath, framesDir, &frameError);
        if (m_cancelled)
        {
            _finish({}, tr("Cancelled."));
            co_return;
        }
        if (frames.isEmpty())
        {
            // No frames means no check is possible: hand the video over
            // rather than throwing away a completed generation.
            emit progress(tr("Frame extraction failed (%1) — skipping the "
                             "content check.").arg(frameError));
            _finish(videoPath, {});
            co_return;
        }

        emit progress(tr("Checking the clip against the brief with %1...")
            .arg(request.decisionCli->getName()));
        QStringList frameRefs;
        for (const QString &frame : frames)
        {
            frameRefs << QStringLiteral("frames_check/") + frame;
        }
        const bool imageFixPossible = request.imageCli
            && !currentImage.isEmpty() && QFileInfo::exists(currentImage);
        QString checkPrompt = tr(
            "The files %1 in the current working directory are key frames "
            "extracted from a generated video.\n"
            "Video brief: %2\n"
            "Prompt used:\n%3\n\n"
            "Look at the frames and judge: is the clip on topic for the brief, "
            "and technically sound (no failed render, no blank or garbled "
            "frames, no unwanted text or watermarks, and any specific "
            "product mentioned in the brief kept its real design)?\n"
            "Reply with STRICT JSON only:\n"
            "{\"ok\": true|false, \"reason\": \"...\", \"cause\": "
            "\"image\"|\"prompt\"|\"randomness\", \"imageFix\": \"...\", "
            "\"improvedPrompt\": \"...\"}\n"
            "When ok is false, decide WHERE the problem actually lives:\n"
            "- \"image\": the defect is baked into the SOURCE IMAGE itself (a "
            "watermark/logo/brand text visible in it, wrong wardrobe/subject "
            "shown in it...) — no prompt rewrite can fix something the video "
            "backend is only animating from a fixed picture.\n"
            "- \"prompt\": the problem is about the motion/action/camera work "
            "the PROMPT TEXT ITSELF asked for (wrong pacing, wrong framing "
            "described in the prompt...) — a rewrite should target that "
            "specifically.\n"
            "- \"randomness\": the prompt and image are both fine, but THIS "
            "PARTICULAR render drifted for no describable reason (e.g. a "
            "product's design came out subtly different this one take even "
            "though nothing in the prompt or image asked for that) — "
            "retrying the EXACT SAME prompt and image, unchanged, is more "
            "likely to fix this than rewriting anything, since there is "
            "nothing wrong to describe a fix for.\n"
            "- \"imageFix\" (only when cause is \"image\"): a short, "
            "specific instruction describing what to remove or change IN THE "
            "IMAGE (e.g. \"Remove the watermark/logo text visible in the "
            "bottom-right corner.\"), keeping everything else about it "
            "identical.\n"
            "- \"improvedPrompt\" (only when cause is \"prompt\"): an "
            "improved version of the prompt that should avoid the problem. "
            "Omit both \"imageFix\" and \"improvedPrompt\" when cause is "
            "\"randomness\".")
            .arg(frameRefs.join(QStringLiteral(", ")),
                 request.contextSummary, currentPrompt);
        if (!imageFixPossible)
        {
            checkPrompt += tr("\nNo image regeneration is available this run — "
                "never reply \"image\", use \"prompt\" or \"randomness\" "
                "instead even for an image-rooted issue.");
        }

        const QString outputDirLocal = request.outputDir;
        CliRunResult check;
        check = co_await request.decisionCli->runPrompt(checkPrompt, outputDirLocal);
        if (m_cancelled)
        {
            _finish({}, tr("Cancelled."));
            co_return;
        }
        const QJsonObject verdict = parseJsonReply(check.output);
        if (!verdict.contains(QStringLiteral("ok"))
            || verdict.value(QStringLiteral("ok")).toBool())
        {
            if (!verdict.contains(QStringLiteral("ok")))
            {
                emit progress(tr("Unusable check reply — accepting the video."));
            }
            _finish(videoPath, {});
            co_return;
        }

        // Clip refused: set it aside and start over — the Gemini web session
        // cannot be continued, but the prompt/image can carry over.
        const QString reason = verdict.value(QStringLiteral("reason")).toString();
        emit progress(tr("Clip refused by the check: %1").arg(reason));
        if (!reason.isEmpty() && request.recordLesson)
        {
            // Recorded regardless of whether this refusal is about to hit
            // the acceptance cap below — a real, concrete pitfall was just
            // found either way, and it should inform every future
            // generation (see PromptLessons), not just this run's
            // remaining rounds.
            request.recordLesson(reason);
        }
        ++contentCheckFailures;
        if (contentCheckFailures >= kMaxContentCheckFailures)
        {
            // Two failures against the same brief is treated as evidence
            // the prompt/image are about as good as they will get, not a
            // signal to keep spending browser-automation rounds chasing
            // perfection that may never converge — accept this clip as
            // final instead of quarantining it and looping again.
            emit progress(tr("Accepting the video despite failing the "
                "content check %1 time(s) — further retries are unlikely "
                "to converge (%2).").arg(contentCheckFailures).arg(reason));
            _finish(videoPath, {});
            co_return;
        }
        const QString rejectedPath = QDir(request.outputDir).absoluteFilePath(
            QStringLiteral("rejected_round%1.mp4").arg(round));
        QFile::remove(rejectedPath);
        QFile::rename(videoPath, rejectedPath);

        const QString cause = verdict.value(QStringLiteral("cause")).toString();
        if (imageFixPossible && cause == QLatin1String("image"))
        {
            // A prompt rewrite cannot remove something baked into the
            // source image (a watermark, in the case that motivated this) —
            // regenerate the image with the SPECIFIC fix instead of blindly
            // retrying the same flawed picture every round.
            const QString imageFix
                = verdict.value(QStringLiteral("imageFix")).toString().trimmed();
            if (!imageFix.isEmpty())
            {
                emit progress(tr("Image-side issue — regenerating with %1: %2")
                    .arg(request.imageCli->getName(), imageFix));
                const QString instruction = tr(
                    "Fix this specific issue: %1\nOtherwise keep the image "
                    "identical — same subject, pose, outfit, background and "
                    "lighting.").arg(imageFix);
                // Named local, assigned (not copy-initialized) from the
                // co_await expression: GCC 13 ICEs on temporaries owned by a
                // co_await call.
                QString newImage;
                newImage = co_await _regenerateImage(currentImage, request.outputDir,
                    request.videoFormatLabel, instruction, request.imageCli);
                if (!newImage.isEmpty())
                {
                    currentImage = newImage;
                }
                else
                {
                    emit progress(tr("Image regeneration produced no file — "
                                     "keeping the current image."));
                }
                continue;
            }
            emit progress(tr("Check flagged an image-side issue but gave no "
                             "fix instruction — retrying unchanged instead."));
        }
        else if (cause == QLatin1String("randomness"))
        {
            // Neither the prompt nor the image is actually wrong — this
            // particular render just drifted (observed live: a product's
            // design came out subtly different on one take with nothing
            // describable to fix) — rewriting anything here would only
            // drift an already-good prompt further away for no reason.
            // currentPrompt/currentImage intentionally left untouched.
            emit progress(tr("Likely one-off rendering variance, not a "
                "prompt or image problem — retrying the same prompt and "
                "image unchanged."));
        }
        else
        {
            const QString improved
                = verdict.value(QStringLiteral("improvedPrompt")).toString().trimmed();
            if (!improved.isEmpty())
            {
                // Same as the rewrite path above: the format line is added
                // on the final text at every use, never folded in here.
                currentPrompt = improved;
            }
        }
    }
}

QCoro::Task<QString> VideoGenerationWorkflow::_regenerateImage(
    QString currentImage, QString outputDir, QString videoFormatLabel,
    QString instruction, AbstractCli *imageCli)
{
    const QString imagePrompt = tr(
        "You are given the image file '%1' in the current working directory "
        "as inspiration.\n%2\nUse a %3 format. Save the result in the current "
        "working directory as exactly 'generation_source.png'. Reply with "
        "just the file name.")
        .arg(fileReference(currentImage, outputDir), instruction,
             videoFormatLabel.isEmpty() ? tr("vertical (9:16)") : videoFormatLabel);
    // Named local, assigned (not copy-initialized) from the co_await
    // expression: GCC 13 ICEs on temporaries owned by a co_await call.
    CliRunResult imageResult;
    imageResult = co_await imageCli->runPrompt(imagePrompt, outputDir);
    Q_UNUSED(imageResult);
    const QString newImage = QDir(outputDir).absoluteFilePath(
        QStringLiteral("generation_source.png"));
    co_return QFileInfo::exists(newImage) ? newImage : QString{};
}

QCoro::Task<QStringList> VideoGenerationWorkflow::_extractKeyFrames(
    QString videoPath, QString framesDir, QString *errorMessage)
{
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
    {
        *errorMessage = tr("ffmpeg was not found on PATH.");
        co_return QStringList{};
    }

    QDir dir{framesDir};
    dir.mkpath(QStringLiteral("."));
    for (const QString &old : dir.entryList({QStringLiteral("frame_*.jpg")}, QDir::Files))
    {
        dir.remove(old);
    }

    // Up to 5 frames, at least 2 seconds apart (the escaped comma keeps the
    // expression's comma out of ffmpeg's filter separator).
    const QStringList args{
        QStringLiteral("-y"), QStringLiteral("-i"), videoPath,
        QStringLiteral("-vf"),
        QStringLiteral("select=isnan(prev_selected_t)+gte(t-prev_selected_t\\,2)"),
        QStringLiteral("-frames:v"), QStringLiteral("5"),
        QStringLiteral("-vsync"), QStringLiteral("vfr"),
        dir.absoluteFilePath(QStringLiteral("frame_%02d.jpg")),
    };
    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForStarted(5000))
    {
        *errorMessage = tr("Could not start ffmpeg (%1).").arg(process.errorString());
        co_return QStringList{};
    }
    // A QCoroProcess finished-signal awaiter resumes inside QProcess's own
    // signal emission. This local QProcess can then be destroyed before the
    // callback unwinds, which crashed just after all five frames were saved.
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < 120'000)
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    if (process.state() != QProcess::NotRunning)
    {
        *errorMessage = tr("ffmpeg timed out.");
        process.kill();
        process.waitForFinished(2000);
        co_return QStringList{};
    }

    const QStringList frames
        = dir.entryList({QStringLiteral("frame_*.jpg")}, QDir::Files, QDir::Name);
    if (frames.isEmpty())
    {
        *errorMessage = QString::fromUtf8(
            process.readAllStandardError().right(300));
    }
    co_return frames;
}
