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

namespace {

// Reads one stat from the script's JSON reply; anything non-numeric
// (null, absent) keeps the "unavailable" default of -1.
qint64 jsonStat(const QJsonObject &obj, const QString &key)
{
    const QJsonValue value = obj.value(key);
    return value.isDouble() ? static_cast<qint64>(value.toDouble()) : -1;
}

} // namespace

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

QSet<AbstractSocialAccount::StatSource> AbstractSocialAccount::supportedVideoStatSources() const
{
    return {};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> AbstractSocialAccount::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    Q_UNUSED(postUrl);
    VideoStatistics stats;
    stats.errorMessage = QObject::tr("%1 does not support fetching per-post statistics (%2).")
        .arg(getName(), statSourceKey(source));
    co_return stats;
}

QString AbstractSocialAccount::statSourceKey(StatSource source)
{
    return source == StatSource::Analytics
        ? QStringLiteral("analytics")
        : QStringLiteral("public");
}

AbstractSocialAccount::StatSource AbstractSocialAccount::statSourceFromKey(const QString &key)
{
    return key == QLatin1String("analytics")
        ? StatSource::Analytics
        : StatSource::PublicView;
}

QCoro::Task<QJsonObject> AbstractSocialAccount::_runJsonScript(
    const QString &scriptFileName, const QStringList &args) const
{
    const auto errorObject = [](const QString &message) {
        return QJsonObject{{QStringLiteral("error"), message}};
    };

    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        co_return errorObject(QObject::tr("python3 was not found on PATH."));
    }

    const QString scriptPath = QDir(QString::fromUtf8(SOCIAL_STATS_SCRIPT_DIR))
        .filePath(scriptFileName);
    if (!QFileInfo::exists(scriptPath))
    {
        co_return errorObject(QObject::tr("Statistics fetcher script not found at %1 "
            "(stale build? re-run cmake).").arg(scriptPath));
    }

    QProcess process;
    process.start(python, QStringList{scriptPath} + args);

    if (!co_await qCoro(process).waitForStarted())
    {
        co_return errorObject(QObject::tr("Could not start the statistics fetcher (%1).")
            .arg(process.errorString()));
    }

    // Generous timeout: most scripts finish in a few seconds, but some (e.g.
    // Pinterest, or any analytics fetch) may open a visible browser and wait
    // several minutes for the user to log in interactively before they can
    // proceed.
    if (!co_await qCoro(process).waitForFinished(360'000))
    {
        process.kill();
        co_return errorObject(QObject::tr("The statistics fetcher timed out."));
    }

    const QByteArray output = process.readAllStandardOutput().trimmed();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        const QByteArray stderrOutput = process.readAllStandardError().trimmed();
        co_return errorObject(output.isEmpty() && !stderrOutput.isEmpty()
            ? QObject::tr("The statistics fetcher failed: %1")
                .arg(QString::fromUtf8(stderrOutput).left(300))
            : QObject::tr("Unexpected output from the statistics fetcher: %1")
                .arg(QString::fromUtf8(output).left(200)));
    }

    co_return doc.object();
}

QCoro::Task<AbstractSocialAccount::Statistics> AbstractSocialAccount::runStatsScript(
    const QString &scriptFileName, const QString &url) const
{
    // GCC 13 ICEs when the co_await expression owns temporaries (braced-list
    // arguments, copy-init of the result) — keep both outside of it.
    const QStringList args{url};
    QJsonObject object;
    object = co_await _runJsonScript(scriptFileName, args);

    Statistics stats;
    stats.followers = jsonStat(object, QStringLiteral("followers"));
    stats.views = jsonStat(object, QStringLiteral("views"));
    stats.likes = jsonStat(object, QStringLiteral("likes"));
    stats.errorMessage = object.value(QStringLiteral("error")).toString();
    co_return stats;
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> AbstractSocialAccount::runVideoStatsScript(
    const QString &scriptFileName, const QString &postUrl, StatSource source) const
{
    const QStringList args{postUrl, statSourceKey(source)};
    QJsonObject object;
    object = co_await _runJsonScript(scriptFileName, args);

    VideoStatistics stats;
    stats.views = jsonStat(object, QStringLiteral("views"));
    stats.likes = jsonStat(object, QStringLiteral("likes"));
    stats.comments = jsonStat(object, QStringLiteral("comments"));
    stats.shares = jsonStat(object, QStringLiteral("shares"));
    stats.impressions = jsonStat(object, QStringLiteral("impressions"));
    stats.watchTimeSeconds = jsonStat(object, QStringLiteral("watch_time_seconds"));
    stats.followsFromVideo = jsonStat(object, QStringLiteral("follows"));
    const QJsonValue watchPercent = object.value(QStringLiteral("avg_watch_percent"));
    if (watchPercent.isDouble())
    {
        stats.avgWatchPercent = watchPercent.toDouble();
    }
    stats.errorMessage = object.value(QStringLiteral("error")).toString();
    co_return stats;
}
