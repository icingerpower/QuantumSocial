#ifndef SOCIALACCOUNTINSTAGRAM_H
#define SOCIALACCOUNTINSTAGRAM_H

#include "AbstractSocialAccount.h"

class SocialAccountInstagram : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
};

#endif // SOCIALACCOUNTINSTAGRAM_H
