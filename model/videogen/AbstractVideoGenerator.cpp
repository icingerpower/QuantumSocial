// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "AbstractVideoGenerator.h"
#include "VideoGenerationRecipe.h"

#include <chrono>

#include <QCoro/QCoroTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#ifndef VIDEO_GEN_SCRIPT_DIR
#define VIDEO_GEN_SCRIPT_DIR ""
#endif

AbstractVideoGenerator::AbstractVideoGenerator() = default;

AbstractVideoGenerator::~AbstractVideoGenerator()
{
    stopWorker();
}

bool AbstractVideoGenerator::isWorkerRunning() const
{
    return m_worker && m_worker->state() != QProcess::NotRunning;
}

void AbstractVideoGenerator::stopWorker()
{
    _stopWorkerInternal();
}

void AbstractVideoGenerator::_stopWorkerInternal() const
{
    if (!m_worker)
    {
        return;
    }
    if (m_worker->state() != QProcess::NotRunning)
    {
        // Closing stdin is the worker's shutdown signal (its read loop ends and
        // it closes the browser context and Playwright itself); kill only if it ignores that.
        m_worker->closeWriteChannel();
        if (!m_worker->waitForFinished(5000))
        {
            m_worker->kill();
            m_worker->waitForFinished(2000);
        }
    }
    m_worker.reset();
}

void AbstractVideoGenerator::stopAllWorkers()
{
    for (AbstractVideoGenerator *generator : ALL_VIDEO_GENERATORS())
    {
        if (generator)
        {
            generator->stopWorker();
        }
    }
}

AbstractVideoGenerator::Recorder::Recorder(AbstractVideoGenerator *generator)
    : m_id(generator ? generator->getId() : QString{})
    , m_generator(generator)
{
    if (m_generator)
    {
        getGenerators().insert(m_id, m_generator);
    }
}

AbstractVideoGenerator::Recorder::~Recorder()
{
    if (!m_id.isEmpty() && getGenerators().value(m_id) == m_generator)
    {
        getGenerators().remove(m_id);
    }
}

const QMap<QString, AbstractVideoGenerator *> &AbstractVideoGenerator::ALL_VIDEO_GENERATORS()
{
    return getGenerators();
}

QMap<QString, AbstractVideoGenerator *> &AbstractVideoGenerator::getGenerators()
{
    static QMap<QString, AbstractVideoGenerator *> map;
    return map;
}

QList<AbstractVideoGenerator::SettingSpec> AbstractVideoGenerator::availableSettings() const
{
    return {};
}

double AbstractVideoGenerator::continuationDurationSeconds(const QVariantMap &) const
{
    return 0.0;
}

QCoro::Task<void> AbstractVideoGenerator::deleteConversation(
    const QString &conversationUrl, const QVariantMap &settings) const
{
    Q_UNUSED(conversationUrl);
    Q_UNUSED(settings);
    co_return;
}

QCoro::Task<AbstractVideoGenerator::DeleteConversationsResult>
AbstractVideoGenerator::deleteConversations(
    const QStringList &conversationUrls, const QVariantMap &settings) const
{
    Q_UNUSED(settings);
    DeleteConversationsResult res;
    res.failedUrls = conversationUrls;
    if (!conversationUrls.isEmpty())
    {
        res.errorMessage = QObject::tr("Conversation cleanup is not supported by %1.").arg(getName());
    }
    co_return res;
}

QCoro::Task<void> AbstractVideoGenerator::runDeleteConversationInWorker(
    const QString &conversationUrl, const QVariantMap &settings) const
{
    const QString trimmedUrl = conversationUrl.trimmed();
    const QVariantMap settingsCopy = settings;
    if (trimmedUrl.isEmpty() || !isWorkerRunning())
    {
        co_return;
    }
    QJsonObject request;
    request.insert(QStringLiteral("deleteConversation"), true);
    request.insert(QStringLiteral("conversationUrl"), trimmedUrl);
    request.insert(QStringLiteral("settings"), QJsonObject::fromVariantMap(settingsCopy));
    m_worker->write(QJsonDocument{request}.toJson(QJsonDocument::Compact) + "\n");

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 20000)
    {
        if (m_worker->canReadLine())
        {
            const QByteArray line = m_worker->readLine().trimmed();
            if (line.isEmpty())
            {
                continue;
            }
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject())
            {
                break;
            }
            continue;
        }
        if (m_worker->state() == QProcess::NotRunning)
        {
            break;
        }
        co_await QCoro::sleepFor(std::chrono::milliseconds(50));
    }
}

QCoro::Task<AbstractVideoGenerator::DeleteConversationsResult>
AbstractVideoGenerator::runDeleteConversationsInWorker(
    const QString &scriptFileName,
    const QStringList &conversationUrls,
    const QVariantMap &settings) const
{
    const QString scriptNameCopy = scriptFileName;
    const QVariantMap settingsCopy = settings;
    DeleteConversationsResult result;
    QStringList cleanedUrls;
    for (const QString &raw : conversationUrls)
    {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty() && !cleanedUrls.contains(trimmed))
        {
            cleanedUrls.append(trimmed);
        }
    }
    if (cleanedUrls.isEmpty())
    {
        co_return result;
    }

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        result.failedUrls = cleanedUrls;
        result.errorMessage = QObject::tr("python3 was not found on PATH.");
        co_return result;
    }

    const QString scriptPath = QDir(QString::fromUtf8(VIDEO_GEN_SCRIPT_DIR))
        .filePath(scriptNameCopy);
    if (!QFileInfo::exists(scriptPath))
    {
        result.failedUrls = cleanedUrls;
        result.errorMessage = QObject::tr("Video generator script not found at %1 "
            "(stale build? re-run cmake).").arg(scriptPath);
        co_return result;
    }

    const bool wasRunning = isWorkerRunning();
    if (!wasRunning)
    {
        m_worker = std::make_unique<QProcess>();
        m_worker->setProcessChannelMode(QProcess::SeparateChannels);
        const QStringList workerArgs{scriptPath, QStringLiteral("--worker")};
        m_worker->start(python, workerArgs);
        if (!m_worker->waitForStarted(5000))
        {
            result.failedUrls = cleanedUrls;
            result.errorMessage = QObject::tr("Could not start the video generator (%1).")
                .arg(m_worker->errorString());
            m_worker.reset();
            co_return result;
        }
    }

    QJsonObject request;
    request.insert(QStringLiteral("deleteConversations"), true);
    request.insert(QStringLiteral("conversationUrls"), QJsonArray::fromStringList(cleanedUrls));
    request.insert(QStringLiteral("settings"), QJsonObject::fromVariantMap(settingsCopy));
    m_worker->write(QJsonDocument{request}.toJson(QJsonDocument::Compact) + "\n");

    const qint64 perChatBudgetMs = static_cast<qint64>(cleanedUrls.size()) * 25000 + 30000;
    const qint64 timeoutMs = wasRunning ? qMax<qint64>(60000, perChatBudgetMs)
                                        : qMax<qint64>(360000, perChatBudgetMs);
    QElapsedTimer timer;
    timer.start();
    QJsonDocument doc;
    while (timer.elapsed() < timeoutMs)
    {
        if (m_worker->canReadLine())
        {
            const QByteArray line = m_worker->readLine().trimmed();
            if (line.isEmpty())
            {
                continue;
            }
            QJsonParseError parseError;
            doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject())
            {
                break;
            }
            doc = QJsonDocument{};
            continue;
        }
        if (m_worker->state() == QProcess::NotRunning)
        {
            break;
        }
        co_await QCoro::sleepFor(std::chrono::milliseconds(80));
    }

    if (doc.isNull() || !doc.isObject())
    {
        const QByteArray stderrOutput = m_worker->readAllStandardError().trimmed();
        result.failedUrls = cleanedUrls;
        result.errorMessage = m_worker->state() == QProcess::NotRunning
            ? QObject::tr("The video generator worker stopped unexpectedly%1")
                .arg(stderrOutput.isEmpty() ? QString{}
                    : QObject::tr(": %1").arg(QString::fromUtf8(stderrOutput).left(300)))
            : QObject::tr("Timed out while deleting Gemini chats.");
        m_worker->kill();
        m_worker->waitForFinished(2000);
        m_worker.reset();
        co_return result;
    }

    const QJsonObject obj = doc.object();
    for (const QJsonValue &val : obj.value(QStringLiteral("deleted")).toArray())
    {
        const QString url = val.toString().trimmed();
        if (!url.isEmpty())
        {
            result.deletedUrls.append(url);
        }
    }
    for (const QJsonValue &val : obj.value(QStringLiteral("failed")).toArray())
    {
        const QString url = val.toString().trimmed();
        if (!url.isEmpty())
        {
            result.failedUrls.append(url);
        }
    }
    result.errorMessage = obj.value(QStringLiteral("error")).toString().trimmed();

    if (!wasRunning && settingsCopy.value(QStringLiteral("closeBrowserOnFinish"), true).toBool())
    {
        _stopWorkerInternal();
    }
    co_return result;
}

QCoro::Task<AbstractVideoGenerator::Result> AbstractVideoGenerator::runGeneratorScript(
    const QString &scriptFileName, const QString &prompt,
    const QStringList &imagePaths, const QString &outputDir,
    const QVariantMap &settings) const
{
    Result result;

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        result.errorMessage = QObject::tr("python3 was not found on PATH.");
        co_return result;
    }

    const QString scriptPath = QDir(QString::fromUtf8(VIDEO_GEN_SCRIPT_DIR))
        .filePath(scriptFileName);
    if (!QFileInfo::exists(scriptPath))
    {
        result.errorMessage = QObject::tr("Video generator script not found at %1 "
            "(stale build? re-run cmake).").arg(scriptPath);
        co_return result;
    }

    const QString promptPath = QDir(outputDir).absoluteFilePath(
        QStringLiteral("generation_prompt.txt"));
    QFile promptFile{promptPath};
    if (!promptFile.open(QFile::WriteOnly))
    {
        result.errorMessage = QObject::tr("Could not write the prompt file %1.")
            .arg(promptPath);
        co_return result;
    }
    promptFile.write(prompt.toUtf8());
    promptFile.close();

    const VideoGenerationRecipe recipe{getId(), prompt, imagePaths, settings};
    if (!recipe.save(QDir(outputDir), &result.errorMessage))
    {
        result.retryable = false;
        co_return result;
    }

    // Start the worker on first use, or restart it if a previous one died
    // (crash, or a browser loss it could not recover from).
    if (!m_worker || m_worker->state() == QProcess::NotRunning)
    {
        m_worker = std::make_unique<QProcess>();
        // stderr kept OUT of stdout: the protocol is one JSON line per
        // request on stdout, and Python warnings on stderr must not corrupt it.
        m_worker->setProcessChannelMode(QProcess::SeparateChannels);
        const QStringList workerArgs{scriptPath, QStringLiteral("--worker")};
        m_worker->start(python, workerArgs);
        if (!m_worker->waitForStarted(5000))
        {
            result.errorMessage = QObject::tr("Could not start the video generator (%1).")
                .arg(m_worker->errorString());
            m_worker.reset();
            co_return result;
        }
    }

    QJsonObject request;
    request.insert(QStringLiteral("promptFile"), promptPath);
    request.insert(QStringLiteral("outputDir"), outputDir);
    request.insert(QStringLiteral("settings"), QJsonObject::fromVariantMap(settings));
    request.insert(QStringLiteral("images"), QJsonArray::fromStringList(imagePaths));
    if (settings.contains(QStringLiteral("chatMode")))
    {
        request.insert(QStringLiteral("chatMode"), settings.value(QStringLiteral("chatMode")).toString());
    }
    if (settings.contains(QStringLiteral("messageAction")))
    {
        request.insert(QStringLiteral("messageAction"), settings.value(QStringLiteral("messageAction")).toString());
    }
    if (settings.contains(QStringLiteral("conversationUrl")))
    {
        request.insert(QStringLiteral("conversationUrl"), settings.value(QStringLiteral("conversationUrl")).toString());
    }
    m_worker->write(QJsonDocument{request}.toJson(QJsonDocument::Compact) + "\n");

    // Video generation is slow, and browser-driven backends may additionally
    // wait for an interactive login on first run: the reply timeout is the
    // backend's own generation timeout plus a generous margin.
    const int timeoutMs = (settings.value(QStringLiteral("generationTimeoutSec"), 900)
        .toInt() + 600) * 1000;
    QElapsedTimer timer;
    timer.start();
    QByteArray line;
    QJsonDocument doc;
    while (timer.elapsed() < timeoutMs)
    {
        if (m_worker->canReadLine())
        {
            line = m_worker->readLine().trimmed();
            if (line.isEmpty())
            {
                continue;
            }
            QJsonParseError parseError;
            doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject())
            {
                break;
            }
            // Stray non-JSON output (a Python warning that reached stdout,
            // say) must not abort the request — skip it and keep reading.
            doc = QJsonDocument{};
            continue;
        }
        if (m_worker->state() == QProcess::NotRunning)
        {
            break;
        }
        // QCoroProcess's ready-read awaiter also connects to QProcess::finished.
        // If the worker exits while that signal is being delivered, resuming
        // this coroutine can destroy/reset the process from inside its own
        // finished callback. Polling with a timer avoids that reentrant
        // lifetime hazard while keeping the UI event loop responsive.
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }

    if (doc.isNull() || !doc.isObject())
    {
        const QByteArray stderrOutput = m_worker->readAllStandardError().trimmed();
        result.browserLost = m_worker->state() == QProcess::NotRunning;
        result.errorMessage = m_worker->state() == QProcess::NotRunning
            ? QObject::tr("The video generator worker stopped unexpectedly%1")
                .arg(stderrOutput.isEmpty() ? QString{}
                    : QObject::tr(": %1").arg(QString::fromUtf8(stderrOutput).left(300)))
            : QObject::tr("The video generator timed out.");
        // Whatever went wrong, this worker is not trustworthy anymore — drop
        // it so the next call starts a clean one.
        m_worker->kill();
        m_worker->waitForFinished(2000);
        m_worker.reset();
        co_return result;
    }

    const QJsonObject obj = doc.object();
    result.videoPath = obj.value(QStringLiteral("video")).toString();
    result.errorMessage = obj.value(QStringLiteral("error")).toString();
    result.rejected = obj.value(QStringLiteral("rejected")).toBool();
    result.retryable = obj.value(QStringLiteral("retryable")).toBool(true);
    result.browserLost = obj.value(QStringLiteral("browserLost")).toBool();
    result.conversationUrl = obj.value(QStringLiteral("conversationUrl")).toString();
    result.promptSubmitted = obj.value(QStringLiteral("promptSubmitted")).toBool();
    result.quotaExceeded = obj.value(QStringLiteral("quotaExceeded")).toBool();
    if (!result.videoPath.isEmpty() && !QFileInfo::exists(result.videoPath))
    {
        result.errorMessage = QObject::tr("The video generator reported %1 "
            "but the file does not exist.").arg(result.videoPath);
        result.videoPath.clear();
    }
    co_return result;
}
