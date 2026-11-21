#ifndef SOCIALACCOUNTINSTAGRAM_H
#define SOCIALACCOUNTINSTAGRAM_H

#include "AbstractSocialAccount.h"

class SocialAccountInstagram : public AbstractSocialAccount
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Statistics> fetchStatistics(const QString &url) const override;
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

#endif // SOCIALACCOUNTINSTAGRAM_H
