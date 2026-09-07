#ifndef SOCIALACCOUNTYOUTUBE_H
#define SOCIALACCOUNTYOUTUBE_H

#include "AbstractSocialAccount.h"

class SocialAccountYoutube : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
    QSet<StatSource> supportedVideoStatSources() const override;
    QCoro::Task<VideoStatistics> fetchVideoStatistics(
        const QString &postUrl, StatSource source) const override;
};

#endif // SOCIALACCOUNTYOUTUBE_H
