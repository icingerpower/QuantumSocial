#include "SocialAccountTiktok.h"

QString SocialAccountTiktok::getId() const
{
    return QStringLiteral("tiktok");
}

QString SocialAccountTiktok::getName() const
{
    return QStringLiteral("TikTok");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountTiktok::fetchStatistics(
    const QString &url) const
{
    return runStatsScript(QStringLiteral("fetch_stats_tiktok.py"), url);
}

QSet<AbstractSocialAccount::StatSource> SocialAccountTiktok::supportedVideoStatSources() const
{
    // Analytics (TikTok Studio) requires being logged in as the owner; the
    // script keeps a persistent browser profile for that, same approach as
    // the Pinterest profile-stats script.
    return {StatSource::PublicView, StatSource::Analytics};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> SocialAccountTiktok::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    return runVideoStatsScript(QStringLiteral("fetch_video_stats_tiktok.py"), postUrl, source);
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountTiktok)
