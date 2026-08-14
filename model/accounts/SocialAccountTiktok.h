#ifndef SOCIALACCOUNTTIKTOK_H
#define SOCIALACCOUNTTIKTOK_H

#include "AbstractSocialAccount.h"

class SocialAccountTiktok : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
};

#endif // SOCIALACCOUNTTIKTOK_H
