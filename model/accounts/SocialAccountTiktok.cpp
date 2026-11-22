#include "SocialAccountTiktok.h"

#include <QUrl>

QString SocialAccountTiktok::getId() const
{
    return QStringLiteral("tiktok");
}

QString SocialAccountTiktok::getName() const
{
    return QStringLiteral("TikTok");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountTiktok::fetchStatistics(
    const QString &url) const
{
    return runStatsScript(QStringLiteral("fetch_stats_tiktok.py"), url);
}

QSet<AbstractSocialAccount::StatSource> SocialAccountTiktok::supportedVideoStatSources() const
{
    // Analytics (TikTok Studio) requires being logged in as the owner; the
    // script keeps a persistent browser profile for that, same approach as
    // the Pinterest profile-stats script.
    return {StatSource::PublicView, StatSource::Analytics};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> SocialAccountTiktok::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    return runVideoStatsScript(QStringLiteral("fetch_video_stats_tiktok.py"), postUrl, source);
}

bool SocialAccountTiktok::supportsAccountVideoStatistics() const
{
    return true;
}

QCoro::Task<AbstractSocialAccount::AccountVideosResult>
SocialAccountTiktok::fetchAccountVideoStatistics(
    const QString &profileUrl, const QStringList &knownShortCodes) const
{
    return runAccountVideosScript(
        QStringLiteral("fetch_account_videos_tiktok.py"), profileUrl, knownShortCodes);
}

bool SocialAccountTiktok::supportsSpyScrollAndSave() const
{
    return true;
}

QString SocialAccountTiktok::defaultSpySearchUrl(const QString &query) const
{
    return QStringLiteral("https://www.tiktok.com/search?q=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed())));
}

QString SocialAccountTiktok::defaultSpyHomeUrl() const
{
    return QStringLiteral("https://www.tiktok.com/");
}

QCoro::Task<AbstractSocialAccount::SpyResult> SocialAccountTiktok::spyScrollAndSaveMode(
    const QString &queryOrUrl,
    const QString &downloadDir,
    int maxPosts,
    SpyMode mode,
    int minEngagement,
    const QJsonObject &spyContext,
    int maxDaysOld) const
{
    return runSpyScriptMode(
        QStringLiteral("spy_scroll_tiktok.py"),
        queryOrUrl,
        downloadDir,
        maxPosts,
        mode,
        minEngagement,
        spyContext,
        maxDaysOld);
}

bool SocialAccountTiktok::supportsAutoPublish() const
{
    return true;
}

QCoro::Task<AbstractSocialAccount::PublishResult> SocialAccountTiktok::publishCreative(
    const PublishRequest &request,
    const std::function<void(const QString &)> &onLog,
    std::shared_ptr<bool> cancelFlag) const
{
    return runPublishScript(
        QStringLiteral("publish_tiktok.py"), request, onLog, std::move(cancelFlag));
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountTiktok)
