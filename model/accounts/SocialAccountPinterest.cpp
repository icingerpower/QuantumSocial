#include "SocialAccountPinterest.h"

QString SocialAccountPinterest::getId() const
{
    return QStringLiteral("pinterest");
}

QString SocialAccountPinterest::getName() const
{
    return QStringLiteral("Pinterest");
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountPinterest)
