#include "SocialAccountYoutube.h"

QString SocialAccountYoutube::getId() const
{
    return QStringLiteral("youtube");
}

QString SocialAccountYoutube::getName() const
{
    return QStringLiteral("YouTube");
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountYoutube)
