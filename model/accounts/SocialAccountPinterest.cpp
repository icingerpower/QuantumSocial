#include "SocialAccountPinterest.h"

#include <QUrl>

QString SocialAccountPinterest::getId() const
{
    return QStringLiteral("pinterest");
}

QString SocialAccountPinterest::getName() const
{
    return QStringLiteral("Pinterest");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountPinterest::fetchStatistics(
    const QString &url) const
{
    return runStatsScript(QStringLiteral("fetch_stats_pinterest.py"), url);
}

QSet<AbstractSocialAccount::StatSource> SocialAccountPinterest::supportedVideoStatSources() const
{
    // Pin pages publicly expose reaction and comment counts; the pin-stats
    // overlay (impressions, saves, ...) shown to the pin's owner is handled
    // by the analytics source reusing the same persistent browser profile as
    // fetch_stats_pinterest.py.
    return {StatSource::PublicView, StatSource::Analytics};
}

QCoro::Task<AbstractSocialAccount::VideoStatistics> SocialAccountPinterest::fetchVideoStatistics(
    const QString &postUrl, StatSource source) const
{
    return runVideoStatsScript(QStringLiteral("fetch_video_stats_pinterest.py"), postUrl, source);
}

bool SocialAccountPinterest::supportsSpyScrollAndSave() const
{
    return true;
}

QString SocialAccountPinterest::defaultSpySearchUrl(const QString &query) const
{
    return QStringLiteral("https://www.pinterest.com/search/pins/?q=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed())));
}

QCoro::Task<AbstractSocialAccount::SpyResult> SocialAccountPinterest::spyScrollAndSave(
    const QString &queryOrUrl,
    const QString &downloadDir,
    int maxPosts,
    bool autoScroll,
    int minEngagement,
    const QStringList &excludeUrls,
    int maxDaysOld) const
{
    return runSpyScript(
        QStringLiteral("spy_scroll_pinterest.py"),
        queryOrUrl,
        downloadDir,
        maxPosts,
        autoScroll,
        minEngagement,
        excludeUrls,
        maxDaysOld);
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountPinterest)
