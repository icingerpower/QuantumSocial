#ifndef SOCIALACCOUNTPINTEREST_H
#define SOCIALACCOUNTPINTEREST_H

#include "AbstractSocialAccount.h"

class SocialAccountPinterest : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
};

#endif // SOCIALACCOUNTPINTEREST_H
