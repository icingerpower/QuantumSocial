#ifndef SOCIALACCOUNTPINTEREST_H
#define SOCIALACCOUNTPINTEREST_H

#include "AbstractSocialAccount.h"

class SocialAccountPinterest : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
    QSet<StatSource> supportedVideoStatSources() const override;
    QCoro::Task<VideoStatistics> fetchVideoStatistics(
        const QString &postUrl, StatSource source) const override;
    bool supportsSpyScrollAndSave() const override;
    QString defaultSpySearchUrl(const QString &query) const override;
    QCoro::Task<SpyResult> spyScrollAndSave(
        const QString &queryOrUrl,
        const QString &downloadDir,
        int maxPosts = 24,
        bool autoScroll = true,
        int minEngagement = 0,
        const QStringList &excludeUrls = {},
        int maxDaysOld = 0) const override;
};

#endif // SOCIALACCOUNTPINTEREST_H
