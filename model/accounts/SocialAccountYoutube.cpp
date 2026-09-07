#include "SocialAccountYoutube.h"

QString SocialAccountYoutube::getId() const
{
    return QStringLiteral("youtube");
}

QString SocialAccountYoutube::getName() const
{
    return QStringLiteral("YouTube");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountYoutube::fetchStatistics(
    const QString &url) const
{
    return runStatsScript(QStringLiteral("fetch_stats_youtube.py"), url);
}

QSet<AbstractSocialAccount::StatSource> SocialAccountYoutube::supportedVideoStatSources() const
{
    // Public watch pages expose views/likes/comments. Analytics (YouTube
    // Studio) is not implemented yet — the dashboard is heavily dynamic and
    // needs its own dedicated script before we can declare it here.
    return {StatSource::PublicView};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> SocialAccountYoutube::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    return runVideoStatsScript(QStringLiteral("fetch_video_stats_youtube.py"), postUrl, source);
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountYoutube)
