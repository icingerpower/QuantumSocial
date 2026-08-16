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

DECLARE_SOCIAL_ACCOUNT(SocialAccountTiktok)
