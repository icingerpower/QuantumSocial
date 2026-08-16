#ifndef SOCIALACCOUNTYOUTUBE_H
#define SOCIALACCOUNTYOUTUBE_H

#include "AbstractSocialAccount.h"

class SocialAccountYoutube : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
};

#endif // SOCIALACCOUNTYOUTUBE_H
