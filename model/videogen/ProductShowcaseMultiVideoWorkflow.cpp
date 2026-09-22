#include "ProductShowcaseMultiVideoWorkflow.h"

#include <chrono>
#include <QCoro/QCoroTimer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>

#include "AbstractCli.h"
#include "VideoClipProcessing.h"
#include "model/imagegen/AbstractImageGenerator.h"

ProductShowcaseMultiVideoWorkflow::ProductShowcaseMultiVideoWorkflow(QObject *parent)
    : AbstractMultiVideoWorkflow(parent)
{
}

ProductShowcaseMultiVideoWorkflow::~ProductShowcaseMultiVideoWorkflow()
{
    if (m_currentVideoWorkflow)
    {
        m_currentVideoWorkflow->cancel();
    }
}

QString ProductShowcaseMultiVideoWorkflow::workflowName() const
{
    return tr("Product Showcase (Intro + Products)");
}

QString ProductShowcaseMultiVideoWorkflow::workflowId() const
{
    return QStringLiteral("product-showcase");
}

QString ProductShowcaseMultiVideoWorkflow::videoFileName(int stepIndex, int takeIndex) const
{
    const int stepNumber = stepIndex + 1;
    const int takeNumber = takeIndex + 1;
    if (stepIndex == 0)
    {
        return QStringLiteral("%1_step%2_intro_take%3.mp4")
            .arg(stepNumber, 2, 10, QLatin1Char('0'))
            .arg(stepNumber)
            .arg(takeNumber, 2, 10, QLatin1Char('0'));
    }
    else
    {
        return QStringLiteral("%1_step%2_product%3_take%4.mp4")
            .arg(stepNumber, 2, 10, QLatin1Char('0'))
            .arg(stepNumber)
            .arg(stepIndex)
            .arg(takeNumber, 2, 10, QLatin1Char('0'));
    }
}

void ProductShowcaseMultiVideoWorkflow::configure(
    int totalSteps,
    const QString &introPrompt, int introTakes,
    const QString &afterIntroPrompt, int afterIntroTakes,
    const QList<QList<ImageInput>> &stepImages)
{
    m_steps.clear();
    const int count = qMax(2, totalSteps);

    // Step 0: Intro Step
    Step introStep;
    introStep.type = StepType::Intro;
    introStep.title = tr("Step 1 (Intro)");
    introStep.prompt = introPrompt;
    introStep.videoCount = qMax(1, introTakes);
    if (!stepImages.isEmpty() && stepImages.size() > 0)
    {
        introStep.images = stepImages.at(0);
    }
    m_steps.append(introStep);

    // Steps 1..N: After-intro steps (Products)
    for (int i = 1; i < count; ++i)
    {
        Step productStep;
        productStep.type = StepType::AfterIntro;
        productStep.title = tr("Step %1 (Product %2)").arg(i + 1).arg(i);
        productStep.prompt = afterIntroPrompt;
        productStep.videoCount = qMax(1, afterIntroTakes);
        if (i < stepImages.size())
        {
            productStep.images = stepImages.at(i);
        }
        m_steps.append(productStep);
    }

    initializeTakes();
}

void ProductShowcaseMultiVideoWorkflow::start(
    const QString &projectDir, const QString &outputDir,
    const QString &generatorId, const QVariantMap &settings,
    AbstractCli *decisionCli, AbstractCli *imageCli,
    const QString &videoFormatLabel)
{
    m_projectDir = projectDir;
    m_outputDir = outputDir;
    m_generatorId = generatorId;
    m_settings = settings;
    m_decisionCli = decisionCli;
    m_imageCli = imageCli;
    m_videoFormatLabel = videoFormatLabel;
    m_cancelled = false;

    QDir{m_outputDir}.mkpath(QStringLiteral("."));

    if (m_takes.isEmpty())
    {
        initializeTakes();
    }

    setState(State::Running);
    m_executionTask = _runWorkflow();
}

void ProductShowcaseMultiVideoWorkflow::resume()
{
    if (m_state == State::Running)
    {
        return;
    }
    m_cancelled = false;
    setState(State::Running);
    emit progress(tr("Resuming multi-video generation (%1/%2 completed)...")
        .arg(completedVideoCount()).arg(totalVideoCount()));
    m_executionTask = _runWorkflow();
}

void ProductShowcaseMultiVideoWorkflow::cancel()
{
    m_cancelled = true;
    if (m_currentVideoWorkflow)
    {
        m_currentVideoWorkflow->cancel();
    }
    setState(State::Cancelled, tr("Cancelled by user."));
}

void ProductShowcaseMultiVideoWorkflow::selectIntroTake(int takeIndex)
{
    setSelectedTakeIndex(0, takeIndex);
    m_selectedIntroTakeIndex = takeIndex;
    for (const auto &take : m_takes)
    {
        if (take.stepIndex == 0 && take.takeIndex == takeIndex)
        {
            if (!take.conversationUrl.isEmpty())
            {
                m_conversationUrl = take.conversationUrl;
            }
            break;
        }
    }
    _saveState();
    updateMergedVideo();
    if (m_state != State::Completed && completedVideoCount() < totalVideoCount())
    {
        resume();
    }
}

void ProductShowcaseMultiVideoWorkflow::selectTake(int stepIndex, int takeIndex)
{
    setSelectedTakeIndex(stepIndex, takeIndex);
    if (stepIndex == 0)
    {
        selectIntroTake(takeIndex);
        return;
    }
    _saveState();
    updateMergedVideo();
    if (m_state != State::Completed && completedVideoCount() < totalVideoCount())
    {
        resume();
    }
}

void ProductShowcaseMultiVideoWorkflow::resetStep(int stepIndex, bool resetSubsequentSteps)
{
    if (stepIndex < 0 || stepIndex >= m_steps.size())
    {
        return;
    }

    const int maxStep = resetSubsequentSteps ? m_steps.size() - 1 : stepIndex;
    for (int s = stepIndex; s <= maxStep; ++s)
    {
        m_selectedTakeIndices.remove(s);
        if (s == 0)
        {
            m_selectedIntroTakeIndex = -1;
            m_conversationUrl.clear();
        }

        for (auto &t : m_takes)
        {
            if (t.stepIndex == s)
            {
                if (!t.filePath.isEmpty() && QFile::exists(t.filePath))
                {
                    QFile::remove(t.filePath);
                }
                const QString expectedPath = QDir{m_outputDir}.absoluteFilePath(t.expectedFileName);
                if (QFile::exists(expectedPath))
                {
                    QFile::remove(expectedPath);
                }
                // Check all subdirectories in m_outputDir for expectedFileName (e.g. finalized folders like QS-000Y)
                if (!m_outputDir.isEmpty())
                {
                    const QDir outDir{m_outputDir};
                    const auto subDirs = outDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const auto &sub : subDirs)
                    {
                        const QString subFile = outDir.absoluteFilePath(sub + QLatin1Char('/') + t.expectedFileName);
                        if (QFile::exists(subFile))
                        {
                            QFile::remove(subFile);
                        }
                    }
                    // Remove temporary working folder if present
                    const QString workingDir = _workingDirectory().absoluteFilePath(
                        QStringLiteral("working_step%1_take%2").arg(s + 1).arg(t.takeIndex + 1));
                    if (QDir{workingDir}.exists())
                    {
                        QDir{workingDir}.removeRecursively();
                    }
                }
                // Also check m_projectDir if different
                if (!m_projectDir.isEmpty() && m_projectDir != m_outputDir)
                {
                    const QString projFile = QDir{m_projectDir}.absoluteFilePath(t.expectedFileName);
                    if (QFile::exists(projFile))
                    {
                        QFile::remove(projFile);
                    }
                }

                t.filePath.clear();
                t.completed = false;
                t.failed = false;
                t.errorMessage.clear();
                t.conversationUrl.clear();
                t.promptSubmitted = false;
            }
        }
    }

    setState(State::Paused, tr("Step %1 reset.").arg(stepIndex + 1));
    updateMergedVideo();
    _saveState();
}

void ProductShowcaseMultiVideoWorkflow::regenerateStep(int stepIndex, int newTakesCount, bool replaceExisting)
{
    if (stepIndex < 0 || stepIndex >= m_steps.size() || newTakesCount <= 0)
    {
        return;
    }

    if (replaceExisting)
    {
        resetStep(stepIndex, false);
        QList<TakeInfo> filteredTakes;
        for (const auto &t : m_takes)
        {
            if (t.stepIndex != stepIndex)
            {
                filteredTakes << t;
            }
        }
        for (int i = 0; i < newTakesCount; ++i)
        {
            TakeInfo newTake;
            newTake.stepIndex = stepIndex;
            newTake.takeIndex = i;
            newTake.expectedFileName = videoFileName(stepIndex, i);
            newTake.completed = false;
            newTake.failed = false;
            filteredTakes << newTake;
        }
        m_takes = filteredTakes;
        m_steps[stepIndex].videoCount = newTakesCount;
    }
    else
    {
        int existingCount = 0;
        for (const auto &t : m_takes)
        {
            if (t.stepIndex == stepIndex)
            {
                existingCount++;
            }
        }
        for (int i = 0; i < newTakesCount; ++i)
        {
            TakeInfo newTake;
            newTake.stepIndex = stepIndex;
            newTake.takeIndex = existingCount + i;
            newTake.expectedFileName = videoFileName(stepIndex, newTake.takeIndex);
            newTake.completed = false;
            newTake.failed = false;
            m_takes.append(newTake);
        }
        m_steps[stepIndex].videoCount = existingCount + newTakesCount;
        m_selectedTakeIndices.remove(stepIndex);
        if (stepIndex == 0)
        {
            m_selectedIntroTakeIndex = -1;
        }
    }

    setState(State::Paused, tr("Ready to generate new take(s) for Step %1.").arg(stepIndex + 1));
    updateMergedVideo();
    _saveState();
}

void ProductShowcaseMultiVideoWorkflow::_saveState()
{
    if (!m_outputDir.isEmpty())
    {
        if (m_generationShortCode.isEmpty())
        {
            saveToFile(QDir{m_outputDir}.absoluteFilePath(QStringLiteral("multi_video_workflow.json")));
        }
        else
        {
            const QString manifest = QDir{m_outputDir}.absoluteFilePath(
                m_generationShortCode + QStringLiteral("/multi_video_workflow.json"));
            saveToFile(manifest);
        }
    }
    if (!m_projectDir.isEmpty() && m_projectDir != m_outputDir)
    {
        saveToFile(QDir{m_projectDir}.absoluteFilePath(QStringLiteral("multi_video_workflow.json")));
    }
}

QCoro::Task<void> ProductShowcaseMultiVideoWorkflow::_runWorkflow()
{
    QString organizationError;
    if (!_organizeWorkingFiles(&organizationError))
    {
        setState(State::Paused, organizationError);
        _saveState();
        emit paused(organizationError, completedVideoCount(), totalVideoCount());
        co_return;
    }
    // Ensure take file paths are pre-filled
    for (auto &take : m_takes)
    {
        if (take.expectedFileName.isEmpty())
        {
            take.expectedFileName = videoFileName(take.stepIndex, take.takeIndex);
        }
        const QString expectedPath = QDir{m_outputDir}.absoluteFilePath(take.expectedFileName);
        if (!take.completed && QFileInfo::exists(expectedPath) && QFileInfo{expectedPath}.size() > 1024)
        {
            take.completed = true;
            take.filePath = expectedPath;
        }
        if (take.completed && take.conversationUrl.isEmpty() && !m_conversationUrl.isEmpty())
        {
            take.conversationUrl = m_conversationUrl;
        }
    }
    _saveState();

    for (int idx = 0; idx < m_takes.size(); ++idx)
    {
        if (m_cancelled)
        {
            setState(State::Cancelled, tr("Cancelled."));
            _saveState();
            emit failed(tr("Cancelled."));
            co_return;
        }

        auto &take = m_takes[idx];
        if (take.completed)
        {
            continue; // already successfully generated
        }

        if (take.stepIndex < 0 || take.stepIndex >= m_steps.size())
        {
            continue;
        }

        auto &step = m_steps[take.stepIndex];
        const QString stepTag = tr("Step %1/%2 Take %3/%4")
            .arg(take.stepIndex + 1).arg(m_steps.size())
            .arg(take.takeIndex + 1).arg(step.videoCount);

        emit progress(tr("[%1] Starting %2...").arg(stepTag, take.expectedFileName));
        emit takeStarted(take.stepIndex, take.takeIndex, take.expectedFileName);

        // Prepare working directory for this take
        const QString takeDir = _workingDirectory().absoluteFilePath(
            QStringLiteral("working_step%1_take%2")
                .arg(take.stepIndex + 1).arg(take.takeIndex + 1));
        QDir{takeDir}.mkpath(QStringLiteral("."));

        // Step inputs directory (shared across takes of this step)
        const QString stepDir = _workingDirectory().absoluteFilePath(
            QStringLiteral("step_%1_inputs").arg(take.stepIndex + 1));
        QDir{stepDir}.mkpath(QStringLiteral("."));

        // Step 1: Ensure image inputs are ready (regenerated if requested)
        const ImagePrepStatus prepStatus = co_await _ensureStepImagesReady(step, stepDir);
        if (m_cancelled)
        {
            setState(State::Cancelled, tr("Cancelled."));
            emit failed(tr("Cancelled."));
            co_return;
        }
        if (prepStatus == ImagePrepStatus::QuotaExceeded)
        {
            const QString pauseMsg = tr("Image generation quota reached (%1). Resume once limits are lifted.")
                .arg(m_imageCli ? m_imageCli->getName() : QStringLiteral("CLI"));
            take.failed = false;
            take.errorMessage.clear();
            setState(State::Paused, pauseMsg);
            _saveState();
            emit progress(pauseMsg);
            emit paused(pauseMsg, completedVideoCount(), totalVideoCount());
            co_return;
        }
        if (prepStatus == ImagePrepStatus::Failed)
        {
            const QString err = tr("[%1] Image preparation failed (could not isolate/regenerate image on white background with %2).")
                .arg(stepTag, m_imageCli ? m_imageCli->getName() : QStringLiteral("CLI"));
            emit progress(err);
            take.failed = true;
            take.errorMessage = err;
            _saveState();
            setState(State::Error, err);
            emit failed(err);
            co_return;
        }

        // Step 2: Configure and run VideoGenerationWorkflow for this take
        VideoGenerationWorkflow::Request request;
        request.generatorId = m_generatorId;
        request.prompt = step.prompt;
        request.outputDir = takeDir;
        request.contextSummary = tr("Multi-video %1: %2").arg(step.title, step.prompt);
        request.videoFormatLabel = m_videoFormatLabel;
        request.settings = m_settings;
        request.decisionCli = m_decisionCli;
        request.imageCli = m_imageCli;
        request.multiVideoContinuation = take.stepIndex > 0;

        // Multi-video conversation management:
        // Clarification: Each video of step 1 is run in a new chat.
        // Only videos of step 2 and more are run in the previous chat.
        // Before going to step 2, if multiple videos were generated for step 1,
        // user selects which intro video to use for subsequent steps.
        if (take.stepIndex == 0)
        {
            request.chatMode = QStringLiteral("fresh");
            request.messageAction = QStringLiteral("new");
            request.conversationUrl.clear();
        }
        else
        {
            // Step 2 or later: check if intro video has been selected
            if (m_selectedIntroTakeIndex < 0)
            {
                QList<TakeInfo> step1Takes;
                bool step1AllDone = true;
                for (const auto &t : m_takes)
                {
                    if (t.stepIndex == 0)
                    {
                        step1Takes << t;
                        if (!t.completed)
                        {
                            step1AllDone = false;
                        }
                    }
                }

                if (!step1AllDone)
                {
                    continue;
                }

                if (step1Takes.size() <= 1)
                {
                    m_selectedIntroTakeIndex = 0;
                    if (!step1Takes.isEmpty())
                    {
                        if (step1Takes[0].conversationUrl.isEmpty())
                        {
                            step1Takes[0].conversationUrl = m_conversationUrl;
                        }
                        m_conversationUrl = step1Takes[0].conversationUrl;
                    }
                }
                else
                {
                    // Multiple intro takes: pause and ask user to pick one
                    const QString pauseMsg = tr("Please select the intro video to continue with subsequent steps.");
                    setState(State::Paused, pauseMsg);
                    _saveState();
                    emit introSelectionRequested(step1Takes);
                    emit progress(pauseMsg);
                    emit paused(pauseMsg, completedVideoCount(), totalVideoCount());
                    co_return;
                }
            }
            else if (m_conversationUrl.isEmpty())
            {
                for (const auto &t : m_takes)
                {
                    if (t.stepIndex == 0 && t.takeIndex == m_selectedIntroTakeIndex)
                    {
                        m_conversationUrl = t.conversationUrl;
                        break;
                    }
                }
            }

            request.chatMode = QStringLiteral("conversation");
            request.messageAction = take.promptSubmitted && !take.conversationUrl.isEmpty()
                ? QStringLiteral("edit_last") : QStringLiteral("new");
            request.conversationUrl = take.promptSubmitted && !take.conversationUrl.isEmpty()
                ? take.conversationUrl : m_conversationUrl;
        }

        if (!step.images.isEmpty())
        {
            const auto &primary = step.images.at(0);
            request.imagePath = !primary.processedPath.isEmpty() ? primary.processedPath : primary.originalPath;
            for (int imgIdx = 1; imgIdx < step.images.size(); ++imgIdx)
            {
                const auto &extra = step.images.at(imgIdx);
                const QString extraPath = !extra.processedPath.isEmpty() ? extra.processedPath : extra.originalPath;
                if (!extraPath.isEmpty() && QFileInfo::exists(extraPath))
                {
                    request.extraImagePaths << extraPath;
                }
            }
        }

        auto workflow = std::make_unique<VideoGenerationWorkflow>();
        m_currentVideoWorkflow = workflow.get();

        connect(workflow.get(), &VideoGenerationWorkflow::progress,
                this, [this, stepTag](const QString &msg) {
            emit progress(QStringLiteral("[%1] %2").arg(stepTag, msg));
        });

        QString generatedVideoPath;
        QString workflowError;
        bool workflowDone = false;

        connect(workflow.get(), &VideoGenerationWorkflow::finished,
                this, [&generatedVideoPath, &workflowError, &workflowDone]
                (const QString &videoPath, const QString &err) {
            generatedVideoPath = videoPath;
            workflowError = err;
            workflowDone = true;
        });

        workflow->start(request);

        // Await workflow completion asynchronously
        while (!workflowDone)
        {
            co_await QCoro::sleepFor(std::chrono::milliseconds(200));
        }

        if (!workflow->lastConversationUrl().isEmpty())
        {
            take.conversationUrl = workflow->lastConversationUrl();
            if (take.stepIndex == 0)
            {
                if (m_selectedIntroTakeIndex == take.takeIndex || m_conversationUrl.isEmpty())
                {
                    m_conversationUrl = take.conversationUrl;
                }
            }
            else
            {
                m_conversationUrl = workflow->lastConversationUrl();
            }
        }

        take.promptSubmitted |= workflow->promptSubmitted();
        m_currentVideoWorkflow = nullptr;

        if (m_cancelled)
        {
            setState(State::Cancelled, tr("Cancelled."));
            _saveState();
            emit failed(tr("Cancelled."));
            co_return;
        }

        if (!generatedVideoPath.isEmpty() && QFileInfo::exists(generatedVideoPath))
        {
            // Successfully generated! Move to final destination
            const QString finalVideoPath = QDir{m_outputDir}.absoluteFilePath(take.expectedFileName);
            QFile::remove(finalVideoPath);
            if (!QDir{}.rename(generatedVideoPath, finalVideoPath))
            {
                if (!QFile::copy(generatedVideoPath, finalVideoPath))
                {
                    const QString error = tr("Could not save the generated clip to %1. The download is still at %2.")
                        .arg(finalVideoPath, generatedVideoPath);
                    take.failed = true;
                    take.errorMessage = error;
                    setState(State::Paused, error);
                    _saveState();
                    emit paused(error, completedVideoCount(), totalVideoCount());
                    co_return;
                }
                QFile::remove(generatedVideoPath);
            }

            take.completed = true;
            take.filePath = finalVideoPath;
            take.failed = false;
            take.errorMessage.clear();

            _saveState();

            emit progress(tr("[%1] Video generated successfully: %2")
                .arg(stepTag, take.expectedFileName));
            emit takeCompleted(take.stepIndex, take.takeIndex, finalVideoPath);

            // Check if all takes for this step are finished
            bool allStepTakesDone = true;
            for (const auto &otherTake : m_takes)
            {
                if (otherTake.stepIndex == take.stepIndex && !otherTake.completed)
                {
                    allStepTakesDone = false;
                    break;
                }
            }
            if (allStepTakesDone)
            {
                emit stepCompleted(take.stepIndex);

                // Only pause mid-generation for Step 1 (intro) if multiple takes were generated,
                // because picking the intro take defines which Gemini conversation to continue in.
                // For all subsequent steps, generation continues unattended without pausing;
                // takes for remaining steps are reviewed and selected at the end.
                if (take.stepIndex == 0 && step.videoCount > 1 && selectedTakeIndex(0) < 0)
                {
                    QList<TakeInfo> stepTakes;
                    for (const auto &t : m_takes)
                    {
                        if (t.stepIndex == 0)
                        {
                            stepTakes << t;
                        }
                    }
                    const QString pauseMsg = tr("Please select the intro video to continue with subsequent steps.");
                    setState(State::Paused, pauseMsg);
                    _saveState();
                    emit introSelectionRequested(stepTakes);
                    emit progress(pauseMsg);
                    emit paused(pauseMsg, completedVideoCount(), totalVideoCount());
                    co_return;
                }
            }
        }
        else
        {
            // Take failed or blocked due to quota / limit!
            take.failed = true;
            take.errorMessage = workflowError;

            const int completed = completedVideoCount();
            const int total = totalVideoCount();
            const QString pauseMsg = workflowError.isEmpty()
                ? tr("Video generation failed or was stopped.")
                : workflowError;

            setState(State::Paused, pauseMsg);
            _saveState();

            emit progress(tr("Generation paused (%1/%2 videos generated): %3")
                .arg(completed).arg(total).arg(pauseMsg));
            emit paused(pauseMsg, completed, total);
            co_return; // Pause execution here so user can resume later
        }
    }

    // All takes completed!
    setState(State::Completed);
    _saveState();

    QStringList allGenerated;
    for (const auto &take : m_takes)
    {
        if (take.completed && !take.filePath.isEmpty())
        {
            allGenerated << take.filePath;
        }
    }

    emit progress(tr("All %1 multi-video parts successfully generated!").arg(allGenerated.size()));
    emit allCompleted(allGenerated);
    updateMergedVideo();
}

QJsonObject ProductShowcaseMultiVideoWorkflow::toJson() const
{
    auto json = AbstractMultiVideoWorkflow::toJson();
    json[QStringLiteral("mergedVideoPath")] = m_mergedVideoPath;
    json[QStringLiteral("mergedSources")] = QJsonArray::fromStringList(m_mergedSources);
    json[QStringLiteral("generationShortCode")] = m_generationShortCode;
    return json;
}

bool ProductShowcaseMultiVideoWorkflow::fromJson(const QJsonObject &json)
{
    if (!AbstractMultiVideoWorkflow::fromJson(json)) return false;
    m_mergedVideoPath = json.value(QStringLiteral("mergedVideoPath")).toString();
    m_mergedSources.clear();
    for (const auto &source : json.value(QStringLiteral("mergedSources")).toArray())
    {
        m_mergedSources << source.toString();
    }
    m_generationShortCode = json.value(QStringLiteral("generationShortCode")).toString();
    if (m_generationShortCode.isEmpty())
    {
        // Older manifests did not store an identity. Recover the folder
        // containing the workflow's current takes, rather than allocating
        // another generation record on resume.
        QMap<QString, int> counts;
        int bestCount = 0;
        for (const auto &take : m_takes)
        {
            if (take.filePath.isEmpty()) continue;
            QDir dir = QFileInfo(take.filePath).dir();
            if (dir.dirName() == QLatin1String("takes")) dir.cdUp();
            const QString code = dir.dirName();
            QDir parent = dir;
            parent.cdUp();
            if (code.startsWith(QLatin1String("QS-")) && parent.absolutePath() == QDir(m_outputDir).absolutePath())
            {
                const int count = ++counts[code];
                if (count > bestCount)
                {
                    bestCount = count;
                    m_generationShortCode = code;
                }
            }
        }
    }
    return true;
}

bool ProductShowcaseMultiVideoWorkflow::prepareGeneration(const QString &shortCode, QString *error)
{
    if (m_merging) return true;
    const QDir generationDir{QDir(m_outputDir).absoluteFilePath(shortCode)};
    const QDir takesDir{generationDir.absoluteFilePath(QStringLiteral("takes"))};
    if (shortCode.isEmpty() || !takesDir.mkpath(QStringLiteral(".")))
    {
        *error = tr("Could not create the generation folder.");
        return false;
    }
    m_generationShortCode = shortCode;
    if (!_organizeWorkingFiles(error))
    {
        _saveState();
        return false;
    }
    for (auto &take : m_takes)
    {
        if (!take.completed) continue;
        const QString target = takesDir.absoluteFilePath(take.expectedFileName);
        if (take.filePath == target) continue;
        if (!QFile::rename(take.filePath, target))
        {
            // Do not replace an existing candidate or discard a download
            // when a move fails (for example across file systems).
            if (!QFile::copy(take.filePath, target))
            {
                *error = tr("Could not move %1 to %2.").arg(take.filePath, target);
                _saveState();
                return false;
            }
            QFile::remove(take.filePath);
        }
        take.filePath = target;
    }
    _saveState();
    // Once finalized, the project metadata and generation folder are the
    // authoritative copies. Do not leave a third manifest beside the result.
    const QString manifestName = QStringLiteral("multi_video_workflow.json");
    if (!m_projectDir.isEmpty()
        && saveToFile(QDir(m_projectDir).absoluteFilePath(manifestName))
        && saveToFile(generationDir.absoluteFilePath(manifestName)))
    {
        QFile::remove(QDir(m_outputDir).absoluteFilePath(manifestName));
    }
    return true;
}

QDir ProductShowcaseMultiVideoWorkflow::_workingDirectory() const
{
    return QDir(QDir(m_outputDir).absoluteFilePath(m_generationShortCode.isEmpty()
        ? QStringLiteral(".work") : m_generationShortCode + QStringLiteral("/work")));
}

bool ProductShowcaseMultiVideoWorkflow::_organizeWorkingFiles(QString *error)
{
    const QDir destination = _workingDirectory();
    if (!destination.mkpath(QStringLiteral(".")))
    {
        *error = tr("Could not create the working folder %1.").arg(destination.absolutePath());
        return false;
    }
    QStringList names;
    for (int step = 0; step < m_steps.size(); ++step)
    {
        names << QStringLiteral("step_%1_inputs").arg(step + 1);
    }
    for (const auto &take : m_takes)
    {
        names << QStringLiteral("working_step%1_take%2").arg(take.stepIndex + 1).arg(take.takeIndex + 1);
    }
    const QStringList oldRoots{m_outputDir, QDir(m_outputDir).absoluteFilePath(QStringLiteral(".work"))};
    for (const auto &root : oldRoots)
    {
        if (QDir(root).absolutePath() == destination.absolutePath()) continue;
        for (const auto &name : names)
        {
            const QString source = QDir(root).absoluteFilePath(name);
            if (!QDir(source).exists()) continue;
            const QString target = destination.absoluteFilePath(name);
            if (QFileInfo::exists(target) || !QDir().rename(source, target))
            {
                *error = tr("Could not move the working folder %1 to %2.").arg(source, target);
                return false;
            }
            const auto relocate = [&source, &target](QString &path) {
                if (path.startsWith(source + QLatin1Char('/')))
                {
                    path = target + path.mid(source.size());
                }
            };
            for (auto &step : m_steps)
            {
                for (auto &image : step.images)
                {
                    relocate(image.originalPath);
                    relocate(image.processedPath);
                }
            }
            _saveState();
        }
    }
    if (!m_generationShortCode.isEmpty())
    {
        QDir(m_outputDir).rmdir(QStringLiteral(".work"));
    }
    return true;
}

QStringList ProductShowcaseMultiVideoWorkflow::_selectedVideoPaths() const
{
    if (m_steps.isEmpty() || completedVideoCount() != totalVideoCount()) return {};
    QStringList paths;
    for (int step = 0; step < m_steps.size(); ++step)
    {
        const TakeInfo *chosen = nullptr;
        for (const auto &take : m_takes)
        {
            if (take.stepIndex == step && take.completed
                && (take.takeIndex == selectedTakeIndex(step) || m_steps[step].videoCount == 1))
            {
                chosen = &take;
                break;
            }
        }
        if (!chosen || !QFileInfo::exists(chosen->filePath)) return {};
        paths << chosen->filePath;
    }
    return paths;
}

void ProductShowcaseMultiVideoWorkflow::updateMergedVideo()
{
    ++m_mergeRevision;
    _saveState();
    const auto paths = _selectedVideoPaths();
    if (paths.isEmpty())
    {
        if (!m_mergedVideoPath.isEmpty()) QFile::remove(m_mergedVideoPath);
        m_mergedVideoPath.clear();
        m_mergedSources.clear();
        _saveState();
        return;
    }
    if (m_merging || (paths == m_mergedSources && QFileInfo::exists(m_mergedVideoPath))) return;
    m_merging = true;
    // Queue this work so selection dialogs and generation finalization finish
    // before capturing paths; rapid selection changes share one merge loop.
    QTimer::singleShot(0, this, [this] { m_mergeTask = _mergeSelectedVideos(); });
}

QCoro::Task<void> ProductShowcaseMultiVideoWorkflow::_mergeSelectedVideos()
{
    QString error;
    while (true)
    {
        const int revision = m_mergeRevision;
        const QStringList sources = _selectedVideoPaths();
        if (sources.isEmpty()) break;
        const QDir generationDir = m_generationShortCode.isEmpty()
            ? QFileInfo(sources.first()).dir()
            : QDir(QDir(m_outputDir).absoluteFilePath(m_generationShortCode));
        const QString destination = generationDir.absoluteFilePath(QStringLiteral("final_video.mp4"));
        emit progress(tr("Merging the selected clips into the final video..."));
        std::function<bool()> stillCurrent = [this, revision] { return revision == m_mergeRevision; };
        const auto result = co_await VideoClipProcessing::merge(sources, destination, stillCurrent);
        if (revision != m_mergeRevision) continue;
        error = result.error;
        if (error.isEmpty())
        {
            m_mergedVideoPath = result.path;
            m_mergedSources = sources;
            _saveState();
            emit progress(tr("Final video ready: %1").arg(result.path));
        }
        else
        {
            emit progress(tr("Could not merge the selected clips: %1").arg(error));
        }
        break;
    }
    m_merging = false;
    emit mergedVideoUpdated(m_mergedVideoPath, error);
}

QCoro::Task<ProductShowcaseMultiVideoWorkflow::ImagePrepStatus> ProductShowcaseMultiVideoWorkflow::_ensureStepImagesReady(
    Step &step, const QString &stepWorkingDir)
{
    if (!m_imageCli)
    {
        for (const auto &img : step.images)
        {
            if (img.regenOption != ImageRegenOption::KeepOriginal)
            {
                emit progress(tr("Warning: no image CLI configured to isolate/regenerate image."));
                co_return ImagePrepStatus::Failed;
            }
        }
        for (auto &img : step.images)
        {
            img.processedPath = img.originalPath;
        }
        co_return ImagePrepStatus::Success;
    }

    bool allOk = true;
    for (int i = 0; i < step.images.size(); ++i)
    {
        auto &img = step.images[i];
        if (img.originalPath.isEmpty() || !QFileInfo::exists(img.originalPath))
        {
            continue;
        }

        if (img.regenOption == ImageRegenOption::KeepOriginal)
        {
            img.processedPath = img.originalPath;
            continue;
        }

        const QString outputFileName = QStringLiteral("step_image_%1_processed.png").arg(i + 1);
        const QString targetPath = QDir{stepWorkingDir}.absoluteFilePath(outputFileName);

        // Check if output file is already on disk (e.g. from previous run or take)
        if (QFileInfo::exists(targetPath) && QFileInfo{targetPath}.size() > 0)
        {
            img.processedPath = targetPath;
            continue;
        }

        if (!img.processedPath.isEmpty() && img.processedPath != img.originalPath && QFileInfo::exists(img.processedPath))
        {
            continue; // already regenerated
        }

        const QString actionDesc = (img.regenOption == ImageRegenOption::IsolateOnWhite)
            ? tr("isolating on white background")
            : tr("regenerating");

        emit progress(tr("Step image %1: %2 with %3...")
            .arg(i + 1).arg(actionDesc, m_imageCli->getName()));

        const ImageRegenResult regenResult = co_await _regenerateImage(
            img.originalPath, img.regenOption, stepWorkingDir, i + 1);

        if (regenResult.quotaExceeded)
        {
            emit progress(tr("Quota reached on %1: %2").arg(m_imageCli->getName(), regenResult.errorMessage));
            img.processedPath.clear();
            _saveState();
            co_return ImagePrepStatus::QuotaExceeded;
        }

        if (!regenResult.filePath.isEmpty() && QFileInfo::exists(regenResult.filePath))
        {
            img.processedPath = regenResult.filePath;
            emit progress(tr("Step image %1 %2 successfully (%3).")
                .arg(i + 1).arg(actionDesc, QFileInfo{regenResult.filePath}.fileName()));
        }
        else
        {
            emit progress(tr("Error: Failed to %1 step image %2 with %3.")
                .arg(actionDesc).arg(i + 1).arg(m_imageCli->getName()));
            img.processedPath.clear();
            allOk = false;
        }
    }

    _saveState();
    co_return allOk ? ImagePrepStatus::Success : ImagePrepStatus::Failed;
}

QCoro::Task<ProductShowcaseMultiVideoWorkflow::ImageRegenResult> ProductShowcaseMultiVideoWorkflow::_regenerateImage(
    const QString &originalImage, AbstractMultiVideoWorkflow::ImageRegenOption option,
    const QString &outputDir, int imgIndex)
{
    if (!m_imageCli || originalImage.isEmpty() || !QFileInfo::exists(originalImage))
    {
        co_return ImageRegenResult{};
    }

    const QString outputFileName = QStringLiteral("step_image_%1_processed.png").arg(imgIndex);
    const QString targetPath = QDir{outputDir}.absoluteFilePath(outputFileName);

    if (QFileInfo::exists(targetPath) && QFileInfo{targetPath}.size() > 0)
    {
        co_return ImageRegenResult{targetPath, false, QString{}};
    }

    // Copy original image into working folder for the CLI
    const QString localInputName = QStringLiteral("input_image_%1.%2")
        .arg(imgIndex).arg(QFileInfo{originalImage}.suffix());
    const QString localInputPath = QDir{outputDir}.absoluteFilePath(localInputName);
    QFile::remove(localInputPath);
    QFile::copy(originalImage, localInputPath);

    const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
    const bool isolateWhite = (option == ImageRegenOption::IsolateOnWhite);
    QString promptText;
    if (isolateWhite)
    {
        promptText = marker + QStringLiteral(
            "You are given the image file '%1' located at absolute path: '%2'.\n"
            "Extract ONLY the product itself (e.g. the shoes/accessory — not any "
            "model, body part or person) and generate a clean e-commerce-style "
            "product shot:\n"
            "- the product alone, isolated, no model/human, no body parts\n"
            "- plain pure white background, studio product-photography lighting\n"
            "- keep the product's exact real design — same silhouette, heel "
            "shape/height, straps/closures, material, color and hardware/"
            "decorative details as in the source photo\n"
            "- sharp, high quality, no watermarks/text/UI overlays\n"
            "Save the result in the current working directory as '%3' (absolute path: '%4').\n"
            "Do not modify the original file. Reply with just the output file name.")
            .arg(localInputName, localInputPath, outputFileName, targetPath);
    }
    else
    {
        promptText = marker + QStringLiteral(
            "You are given the image file '%1' located at absolute path: '%2'.\n"
            "Create a cleaned-up, high-quality version of it:\n"
            "- remove any social network icons, UI overlays, watermarks, captions and text\n"
            "- improve sharpness and overall quality\n"
            "- keep the subject, framing, colors and style otherwise identical\n"
            "Save the result in the current working directory as '%3' (absolute path: '%4').\n"
            "Do not modify the original file. Reply with just the output file name.")
            .arg(localInputName, localInputPath, outputFileName, targetPath);
    }

    const QDateTime callStart = QDateTime::currentDateTime();
    CliRunResult result = co_await m_imageCli->runPrompt(promptText, outputDir);

    const bool quotaExceeded =
        m_imageCli->classifyError(result.errorOutput) == CliErrorKind::QuotaExceeded
        || m_imageCli->classifyError(result.output) == CliErrorKind::QuotaExceeded
        || result.errorOutput.contains(QStringLiteral("quota"), Qt::CaseInsensitive)
        || result.errorOutput.contains(QStringLiteral("RESOURCE_EXHAUSTED"), Qt::CaseInsensitive)
        || result.output.contains(QStringLiteral("quota"), Qt::CaseInsensitive)
        || result.output.contains(QStringLiteral("RESOURCE_EXHAUSTED"), Qt::CaseInsensitive);

    if (quotaExceeded)
    {
        co_return ImageRegenResult{QString{}, true, tr("Quota exceeded for %1").arg(m_imageCli->getName())};
    }

    bool produced = result.processStarted && result.exitCode == 0
        && QFileInfo::exists(targetPath) && QFileInfo{targetPath}.size() > 0;

    // Check CLI fallback dirs with timestamp guard (same as ImageGeneratorCli & PaneGeneration)
    if (!produced && result.processStarted && result.exitCode == 0)
    {
        for (const QString &fallbackDir : m_imageCli->outputFallbackDirs())
        {
            const QString fallbackPath = QDir{fallbackDir}.absoluteFilePath(outputFileName);
            const QFileInfo fallbackInfo{fallbackPath};
            if (!fallbackInfo.exists() || fallbackInfo.lastModified() < callStart)
            {
                continue;
            }
            QFile::remove(targetPath);
            if (QFile::rename(fallbackPath, targetPath)
                || (QFile::copy(fallbackPath, targetPath) && QFile::remove(fallbackPath)))
            {
                produced = true;
                break;
            }
        }
    }

    if (produced)
    {
        co_return ImageRegenResult{targetPath, false, QString{}};
    }

    co_return ImageRegenResult{QString{}, false, result.errorOutput};
}
