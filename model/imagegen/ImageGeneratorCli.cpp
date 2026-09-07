// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "ImageGeneratorCli.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "AbstractCli.h"

namespace {

// Observed live (agy_test scratch reproduction): Antigravity's "replied with
// the filename but never wrote it" failure is plain flakiness, not a
// deterministic reaction to the prompt or to prior files in the working
// directory — an immediate retry with the IDENTICAL prompt succeeded right
// away. So attempts 1-2 are the same prompt; only once that plain retry has
// also failed does attempt 3 escalate to a stronger, more explicit
// imperative (mirrors VideoGenerationWorkflow's perturb-then-ask-CLI
// escalation, scaled down: no decision-CLI round-trip needed here, editing
// the instruction directly is enough to unstick it). Raised from 3 to 5:
// live logs showed a regeneration exhausting all 3 write-retries to pure
// flakiness (every attempt "replied with the filename but never wrote it"),
// losing the whole image — more headroom before giving up costs one extra
// CLI call in the common case but avoids that failure mode.
constexpr int kMaxAttemptsPerImage = 5;
// A second, independent budget for CONTENT problems (file written fine, but
// the picture itself misses the brief — product barely visible, subject
// facing the wrong way...). Kept small: this is a real extra CLI round-trip
// per retry (view the image, judge it, regenerate), unlike the write-retry
// loop above which is nearly free to repeat.
constexpr int kMaxContentCheckRounds = 2;

QString escalatedSuffix(const QString &fileName)
{
    return QStringLiteral(
        "\nThis is a RETRY: your previous reply named %1 but the file was never "
        "actually created. You MUST call your image generation tool THIS TURN — "
        "do not reply with only text or a file name. Generate the image now.")
        .arg(fileName);
}

// CLIs are asked for strict JSON but may wrap it anyway: parse between the
// first '{' and the last '}' (same technique as VideoGenerationWorkflow's
// clip check).
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

} // namespace

QString ImageGeneratorCli::getId() const
{
    return QStringLiteral("cli");
}

QString ImageGeneratorCli::getName() const
{
    return QStringLiteral("AI CLI");
}

QCoro::Task<AbstractImageGenerator::Result> ImageGeneratorCli::generate(
    const QString &prompt, const QStringList &referenceImagePaths, int imageCount,
    const QString &outputDir, const QVariantMap &settings, AbstractCli *cli,
    const std::function<void(const QString &)> &logProgress,
    const std::function<void(const QString &)> &recordLesson) const
{
    const QString formatLabel = settings.value(QStringLiteral("formatLabel")).toString();
    const QString knownPitfalls = settings.value(QStringLiteral("knownPitfalls")).toString();
    Result result;
    if (!cli)
    {
        result.errorMessage = QObject::tr("No CLI selected for image generation.");
        co_return result;
    }
    const auto log = [&logProgress](const QString &message) {
        if (logProgress)
        {
            logProgress(message);
        }
    };
    const auto recordPitfall = [&recordLesson](const QString &lesson) {
        if (recordLesson)
        {
            recordLesson(lesson);
        }
    };

    const QDir dir{outputDir};

    // One CLI call PER IMAGE, even for a slideshow — NOT one call asked to
    // write N files. Antigravity's image tool is scoped to a single file
    // per call ("save it to THE file path... then stop"): a one-shot
    // request for 5 files just made it narrate the filenames as text
    // instead of generating anything (observed live). One call per image
    // matches every CLI's real single-image contract; the cost is
    // imageCount separate invocations instead of one.
    for (int i = 1; i <= imageCount; ++i)
    {
        const QString fileName = imageCount == 1
            ? QStringLiteral("image.png")
            : QStringLiteral("image_%1.png").arg(i, 2, 10, QLatin1Char('0'));

        // See AbstractCli.h / CliAntigravity.cpp: without this exact
        // marker, Antigravity's preparePrompt() concludes "plain text
        // only, do NOT use tools" for the call. Harmless no-op for CLIs
        // that don't look for it (Claude, Codex).
        QString basePrompt = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER)
            + QStringLiteral("\n") + prompt.trimmed();
        QStringList referenceFileNames;
        if (!referenceImagePaths.isEmpty())
        {
            for (const QString &path : referenceImagePaths)
            {
                referenceFileNames << QFileInfo{path}.fileName();
            }
            basePrompt += QStringLiteral(
                "\nReference image(s) available in the current working directory: "
                "%1.\nProduct fidelity — non-negotiable: the reference photo(s) "
                "show the REAL product exactly as it must appear. Reproduce its "
                "exact design — same silhouette (e.g. closed pump vs open sandal "
                "vs platform), same heel shape and height, same straps/closures, "
                "same material and color, same hardware/decorative details. Do "
                "NOT substitute a different-but-similar item.")
                .arg(referenceFileNames.join(QStringLiteral(", ")));
        }
        if (imageCount > 1)
        {
            basePrompt += QStringLiteral(
                "\nThis is image %1 of %2 in a coherent slideshow series — keep "
                "the same subject, palette and overall style as the rest of the "
                "set, varying only the composition/angle already described above.")
                .arg(i).arg(imageCount);
        }
        if (!formatLabel.isEmpty())
        {
            // The image CLI has no separate aspect-ratio control the way a
            // video backend can — this text instruction is the only lever,
            // so it must be explicit and unmissable rather than trusting
            // the suggested prompt text to have mentioned it.
            basePrompt += QStringLiteral(
                "\nOutput aspect ratio: exactly %1. Fill the entire canvas in "
                "that ratio — no letterboxing, pillarboxing, or blank bars.")
                .arg(formatLabel);
        }
        if (!knownPitfalls.isEmpty())
        {
            basePrompt += QStringLiteral("\n%1").arg(knownPitfalls);
        }
        basePrompt += QStringLiteral(
            "\nUsing your file tools, save the result in the current working "
            "directory as exactly: %1. Do not modify any other file. Reply with "
            "just the output file name.").arg(fileName);

        const QString imagePath = dir.absoluteFilePath(fileName);
        // A content-round regeneration can itself technically fail to
        // produce a file (observed live: the write-retry budget can run out
        // entirely on a regeneration attempt) — without this backup, that
        // used to hard-fail the WHOLE image, discarding a perfectly real
        // file from the previous round that only had a composition issue.
        // An imperfect-but-real image beats none at all, same principle as
        // the "kept despite failing the content check" fallback below.
        const QString backupPath = imagePath + QStringLiteral(".prevround");
        QString currentPrompt = basePrompt;
        bool accepted = false;
        QString lastContentIssue;

        for (int contentRound = 1;
             contentRound <= kMaxContentCheckRounds && !accepted; ++contentRound)
        {
            if (contentRound > 1)
            {
                // Otherwise the write-verification below would find the
                // PREVIOUS (rejected) round's file still sitting at
                // imagePath and wrongly conclude the regeneration succeeded
                // without the CLI having done anything.
                QFile::remove(backupPath);
                QFile::rename(imagePath, backupPath);
            }
            QString lastErrorMessage;
            bool produced = false;
            for (int attempt = 1; attempt <= kMaxAttemptsPerImage && !produced; ++attempt)
            {
                // Attempts 1-2 use the identical prompt (a plain retry alone
                // fixed this live, every time it was tried) — only the last
                // attempt escalates to a stronger, more explicit instruction.
                const QString attemptPrompt = attempt < kMaxAttemptsPerImage
                    ? currentPrompt : currentPrompt + escalatedSuffix(fileName);

                if (attempt > 1)
                {
                    log(QObject::tr("%1 — image %2/%3: retrying (attempt %4/%5)%6...")
                        .arg(cli->getName()).arg(i).arg(imageCount)
                        .arg(attempt).arg(kMaxAttemptsPerImage)
                        .arg(attempt == kMaxAttemptsPerImage
                            ? QObject::tr(" with a stronger instruction") : QString()));
                }

                // Named local, assigned (not copy-initialized) from the
                // co_await expression: GCC 13 ICEs on temporaries owned by a
                // co_await call.
                const QDateTime callStart = QDateTime::currentDateTime();
                CliRunResult cliResult;
                cliResult = co_await cli->runPrompt(attemptPrompt, outputDir);

                produced = cliResult.processStarted && cliResult.exitCode == 0
                    && QFileInfo::exists(imagePath);

                // Confirmed live: Antigravity's file tool can silently write
                // to a fixed internal location instead of outputDir (see
                // CliAntigravity::outputFallbackDirs()) — check there before
                // concluding nothing was produced. The mtime guard matters:
                // that directory is a long-lived, shared dump used across
                // many unrelated tasks/projects, so an old file that merely
                // happens to share this generic name (image_01.png...) must
                // not be mistaken for this call's output.
                QString recoveredFrom;
                if (!produced && cliResult.processStarted && cliResult.exitCode == 0)
                {
                    for (const QString &fallbackDir : cli->outputFallbackDirs())
                    {
                        const QString fallbackPath
                            = QDir{fallbackDir}.absoluteFilePath(fileName);
                        const QFileInfo fallbackInfo{fallbackPath};
                        if (!fallbackInfo.exists() || fallbackInfo.lastModified() < callStart)
                        {
                            continue;
                        }
                        QFile::remove(imagePath);
                        if (QFile::rename(fallbackPath, imagePath)
                            || (QFile::copy(fallbackPath, imagePath)
                                && QFile::remove(fallbackPath)))
                        {
                            produced = true;
                            recoveredFrom = fallbackDir;
                            break;
                        }
                    }
                }
                if (produced)
                {
                    if (!recoveredFrom.isEmpty())
                    {
                        log(QObject::tr("%1 — image %2/%3: the file landed in %4 "
                            "instead of the requested folder — recovered it from "
                            "there.").arg(cli->getName()).arg(i).arg(imageCount)
                            .arg(recoveredFrom));
                    }
                    else if (attempt > 1)
                    {
                        log(QObject::tr("%1 — image %2/%3: succeeded on retry.")
                            .arg(cli->getName()).arg(i).arg(imageCount));
                    }
                    break;
                }

                // A specific, previously-seen failure mode: the CLI's reply
                // text names exactly the file it was asked for but never
                // actually created it — usually its own preamble or a policy
                // refusal suppressed tool/file-write use for this call.
                // Reproduced live: this is plain flakiness (an immediate,
                // unmodified retry succeeded every time it was tried) rather
                // than a deterministic reaction to the prompt, hence retrying
                // before giving up. Distinguishing this from "crashed" /
                // "produced nothing" also saves a debugging round-trip.
                // imageCount > 1 reports which image in the set failed — the
                // ones already in result.imagePaths are real, kept for
                // reference even though the job as a whole may end up
                // reported failed.
                const bool describedButNotWritten = cliResult.processStarted
                    && cliResult.exitCode == 0 && cliResult.output.contains(fileName);
                lastErrorMessage = describedButNotWritten
                    ? QObject::tr("%1 replied with the expected file name (%2) but "
                        "never actually wrote it (image %3/%4) — it may have been "
                        "steered into a text-only reply for this prompt (no tool/"
                        "file-write use). Full CLI output: %5")
                        .arg(cli->getName(), fileName).arg(i).arg(imageCount)
                        .arg(cliResult.output.left(500))
                    : QObject::tr("%1 failed on image %2/%3: %4")
                        .arg(cli->getName()).arg(i).arg(imageCount)
                        .arg(cliResult.errorOutput.isEmpty()
                            ? cliResult.output.left(500) : cliResult.errorOutput.left(500));
                log(lastErrorMessage);
            }

            if (!produced)
            {
                if (QFileInfo::exists(backupPath))
                {
                    QFile::rename(backupPath, imagePath);
                    log(QObject::tr("%1 — image %2/%3: regeneration failed "
                        "technically after %4 attempts — keeping the previous "
                        "round's image despite its content issue (%5).")
                        .arg(cli->getName()).arg(i).arg(imageCount)
                        .arg(kMaxAttemptsPerImage).arg(lastContentIssue.left(300)));
                    accepted = true;
                    break;
                }
                result.errorMessage = QObject::tr("%1 (gave up after %2 attempts)")
                    .arg(lastErrorMessage).arg(kMaxAttemptsPerImage);
                co_return result;
            }
            // A fresh file exists now — the previous round's backup (if any)
            // is no longer needed as a fallback.
            QFile::remove(backupPath);

            // The file exists — but does it actually satisfy the brief?
            // Writing successfully says nothing about whether the subject
            // matter it was asked for (e.g. a specific product) is actually
            // visible, or whether the pose/orientation makes sense. Ask the
            // same CLI, since it can already view images, to judge before
            // accepting.
            log(QObject::tr("%1 — image %2/%3: checking the result against "
                "the brief...").arg(cli->getName()).arg(i).arg(imageCount));
            QString checkPrompt = QObject::tr(
                "The file %1 in the current working directory is an image just "
                "generated for a short-form social-media post.\nBrief: %2\n\n"
                "Look at the image and judge: does it match the brief's SUBJECT "
                "and composition — is any specific product/detail the brief "
                "calls out (e.g. shoes, an accessory) CLEARLY VISIBLE and a real "
                "focal point, not reduced to a tiny or blurry detail in a wide "
                "shot? Is the pose, and the body/head orientation, natural and "
                "coherent (not facing a nonsensical direction, no anatomical "
                "glitches)?")
                .arg(fileName, prompt.trimmed());
            if (!referenceFileNames.isEmpty())
            {
                // Root-caused live: a text-only brief check ("is a shoe
                // visible") never catches the model substituting a
                // DIFFERENT shoe — e.g. a plain patent pump instead of the
                // reference's studded platform sandal — because nothing was
                // ever compared against the actual reference photo. This is
                // the check that would have caught that.
                checkPrompt += QObject::tr(
                    "\n\nProduct fidelity — check THIS CAREFULLY: compare the "
                    "product in %1 against the reference product photo(s) also "
                    "in the current working directory (%2), which show the REAL "
                    "product exactly as it must appear. It must be the SAME item "
                    "— same silhouette (closed pump vs open sandal vs "
                    "platform...), same heel shape and height, same straps/"
                    "closures, same material and color, same hardware/"
                    "decorative details. A different-but-similar item is a FAIL "
                    "even if the rest of the composition is fine.")
                    .arg(fileName, referenceFileNames.join(QStringLiteral(", ")));
            }
            checkPrompt += QObject::tr(
                "\nReply with STRICT JSON only:\n"
                "{\"ok\": true|false, \"reason\": \"...\", \"fix\": \"...\"}\n"
                "\"fix\" (only when ok is false): a short, specific instruction "
                "for what to change in a regenerated image.");
            // Named local, assigned (not copy-initialized) from the co_await
            // expression: GCC 13 ICEs on temporaries owned by a co_await call.
            CliRunResult checkResult;
            checkResult = co_await cli->runPrompt(checkPrompt, outputDir);
            const QJsonObject verdict = parseJsonReply(checkResult.output);
            if (!checkResult.processStarted || checkResult.exitCode != 0
                || !verdict.contains(QStringLiteral("ok"))
                || verdict.value(QStringLiteral("ok")).toBool())
            {
                accepted = true;
                break;
            }

            lastContentIssue = verdict.value(QStringLiteral("reason")).toString();
            const QString fix = verdict.value(QStringLiteral("fix")).toString().trimmed();
            // Recorded regardless of whether this round still has budget to
            // retry — a real, concrete pitfall was just found either way,
            // and it should inform every future generation, not just this
            // one's remaining attempts.
            if (!lastContentIssue.isEmpty())
            {
                recordPitfall(lastContentIssue);
            }
            if (contentRound >= kMaxContentCheckRounds || fix.isEmpty())
            {
                // Out of budget (or the CLI gave no actionable fix): a flawed
                // composition is still better than no image at all for a
                // slideshow — keep it, but make the miss visible instead of
                // silently shipping it.
                log(QObject::tr("%1 — image %2/%3: kept despite failing the "
                    "content check (%4).").arg(cli->getName()).arg(i)
                    .arg(imageCount).arg(lastContentIssue.left(300)));
                accepted = true;
                break;
            }
            log(QObject::tr("%1 — image %2/%3: failed the content check (%4) — "
                "regenerating...").arg(cli->getName()).arg(i).arg(imageCount)
                .arg(lastContentIssue.left(300)));
            currentPrompt = basePrompt + QObject::tr(
                "\nIMPORTANT — a previous attempt missed the brief: %1. "
                "Correct this specifically while keeping everything else about "
                "the composition.").arg(fix);
        }

        result.imagePaths << imagePath;
    }
    co_return result;
}

DECLARE_IMAGE_GENERATOR(ImageGeneratorCli)
