// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 (internal
// compiler error / bad codegen) — force -O1 for this translation unit, same
// workaround used elsewhere in sibling projects for QCoro-based code.
#pragma GCC optimize("O1")

#include "AbstractSocialAccount.h"

#include <QCoro/QCoroProcess>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#ifndef SOCIAL_STATS_SCRIPT_DIR
#define SOCIAL_STATS_SCRIPT_DIR ""
#endif

AbstractSocialAccount::Recorder::Recorder(AbstractSocialAccount *account)
{
    getSocialAccounts().insert(account->getId(), account);
}

const QMap<QString, AbstractSocialAccount *> &AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS()
{
    return getSocialAccounts();
}

QMap<QString, AbstractSocialAccount *> &AbstractSocialAccount::getSocialAccounts()
{
    static QMap<QString, AbstractSocialAccount *> map;
    return map;
}

QCoro::Task<AbstractSocialAccount::Statistics> AbstractSocialAccount::runStatsScript(
    const QString &scriptFileName, const QString &url) const
{
    Statistics stats;

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        stats.errorMessage = QObject::tr("python3 was not found on PATH.");
        co_return stats;
    }

    const QString scriptPath = QDir(QString::fromUtf8(SOCIAL_STATS_SCRIPT_DIR))
        .filePath(scriptFileName);
    if (!QFileInfo::exists(scriptPath))
    {
        stats.errorMessage = QObject::tr("Statistics fetcher script not found at %1 "
            "(stale build? re-run cmake).").arg(scriptPath);
        co_return stats;
    }

    QProcess process;
    process.start(python, {scriptPath, url});

    if (!co_await qCoro(process).waitForStarted())
    {
        stats.errorMessage = QObject::tr("Could not start the statistics fetcher (%1).")
            .arg(process.errorString());
        co_return stats;
    }

    // Generous timeout: most scripts finish in a few seconds, but some (e.g.
    // Pinterest) may open a visible browser and wait several minutes for the
    // user to log in interactively before they can proceed.
    if (!co_await qCoro(process).waitForFinished(360'000))
    {
        stats.errorMessage = QObject::tr("The statistics fetcher timed out.");
        process.kill();
        co_return stats;
    }

    const QByteArray output = process.readAllStandardOutput().trimmed();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        const QByteArray stderrOutput = process.readAllStandardError().trimmed();
        stats.errorMessage = output.isEmpty() && !stderrOutput.isEmpty()
            ? QObject::tr("The statistics fetcher failed: %1")
                .arg(QString::fromUtf8(stderrOutput).left(300))
            : QObject::tr("Unexpected output from the statistics fetcher: %1")
                .arg(QString::fromUtf8(output).left(200));
        co_return stats;
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("followers")).isDouble())
    {
        stats.followers = static_cast<qint64>(obj.value(QStringLiteral("followers")).toDouble());
    }
    if (obj.value(QStringLiteral("views")).isDouble())
    {
        stats.views = static_cast<qint64>(obj.value(QStringLiteral("views")).toDouble());
    }
    if (obj.value(QStringLiteral("likes")).isDouble())
    {
        stats.likes = static_cast<qint64>(obj.value(QStringLiteral("likes")).toDouble());
    }
    stats.errorMessage = obj.value(QStringLiteral("error")).toString();

    co_return stats;
}
