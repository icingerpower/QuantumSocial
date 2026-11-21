// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "SocialAccountInstagram.h"

#include <QObject>
#include <QUrl>

QString SocialAccountInstagram::getId() const
{
    return QStringLiteral("instagram");
}

QString SocialAccountInstagram::getName() const
{
    return QStringLiteral("Instagram");
}

QCoro::Task<AbstractSocialAccount::Statistics> SocialAccountInstagram::fetchStatistics(
    const QString &url) const
{
    Q_UNUSED(url);
    Statistics stats;
    stats.errorMessage = QObject::tr("Retrieving statistics for %1 is not supported yet.")
        .arg(getName());
    co_return stats;
}

bool SocialAccountInstagram::supportsSpyScrollAndSave() const
{
    return true;
}

QString SocialAccountInstagram::defaultSpySearchUrl(const QString &query) const
{
    QString tag = query.trimmed().toLower();
    tag.remove(QLatin1Char('#'));
    tag.remove(QLatin1Char(' '));
    if (tag.isEmpty())
    {
        return QStringLiteral("https://www.instagram.com/explore/");
    }
    return QStringLiteral("https://www.instagram.com/explore/search/keyword/?q=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed())));
}

QCoro::Task<AbstractSocialAccount::SpyResult> SocialAccountInstagram::spyScrollAndSave(
    const QString &queryOrUrl,
    const QString &downloadDir,
    int maxPosts,
    bool autoScroll,
    int minEngagement,
    const QStringList &excludeUrls,
    int maxDaysOld) const
{
    return runSpyScript(
        QStringLiteral("spy_scroll_instagram.py"),
        queryOrUrl,
        downloadDir,
        maxPosts,
        autoScroll,
        minEngagement,
        excludeUrls,
        maxDaysOld);
}

DECLARE_SOCIAL_ACCOUNT(SocialAccountInstagram)
