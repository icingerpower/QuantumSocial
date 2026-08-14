#include "SocialAccountTiktok.h"

QString SocialAccountTiktok::getId() const
{
    return QStringLiteral("tiktok");
}

QString SocialAccountTiktok::getName() const
{
    return QStringLiteral("TikTok");
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountTiktok)
