#include "SocialAccountPinterest.h"

QString SocialAccountPinterest::getId() const
{
    return QStringLiteral("pinterest");
}

QString SocialAccountPinterest::getName() const
{
    return QStringLiteral("Pinterest");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountPinterest::fetchStatistics(
    const QString &url) const
{
    return runStatsScript(QStringLiteral("fetch_stats_pinterest.py"), url);
}

QSet<AbstractSocialAccount::StatSource> SocialAccountPinterest::supportedVideoStatSources() const
{
    // Pin pages publicly expose reaction and comment counts; the pin-stats
    // overlay (impressions, saves, ...) shown to the pin's owner is handled
    // by the analytics source reusing the same persistent browser profile as
    // fetch_stats_pinterest.py.
    return {StatSource::PublicView, StatSource::Analytics};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> SocialAccountPinterest::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    return runVideoStatsScript(QStringLiteral("fetch_video_stats_pinterest.py"), postUrl, source);
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountPinterest)
