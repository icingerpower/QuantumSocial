// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "StatisticsScheduler.h"

namespace {

constexpr int STARTUP_DELAY_MS = 5'000;
constexpr int PERIOD_MS = 60 * 60 * 1'000;

bool hasAnyStat(const AbstractSocialAccount::VideoStatistics &stats)
{
    return stats.views >= 0 || stats.likes >= 0 || stats.comments >= 0
        || stats.shares >= 0 || stats.impressions >= 0
        || stats.watchTimeSeconds >= 0 || stats.avgWatchPercent >= 0.0
        || stats.followsFromVideo >= 0;
}

} // namespace

StatisticsScheduler::StatisticsScheduler(TableVideos *videos, QObject *parent)
    : QObject(parent)
    , m_videos(videos)
{
    connect(&m_timer, &QTimer::timeout, this, &StatisticsScheduler::checkNow);
    m_timer.start(PERIOD_MS);
    QTimer::singleShot(STARTUP_DELAY_MS, this, &StatisticsScheduler::checkNow);
}

bool StatisticsScheduler::isRunning() const
{
    return m_running;
}

void StatisticsScheduler::checkNow()
{
    if (m_running)
    {
        return;
    }
    m_task = _fetchDue();
}

void StatisticsScheduler::fetchVideoNow(const QUuid &videoId, bool includeAnalytics)
{
    if (m_running)
    {
        return;
    }
    m_task = _fetchVideo(videoId, includeAnalytics);
}

QCoro::Task<void> StatisticsScheduler::_fetchDue()
{
    m_running = true;
    emit runStarted();

    int added = 0;
    QStringList errors;
    const auto due = m_videos->duePublications();
    for (const auto &publication : due)
    {
        if (co_await _fetchOne(publication.videoId, publication.platformId,
                               publication.postUrl,
                               AbstractSocialAccount::StatSource::PublicView, errors))
        {
            ++added;
        }
    }

    m_running = false;
    emit runFinished(added, errors);
}

QCoro::Task<void> StatisticsScheduler::_fetchVideo(QUuid videoId, bool includeAnalytics)
{
    m_running = true;
    emit runStarted();

    int added = 0;
    QStringList errors;
    // Copy the publication list first: fetching co_awaits, and the record
    // may move if the model changes while we are suspended.
    struct Target
    {
        QString platformId;
        QString postUrl;
    };
    QList<Target> targets;
    if (const TableVideos::VideoRecord *record = m_videos->recordFromId(videoId))
    {
        for (const auto &publication : record->publications)
        {
            targets << Target{publication.platformId, publication.postUrl};
        }
    }

    for (const Target &target : targets)
    {
        if (co_await _fetchOne(videoId, target.platformId, target.postUrl,
                               AbstractSocialAccount::StatSource::PublicView, errors))
        {
            ++added;
        }
        if (includeAnalytics
            && co_await _fetchOne(videoId, target.platformId, target.postUrl,
                                  AbstractSocialAccount::StatSource::Analytics, errors))
        {
            ++added;
        }
    }

    m_running = false;
    emit runFinished(added, errors);
}

QCoro::Task<bool> StatisticsScheduler::_fetchOne(
    QUuid videoId, QString platformId, QString postUrl,
    AbstractSocialAccount::StatSource source, QStringList &errors)
{
    AbstractSocialAccount *account
        = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS().value(platformId, nullptr);
    if (!account || !account->supportedVideoStatSources().contains(source)
        || postUrl.isEmpty())
    {
        co_return false;
    }

    const auto stats = co_await account->fetchVideoStatistics(postUrl, source);
    if (!stats.errorMessage.isEmpty())
    {
        errors << QStringLiteral("%1 (%2): %3")
            .arg(postUrl, AbstractSocialAccount::statSourceKey(source), stats.errorMessage);
    }
    if (!hasAnyStat(stats))
    {
        co_return false;
    }
    m_videos->addSnapshot(videoId, postUrl, source, stats);
    co_return true;
}
