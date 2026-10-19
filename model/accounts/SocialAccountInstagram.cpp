// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "SocialAccountInstagram.h"

#include <QObject>

QString SocialAccountInstagram::getId() const
{
    return QStringLiteral("instagram");
}

QString SocialAccountInstagram::getName() const
{
    return QStringLiteral("Instagram");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountInstagram::fetchStatistics(
    const QString &url) const
{
    Q_UNUSED(url);
    Statistics stats;
    stats.errorMessage = QObject::tr("Retrieving statistics for %1 is not supported yet.")
        .arg(getName());
    co_return stats;
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountInstagram)
