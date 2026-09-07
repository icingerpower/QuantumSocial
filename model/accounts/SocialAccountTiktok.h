#ifndef SOCIALACCOUNTTIKTOK_H
#define SOCIALACCOUNTTIKTOK_H

#include "AbstractSocialAccount.h"

class SocialAccountTiktok : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
    QSet<StatSource> supportedVideoStatSources() const override;
    QCoro::Task<VideoStatistics> fetchVideoStatistics(
        const QString &postUrl, StatSource source) const override;
};

#endif // SOCIALACCOUNTTIKTOK_H
