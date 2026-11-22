// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 (internal
// compiler error / bad codegen) — force -O1 for this translation unit, same
// workaround used elsewhere in sibling projects for QCoro-based code.
#pragma GCC optimize("O1")

#include "AbstractSocialAccount.h"

#include <QCoro/QCoroTimer>
#include <chrono>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimeZone>

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

AbstractSocialAccount::VideoStatistics parseVideoStatsObject(const QJsonObject &object)
{
    AbstractSocialAccount::VideoStatistics stats;
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
    return stats;
}

QDateTime parsePublishDate(const QJsonValue &value)
{
    if (value.isDouble())
    {
        const qint64 timestamp = static_cast<qint64>(value.toDouble());
        if (timestamp > 0)
        {
            return QDateTime::fromSecsSinceEpoch(timestamp, QTimeZone::UTC);
        }
    }
    else if (value.isString())
    {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty())
        {
            bool ok = false;
            const qint64 ts = text.toLongLong(&ok);
            if (ok && ts > 0)
            {
                return QDateTime::fromSecsSinceEpoch(ts, QTimeZone::UTC);
            }
            const QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
            if (dt.isValid())
            {
                return dt.toUTC();
            }
        }
    }
    return QDateTime{};
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

bool AbstractSocialAccount::supportsAccountVideoStatistics() const
{
    return false;
}

QCoro::Task<AbstractSocialAccount::AccountVideosResult>
AbstractSocialAccount::fetchAccountVideoStatistics(
    const QString &profileUrl, const QStringList &knownShortCodes) const
{
    Q_UNUSED(profileUrl);
    Q_UNUSED(knownShortCodes);
    AccountVideosResult result;
    result.notSupported = true;
    result.errorMessage = QObject::tr("Retrieving published video statistics for %1 is not supported yet.")
        .arg(getName());
    co_return result;
}

bool AbstractSocialAccount::supportsSpyScrollAndSave() const
{
    return false;
}

QString AbstractSocialAccount::defaultSpySearchUrl(const QString &query) const
{
    Q_UNUSED(query);
    return QString{};
}

QCoro::Task<AbstractSocialAccount::SpyResult> AbstractSocialAccount::spyScrollAndSave(
    const QString &queryOrUrl,
    const QString &downloadDir,
    int maxPosts,
    bool autoScroll,
    int minEngagement,
    const QStringList &excludeUrls,
    int maxDaysOld) const
{
    Q_UNUSED(queryOrUrl);
    Q_UNUSED(downloadDir);
    Q_UNUSED(maxPosts);
    Q_UNUSED(autoScroll);
    Q_UNUSED(minEngagement);
    Q_UNUSED(excludeUrls);
    Q_UNUSED(maxDaysOld);
    SpyResult result;
    result.notSupported = true;
    result.errorMessage = QObject::tr("Scroll & save for %1 is not supported yet.")
        .arg(getName());
    co_return result;
}

bool AbstractSocialAccount::supportsAutoPublish() const
{
    return false;
}

QCoro::Task<AbstractSocialAccount::PublishResult> AbstractSocialAccount::publishCreative(
    const PublishRequest &request,
    const std::function<void(const QString &)> &onLog,
    std::shared_ptr<bool> cancelFlag) const
{
    Q_UNUSED(request);
    Q_UNUSED(onLog);
    Q_UNUSED(cancelFlag);
    PublishResult result;
    result.notSupported = true;
    result.fatal = true;
    result.errorMessage = QObject::tr("Auto-publish for %1 is not supported yet.")
        .arg(getName());
    co_return result;
}

QString AbstractSocialAccount::extractShortCode(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((?:#\s*qs[-_]?|\bqs[-_]?)([0-9a-hjkmnp-tv-z]{4,})\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
    {
        return QString{};
    }
    return QStringLiteral("QS-") + match.captured(1).toUpper();
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
    const QString &scriptFileName,
    const QStringList &args,
    qint64 timeoutMs,
    const QString &excludeFilePath) const
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
    if (!excludeFilePath.isEmpty())
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QS_SPY_EXCLUDE_FILE"), excludeFilePath);
        process.setProcessEnvironment(env);
    }
    process.start(python, QStringList{scriptPath} + args);

    // Observe the process on timer ticks, outside its signal delivery. A
    // QCoroProcess signal callback can otherwise outlive its awaiter when
    // this coroutine returns and destroys the local QProcess.
    QElapsedTimer timer;
    timer.start();
    while (process.state() == QProcess::Starting && timer.elapsed() < 30'000)
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    if (process.state() == QProcess::Starting
        || process.error() == QProcess::FailedToStart)
    {
        process.kill();
        process.waitForFinished(5'000);
        co_return errorObject(QObject::tr("Could not start the statistics fetcher (%1).")
            .arg(process.errorString()));
    }

    // Generous timeout for background stats scripts (default 6 min), or
    // unbounded (timeoutMs <= 0) for interactive Scroll & save sessions that
    // pause until a human completes login or CAPTCHA in the visible browser.
    timer.restart();
    while (process.state() != QProcess::NotRunning
        && !QCoreApplication::closingDown()
        && (timeoutMs <= 0 || timer.elapsed() < timeoutMs))
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    if (process.state() != QProcess::NotRunning)
    {
        process.kill();
        process.waitForFinished(5'000);
        co_return errorObject(QCoreApplication::closingDown()
            ? QObject::tr("Application is closing.")
            : QObject::tr("The statistics fetcher timed out."));
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

    VideoStatistics stats = parseVideoStatsObject(object);
    co_return stats;
}

QCoro::Task<AbstractSocialAccount::AccountVideosResult>
AbstractSocialAccount::runAccountVideosScript(
    const QString &scriptFileName, const QString &profileUrl,
    const QStringList &knownShortCodes) const
{
    QStringList args{profileUrl};
    if (!knownShortCodes.isEmpty())
    {
        args << knownShortCodes.join(QStringLiteral(","));
    }
    QJsonObject object;
    object = co_await _runJsonScript(scriptFileName, args);

    AccountVideosResult result;
    result.followers = jsonStat(object, QStringLiteral("followers"));
    result.views = jsonStat(object, QStringLiteral("views"));
    result.likes = jsonStat(object, QStringLiteral("likes"));
    result.errorMessage = object.value(QStringLiteral("error")).toString();

    for (const QJsonValue &postValue : object.value(QStringLiteral("posts")).toArray())
    {
        if (!postValue.isObject())
        {
            continue;
        }
        const QJsonObject postObj = postValue.toObject();
        AccountPostStatistics post;
        post.postUrl = postObj.value(QStringLiteral("url")).toString().trimmed();
        post.caption = postObj.value(QStringLiteral("caption")).toString();
        post.shortCode = postObj.value(QStringLiteral("short_code")).toString().trimmed();
        if (post.shortCode.isEmpty() && !post.caption.isEmpty())
        {
            post.shortCode = extractShortCode(post.caption);
        }
        post.publishDate = parsePublishDate(postObj.value(QStringLiteral("publish_date")));
        post.source = statSourceFromKey(postObj.value(QStringLiteral("source")).toString());
        post.stats = parseVideoStatsObject(postObj);
        result.posts << post;
    }

    co_return result;
}

QCoro::Task<AbstractSocialAccount::SpyResult> AbstractSocialAccount::runSpyScript(
    const QString &scriptFileName,
    const QString &queryOrUrl,
    const QString &downloadDir,
    int maxPosts,
    bool autoScroll,
    int minEngagement,
    const QStringList &excludeUrls,
    int maxDaysOld) const
{
    const QString mode = autoScroll ? QStringLiteral("auto") : QStringLiteral("manual");
    const QStringList args{
        queryOrUrl,
        downloadDir,
        QString::number(qMax(1, maxPosts)),
        mode,
        QString::number(qMax(0, minEngagement)),
        QString::number(qMax(0, maxDaysOld))
    };

    QTemporaryFile excludeFile;
    QString excludeFilePath;
    if (!excludeUrls.isEmpty() && excludeFile.open())
    {
        const QByteArray payload = QJsonDocument(QJsonArray::fromStringList(excludeUrls)).toJson(QJsonDocument::Compact);
        excludeFile.write(payload);
        excludeFile.flush();
        excludeFilePath = excludeFile.fileName();
    }

    const qint64 timeoutMs = qEnvironmentVariableIntValue("QS_SPY_HEADLESS") == 1 ? 60'000 : 0;
    QJsonObject object;
    object = co_await _runJsonScript(scriptFileName, args, timeoutMs, excludeFilePath);

    SpyResult result;
    result.errorMessage = object.value(QStringLiteral("error")).toString();
    result.minEngagement = object.value(QStringLiteral("min_score")).toInt(qMax(0, minEngagement));
    result.maxDaysOld = object.value(QStringLiteral("max_days_old")).toInt(qMax(0, maxDaysOld));
    result.browsedCount = object.value(QStringLiteral("browsed_count")).toInt(0);
    result.savedCount = object.value(QStringLiteral("saved_count")).toInt(0);
    result.rejectedCount = object.value(QStringLiteral("rejected_count")).toInt(0);

    for (const QJsonValue &postValue : object.value(QStringLiteral("posts")).toArray())
    {
        if (!postValue.isObject())
        {
            continue;
        }
        const QJsonObject postObj = postValue.toObject();
        SpyPost post;
        post.postUrl = postObj.value(QStringLiteral("url")).toString().trimmed();
        post.title = postObj.value(QStringLiteral("title")).toString().trimmed();
        post.mediaUrl = postObj.value(QStringLiteral("media_url")).toString().trimmed();
        post.localMediaPath = postObj.value(QStringLiteral("local_media")).toString().trimmed();
        post.author = postObj.value(QStringLiteral("author")).toString().trimmed();
        post.publishedAt = postObj.value(QStringLiteral("published_at")).toString().trimmed();
        post.views = jsonStat(postObj, QStringLiteral("views"));
        post.likes = jsonStat(postObj, QStringLiteral("likes"));
        post.comments = jsonStat(postObj, QStringLiteral("comments"));
        post.shares = jsonStat(postObj, QStringLiteral("shares"));
        post.isAd = postObj.value(QStringLiteral("is_ad")).toBool(false);
        if (!post.postUrl.isEmpty() || !post.localMediaPath.isEmpty())
        {
            result.posts << post;
        }
    }

    if (result.savedCount <= 0 && !result.posts.isEmpty())
    {
        result.savedCount = result.posts.size();
    }
    if (result.browsedCount < result.savedCount + result.rejectedCount)
    {
        result.browsedCount = result.savedCount + result.rejectedCount;
    }

    co_return result;
}

QCoro::Task<AbstractSocialAccount::PublishResult> AbstractSocialAccount::runPublishScript(
    const QString &scriptFileName,
    const PublishRequest &request,
    std::function<void(const QString &)> onLog,
    std::shared_ptr<bool> cancelFlag) const
{
    PublishResult result;
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
    {
        result.fatal = true;
        result.errorMessage = QObject::tr("python3 was not found on PATH.");
        co_return result;
    }

    const QString scriptPath = QDir(QString::fromUtf8(SOCIAL_STATS_SCRIPT_DIR))
        .filePath(scriptFileName);
    if (!QFileInfo::exists(scriptPath))
    {
        result.fatal = true;
        result.errorMessage = QObject::tr("Publish script not found at %1.").arg(scriptPath);
        co_return result;
    }

    QTemporaryFile reqFile;
    if (!reqFile.open())
    {
        result.fatal = true;
        result.errorMessage = QObject::tr("Could not create temporary publish request file.");
        co_return result;
    }
    const QJsonObject reqObj{
        {QStringLiteral("accountUrl"), request.accountUrl},
        {QStringLiteral("mediaPaths"), QJsonArray::fromStringList(request.mediaPaths)},
        {QStringLiteral("caption"), request.caption},
        {QStringLiteral("videoOverlayText"), request.videoOverlayText},
        {QStringLiteral("videoTextStyle"), request.videoTextStyle},
        {QStringLiteral("shortCode"), request.shortCode},
        {QStringLiteral("useSystemChromeProfile"), request.useSystemChromeProfile},
        {QStringLiteral("muteOriginalAudio"), request.muteOriginalAudio},
        {QStringLiteral("useSuggestedSound"), request.useSuggestedSound},
        {QStringLiteral("dryRun"), request.dryRun},
        {QStringLiteral("uploadUrlOverride"), request.uploadUrlOverride}
    };
    reqFile.write(QJsonDocument(reqObj).toJson(QJsonDocument::Compact));
    reqFile.flush();

    QProcess process;
    process.start(python, {scriptPath, reqFile.fileName()});

    QByteArray stderrBuffer;
    const auto flushStderrLines = [&]() {
        stderrBuffer += process.readAllStandardError();
        while (true)
        {
            const qsizetype nl = stderrBuffer.indexOf('\n');
            if (nl < 0)
            {
                break;
            }
            const QString line = QString::fromUtf8(stderrBuffer.left(nl)).trimmed();
            stderrBuffer.remove(0, nl + 1);
            if (!line.isEmpty() && onLog)
            {
                onLog(line);
            }
        }
    };

    QElapsedTimer timer;
    timer.start();
    while (process.state() == QProcess::Starting && timer.elapsed() < 30'000)
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    if (process.state() == QProcess::Starting || process.error() == QProcess::FailedToStart)
    {
        process.kill();
        process.waitForFinished(3'000);
        result.fatal = true;
        result.errorMessage = QObject::tr("Could not start publish worker (%1).")
            .arg(process.errorString());
        co_return result;
    }

    const qint64 timeoutMs = qEnvironmentVariableIntValue("QS_PUBLISH_HEADLESS") == 1 ? 60'000 : 0;
    timer.restart();
    while (process.state() != QProcess::NotRunning
           && !QCoreApplication::closingDown()
           && !(cancelFlag && *cancelFlag)
           && (timeoutMs <= 0 || timer.elapsed() < timeoutMs))
    {
        flushStderrLines();
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    flushStderrLines();

    if (process.state() != QProcess::NotRunning)
    {
        process.terminate();
        if (!process.waitForFinished(2'500))
        {
            process.kill();
            process.waitForFinished(2'000);
        }
        result.fatal = true;
        result.errorMessage = (cancelFlag && *cancelFlag)
            ? QObject::tr("Publish job stopped by user.")
            : QObject::tr("Publish worker timed out.");
        co_return result;
    }

    if (!stderrBuffer.trimmed().isEmpty() && onLog)
    {
        onLog(QString::fromUtf8(stderrBuffer).trimmed());
    }

    const QByteArray stdoutBytes = process.readAllStandardOutput().trimmed();
    const qsizetype lastNl = stdoutBytes.lastIndexOf('\n');
    const QByteArray jsonLine = lastNl >= 0 ? stdoutBytes.mid(lastNl + 1).trimmed() : stdoutBytes;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonLine, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        result.fatal = true;
        result.errorMessage = QObject::tr("Unexpected output from publish worker: %1")
            .arg(QString::fromUtf8(stdoutBytes).left(240));
        co_return result;
    }

    const QJsonObject obj = doc.object();
    result.published = obj.value(QStringLiteral("published")).toBool(false);
    result.dryRun = obj.value(QStringLiteral("dry_run")).toBool(false);
    result.fatal = obj.value(QStringLiteral("fatal")).toBool(false);
    result.videoTextApplied = obj.value(QStringLiteral("video_text_applied")).toBool(false);
    result.videoTextStyle = obj.value(QStringLiteral("video_text_style")).toString().trimmed();
    result.postUrl = obj.value(QStringLiteral("post_url")).toString().trimmed();
    result.soundTitle = obj.value(QStringLiteral("sound_title")).toString().trimmed();
    result.logFilePath = obj.value(QStringLiteral("log_file")).toString().trimmed();
    result.screenshotPath = obj.value(QStringLiteral("screenshot")).toString().trimmed();
    result.errorMessage = obj.value(QStringLiteral("error")).toString().trimmed();
    co_return result;
}
