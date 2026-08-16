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

DECLARE_SOCIAL_ACCOUNT(SocialAccountPinterest)
