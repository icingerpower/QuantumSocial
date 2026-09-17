#include "VideoGeneratorGemini.h"

#include <QProcess>
#include <QStandardPaths>

QString VideoGeneratorGemini::getId() const
{
    return QStringLiteral("gemini-browser");
}

QString VideoGeneratorGemini::getName() const
{
    return QStringLiteral("Gemini (browser)");
}

QList<AbstractVideoGenerator::SettingSpec> VideoGeneratorGemini::availableSettings() const
{
    return {
        {QStringLiteral("maxTriesPerPrompt"),
         QObject::tr("Tries with small prompt variations before the CLI rewrites "
                     "the prompt or changes the source image"),
         10},
        {QStringLiteral("generationTimeoutSec"),
         QObject::tr("Maximum seconds to wait for one Veo generation"),
         900},
        {QStringLiteral("inPageRejectionRetries"),
         QObject::tr("After a refusal, quick retries in the same browser with "
                     "a tiny prompt edit (add/remove a dot) before giving up"),
         3},
        {QStringLiteral("attemptStallTimeoutSec"),
         QObject::tr("Seconds without any reply before one attempt is "
                     "considered silently refused and retried"),
         300},
        {QStringLiteral("useSystemChromeProfile"),
         QObject::tr("Use the system Google Chrome with its default profile — no "
                     "login needed, but Chrome must be closed; falls back to a "
                     "dedicated profile when unavailable"),
         true},
        {QStringLiteral("requireUltra"),
         QObject::tr("Before generating, make sure a Google account with the AI "
                     "Ultra plan is selected (switches accounts and remembers "
                     "the right one)"),
         true},
    };
}

QCoro::Task<AbstractVideoGenerator::Result> VideoGeneratorGemini::generate(
    const QString &prompt, const QStringList &imagePaths,
    const QString &outputDir, const QVariantMap &settings) const
{
    // Check the local Python setup before creating the long-lived worker.
    // A missing module makes that worker answer once and exit immediately;
    // repeatedly recreating it cannot help and used to exercise a fragile
    // fast-exit path in the QProcess coroutine handling.
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        Result result;
        result.errorMessage = QObject::tr("python3 was not found on PATH.");
        result.retryable = false;
        co_return result;
    }

    QProcess dependencyCheck;
    dependencyCheck.start(python,
        {QStringLiteral("-c"), QStringLiteral("import playwright")});
    if (!dependencyCheck.waitForStarted(5000)
        || !dependencyCheck.waitForFinished(10000)
        || dependencyCheck.exitStatus() != QProcess::NormalExit
        || dependencyCheck.exitCode() != 0)
    {
        Result result;
        result.errorMessage = QObject::tr(
            "Python Playwright is not installed for %1. Install it with "
            "'python3 -m pip install playwright', then retry.").arg(python);
        result.retryable = false;
        co_return result;
    }

    co_return co_await runGeneratorScript(QStringLiteral("generate_video_gemini.py"),
                                           prompt, imagePaths, outputDir, settings);
}

DECLARE_VIDEO_GENERATOR(VideoGeneratorGemini)
