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
    QString defaultSpyHomeUrl() const override;
    QCoro::Task<SpyResult> spyScrollAndSaveMode(
        const QString &queryOrUrl,
        const QString &downloadDir,
        int maxPosts,
        SpyMode mode,
        int minEngagement = 0,
        const QJsonObject &spyContext = {},
        int maxDaysOld = 0) const override;
};

#endif // SOCIALACCOUNTINSTAGRAM_H
