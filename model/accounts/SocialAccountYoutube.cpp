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

DECLARE_SOCIAL_ACCOUNT(SocialAccountYoutube)
