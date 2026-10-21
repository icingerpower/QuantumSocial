// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "SocialStatistics.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHeaderView>
#include <QMessageBox>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QWidget>

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "../TableAccounts.h"

namespace {

struct ColumnInfo
{
    SocialStatistics::StatColumn column;
    const char *header;
    int defaultWidth;
};

constexpr ColumnInfo STAT_COLUMNS[] = {
    {SocialStatistics::StatColumn::Views, "Views", 80},
    {SocialStatistics::StatColumn::Likes, "Likes", 75},
    {SocialStatistics::StatColumn::Comments, "Comments", 85},
    {SocialStatistics::StatColumn::Shares, "Shares/Saves", 100},
    {SocialStatistics::StatColumn::NormViews, "Views/Follower", 110},
    {SocialStatistics::StatColumn::Engagement, "Engagement", 95},
};

QString primaryRetrieveStatsLogPath()
{
    const QDir workDir = WorkingDirectoryManager::instance()->workingDir();
    if (workDir.exists())
    {
        QDir logsDir(workDir.filePath(QStringLiteral("logs")));
        if (!logsDir.exists())
        {
            QDir().mkpath(logsDir.absolutePath());
        }
        return logsDir.filePath(QStringLiteral("retrieve_statistics.log"));
    }
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QDir cacheDir(cacheRoot.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache/quantumsocial")
                                      : cacheRoot + QStringLiteral("/quantumsocial"));
    if (!cacheDir.exists())
    {
        QDir().mkpath(cacheDir.absolutePath());
    }
    return cacheDir.filePath(QStringLiteral("retrieve_statistics.log"));
}

void appendRetrieveStatsLog(const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const QString line = QStringLiteral("[%1] %2\n").arg(timestamp, message);
    qInfo().noquote() << "[RetrieveStatistics]" << message;

    QStringList paths;
    const QDir workDir = WorkingDirectoryManager::instance()->workingDir();
    if (workDir.exists())
    {
        QDir logsDir(workDir.filePath(QStringLiteral("logs")));
        if (!logsDir.exists())
        {
            QDir().mkpath(logsDir.absolutePath());
        }
        paths << logsDir.filePath(QStringLiteral("retrieve_statistics.log"));
    }
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QDir cacheDir(cacheRoot.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache/quantumsocial")
                                      : cacheRoot + QStringLiteral("/quantumsocial"));
    if (!cacheDir.exists())
    {
        QDir().mkpath(cacheDir.absolutePath());
    }
    const QString cacheLogPath = cacheDir.filePath(QStringLiteral("retrieve_statistics.log"));
    if (!paths.contains(cacheLogPath))
    {
        paths << cacheLogPath;
    }

    for (const QString &path : paths)
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            QTextStream stream(&file);
            stream << line;
        }
    }
}

} // namespace

QString SocialStatistics::header(int statColumnIndex)
{
    if (statColumnIndex < 0 || statColumnIndex >= columnCount())
    {
        return QString{};
    }
    return QString::fromLatin1(STAT_COLUMNS[statColumnIndex].header);
}

QStringList SocialStatistics::headers()
{
    QStringList list;
    list.reserve(columnCount());
    for (int i = 0; i < columnCount(); ++i)
    {
        list << header(i);
    }
    return list;
}

int SocialStatistics::defaultColumnWidth(int statColumnIndex)
{
    if (statColumnIndex < 0 || statColumnIndex >= columnCount())
    {
        return 80;
    }
    return STAT_COLUMNS[statColumnIndex].defaultWidth;
}

void SocialStatistics::applyColumnWidths(QHeaderView *headerView, int firstStatColumnIndex)
{
    if (!headerView)
    {
        return;
    }
    for (int i = 0; i < columnCount(); ++i)
    {
        headerView->resizeSection(firstStatColumnIndex + i, defaultColumnWidth(i));
    }
}

QVariant SocialStatistics::displayValue(const TableVideos::AggregateStats &stats,
                                        int statColumnIndex)
{
    if (statColumnIndex < 0 || statColumnIndex >= columnCount())
    {
        return QVariant{};
    }
    switch (static_cast<StatColumn>(statColumnIndex))
    {
    case StatColumn::Views:
        return stats.totalViews >= 0 ? QVariant{stats.totalViews} : QVariant{};
    case StatColumn::Likes:
        return stats.totalLikes >= 0 ? QVariant{stats.totalLikes} : QVariant{};
    case StatColumn::Comments:
        return stats.totalComments >= 0 ? QVariant{stats.totalComments} : QVariant{};
    case StatColumn::Shares:
        return stats.totalShares >= 0 ? QVariant{stats.totalShares} : QVariant{};
    case StatColumn::NormViews:
        return stats.medianNormViews >= 0.0
            ? QVariant{QString::number(stats.medianNormViews, 'f', 2)}
            : QVariant{};
    case StatColumn::Engagement:
        return stats.medianEngagement >= 0.0
            ? QVariant{QStringLiteral("%1 %")
                  .arg(QString::number(stats.medianEngagement * 100.0, 'f', 1))}
            : QVariant{};
    case StatColumn::Count:
        break;
    }
    return QVariant{};
}

double SocialStatistics::numericSortValue(const TableVideos::AggregateStats &stats,
                                          int statColumnIndex)
{
    if (statColumnIndex < 0 || statColumnIndex >= columnCount())
    {
        return -1.0;
    }
    switch (static_cast<StatColumn>(statColumnIndex))
    {
    case StatColumn::Views:
        return static_cast<double>(stats.totalViews);
    case StatColumn::Likes:
        return static_cast<double>(stats.totalLikes);
    case StatColumn::Comments:
        return static_cast<double>(stats.totalComments);
    case StatColumn::Shares:
        return static_cast<double>(stats.totalShares);
    case StatColumn::NormViews:
        return stats.medianNormViews;
    case StatColumn::Engagement:
        return stats.medianEngagement;
    case StatColumn::Count:
        break;
    }
    return -1.0;
}

bool SocialStatistics::hasAnyVideoStat(const AbstractSocialAccount::VideoStatistics &stats)
{
    return stats.views >= 0 || stats.likes >= 0 || stats.comments >= 0
        || stats.shares >= 0 || stats.impressions >= 0
        || stats.watchTimeSeconds >= 0 || stats.avgWatchPercent >= 0.0
        || stats.followsFromVideo >= 0;
}

AbstractSocialAccount *SocialStatistics::resolveSocialAccount(const QString &typeOrId)
{
    const QString trimmed = typeOrId.trimmed();
    if (trimmed.isEmpty())
    {
        return nullptr;
    }
    const auto &allAccounts = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();
    if (AbstractSocialAccount *byId = allAccounts.value(trimmed.toLower(), nullptr))
    {
        return byId;
    }
    for (AbstractSocialAccount *candidate : allAccounts)
    {
        if (candidate->getName().compare(trimmed, Qt::CaseInsensitive) == 0
            || candidate->getId().compare(trimmed, Qt::CaseInsensitive) == 0)
        {
            return candidate;
        }
    }
    return nullptr;
}

const TableVideos::VideoRecord *SocialStatistics::matchVideoRecord(
    const TableVideos *videos,
    const AbstractSocialAccount::AccountPostStatistics &post)
{
    if (!videos)
    {
        return nullptr;
    }
    if (!post.shortCode.isEmpty())
    {
        if (const auto *record = videos->recordFromShortCode(post.shortCode))
        {
            return record;
        }
    }
    if (!post.caption.isEmpty())
    {
        static const QRegularExpression pattern(
            QStringLiteral(R"((?:#\s*qs[-_]?|\bqs[-_]?)([0-9a-hjkmnp-tv-z]{4,})\b)"),
            QRegularExpression::CaseInsensitiveOption);
        auto it = pattern.globalMatch(post.caption);
        while (it.hasNext())
        {
            const QRegularExpressionMatch match = it.next();
            if (const auto *record = videos->recordFromShortCode(match.captured(0)))
            {
                return record;
            }
        }
    }
    if (!post.postUrl.isEmpty())
    {
        if (const auto *record = videos->recordFromPostUrl(post.postUrl))
        {
            return record;
        }
    }
    return nullptr;
}

QCoro::Task<void> SocialStatistics::retrieveFromAccounts(TableAccounts *accounts,
                                                         TableVideos *videos,
                                                         QWidget *parentWidget)
{
    appendRetrieveStatsLog(QStringLiteral("=== Retrieve statistics started ==="));
    struct AccountRowInfo
    {
        int row = -1;
        QString typeName;
        QString url;
        qint64 followers = -1;
    };
    QList<AccountRowInfo> accountRows;
    if (accounts)
    {
        for (int row = 0; row < accounts->rowCount(); ++row)
        {
            const QString typeName
                = accounts->data(accounts->index(row, TableAccounts::IND_TYPE)).toString().trimmed();
            const QString url
                = accounts->data(accounts->index(row, TableAccounts::IND_URL)).toString().trimmed();
            const qint64 followers
                = accounts->data(accounts->index(row, TableAccounts::IND_FOLLOWERS)).toLongLong();
            if (!url.isEmpty())
            {
                accountRows << AccountRowInfo{row, typeName, url, followers};
            }
        }
    }

    if (accountRows.isEmpty())
    {
        appendRetrieveStatsLog(QStringLiteral("No accounts registered in TableAccounts; aborting."));
        if (parentWidget)
        {
            QMessageBox::information(parentWidget, QObject::tr("No account"),
                QObject::tr("Add at least one account in the Accounts tab first."));
        }
        co_return;
    }

    QStringList knownShortCodes;
    if (videos)
    {
        for (int row = 0; row < videos->rowCount(); ++row)
        {
            if (const auto *record = videos->recordAt(row))
            {
                if (!record->shortCode.isEmpty())
                {
                    knownShortCodes << record->shortCode;
                }
            }
        }
    }
    appendRetrieveStatsLog(
        QStringLiteral("Accounts to query: %1 | Known video shortCodes in TableVideos: %2")
            .arg(accountRows.size())
            .arg(knownShortCodes.size()));

    if (parentWidget)
    {
        parentWidget->setEnabled(false);
    }
    QStringList unsupportedMessages;
    QStringList errors;
    QSet<QString> refreshedPostUrls;
    QHash<QString, qint64> platformFollowers;
    int totalMatchedVideos = 0;

    for (const AccountRowInfo &entry : accountRows)
    {
        AbstractSocialAccount *account = resolveSocialAccount(entry.typeName);
        if (!account)
        {
            const QString msg = QObject::tr("%1: unknown account type \"%2\"")
                .arg(entry.url, entry.typeName);
            appendRetrieveStatsLog(QStringLiteral("ERROR: ") + msg);
            errors << msg;
            continue;
        }

        if (!account->supportsAccountVideoStatistics())
        {
            const QString msg = QObject::tr("%1 (%2): retrieving published video statistics is not supported yet.")
                .arg(account->getName(), entry.url);
            appendRetrieveStatsLog(QStringLiteral("UNSUPPORTED: ") + msg);
            unsupportedMessages << msg;
            continue;
        }

        appendRetrieveStatsLog(QStringLiteral("Fetching account video statistics for %1 (%2)...")
                                   .arg(account->getName(), entry.url));
        const auto accountResult = co_await account->fetchAccountVideoStatistics(entry.url, knownShortCodes);
        if (accountResult.notSupported)
        {
            const QString msg = QObject::tr("%1 (%2): %3")
                .arg(account->getName(), entry.url,
                     accountResult.errorMessage.isEmpty()
                         ? QObject::tr("retrieving published video statistics is not supported yet.")
                         : accountResult.errorMessage);
            appendRetrieveStatsLog(QStringLiteral("UNSUPPORTED: ") + msg);
            unsupportedMessages << msg;
            continue;
        }
        appendRetrieveStatsLog(
            QStringLiteral("Result for %1: followers=%2, views=%3, likes=%4, posts=%5, error=\"%6\"")
                .arg(entry.url)
                .arg(accountResult.followers)
                .arg(accountResult.views)
                .arg(accountResult.likes)
                .arg(accountResult.posts.size())
                .arg(accountResult.errorMessage));

        qint64 followers = entry.followers;
        if (accountResult.followers >= 0 && accounts)
        {
            accounts->setData(accounts->index(entry.row, TableAccounts::IND_FOLLOWERS),
                              accountResult.followers);
            followers = accountResult.followers;
        }
        if (accountResult.views >= 0 && accounts)
        {
            accounts->setData(accounts->index(entry.row, TableAccounts::IND_VIEW),
                              accountResult.views);
        }
        if (accountResult.likes >= 0 && accounts)
        {
            accounts->setData(accounts->index(entry.row, TableAccounts::IND_LIKES),
                              accountResult.likes);
        }
        if (followers > 0)
        {
            platformFollowers.insert(account->getId(), followers);
        }
        if (!accountResult.errorMessage.isEmpty())
        {
            errors << QStringLiteral("%1: %2").arg(entry.url, accountResult.errorMessage);
        }
        else if (accountResult.posts.isEmpty())
        {
            const QString warnMsg = QObject::tr("%1: 0 published videos were returned (see %2 for details).")
                .arg(entry.url, primaryRetrieveStatsLogPath());
            appendRetrieveStatsLog(QStringLiteral("WARNING: ") + warnMsg);
            errors << warnMsg;
        }

        if (!videos)
        {
            appendRetrieveStatsLog(QStringLiteral("WARNING: videos model is null; cannot match posts to videos."));
            continue;
        }

        for (const auto &post : accountResult.posts)
        {
            const TableVideos::VideoRecord *record = matchVideoRecord(videos, post);
            if (!record)
            {
                appendRetrieveStatsLog(
                    QStringLiteral("  Post %1 (shortCode=\"%2\") did not match any video in TableVideos.")
                        .arg(post.postUrl, post.shortCode));
                continue;
            }
            const QUuid videoId = record->id;
            const QString postUrl = !post.postUrl.isEmpty()
                ? post.postUrl
                : QStringLiteral("%1#%2").arg(entry.url, record->shortCode);
            const QDateTime publishDate = post.publishDate.isValid()
                ? post.publishDate
                : (record->generatedDate.isValid()
                       ? record->generatedDate
                       : QDateTime::currentDateTimeUtc());

            appendRetrieveStatsLog(
                QStringLiteral("  Matched %1 -> %2 (properties=%3, views=%4, likes=%5, comments=%6, shares=%7, followers=%8)")
                    .arg(record->shortCode, postUrl)
                    .arg(record->propertyValueIds.size())
                    .arg(post.stats.views)
                    .arg(post.stats.likes)
                    .arg(post.stats.comments)
                    .arg(post.stats.shares)
                    .arg(followers));

            videos->addPublication(videoId, account->getId(), postUrl,
                                   publishDate, followers > 0 ? followers : -1);
            if (hasAnyVideoStat(post.stats))
            {
                videos->addSnapshot(videoId, postUrl, post.source, post.stats);
            }
            refreshedPostUrls.insert(postUrl);
            ++totalMatchedVideos;
        }
    }

    // Also refresh any already-attached publication URLs on published videos
    // that were not covered by the profile page scan above.
    if (videos)
    {
        struct ExistingTarget
        {
            QUuid videoId;
            QString platformId;
            QString postUrl;
            QDateTime publishDate;
            qint64 followersAtPublish = -1;
        };
        QList<ExistingTarget> targets;
        for (int row = 0; row < videos->rowCount(); ++row)
        {
            const TableVideos::VideoRecord *record = videos->recordAt(row);
            if (!record)
            {
                continue;
            }
            for (const TableVideos::Publication &pub : record->publications)
            {
                if (!pub.postUrl.isEmpty() && !refreshedPostUrls.contains(pub.postUrl))
                {
                    targets << ExistingTarget{record->id, pub.platformId, pub.postUrl,
                                              pub.publishDate, pub.followersAtPublish};
                    refreshedPostUrls.insert(pub.postUrl);
                }
            }
        }

        for (const ExistingTarget &target : targets)
        {
            AbstractSocialAccount *account = resolveSocialAccount(target.platformId);
            if (!account)
            {
                continue;
            }
            const qint64 knownFollowers = platformFollowers.value(account->getId(), -1);
            if (target.followersAtPublish <= 0 && knownFollowers > 0)
            {
                videos->addPublication(target.videoId, account->getId(), target.postUrl,
                                       target.publishDate, knownFollowers);
            }
            if (!account->supportedVideoStatSources().contains(
                    AbstractSocialAccount::StatSource::PublicView))
            {
                continue;
            }
            const auto stats = co_await account->fetchVideoStatistics(
                target.postUrl, AbstractSocialAccount::StatSource::PublicView);
            if (!stats.errorMessage.isEmpty())
            {
                errors << QStringLiteral("%1: %2").arg(target.postUrl, stats.errorMessage);
            }
            if (hasAnyVideoStat(stats))
            {
                videos->addSnapshot(target.videoId, target.postUrl,
                                    AbstractSocialAccount::StatSource::PublicView, stats);
            }
        }
    }

    appendRetrieveStatsLog(
        QStringLiteral("=== Retrieve statistics finished: matchedVideos=%1, unsupported=%2, errors=%3 ===")
            .arg(totalMatchedVideos)
            .arg(unsupportedMessages.size())
            .arg(errors.size()));

    if (parentWidget)
    {
        parentWidget->setEnabled(true);
    }

    if (parentWidget && !unsupportedMessages.isEmpty())
    {
        QMessageBox::information(parentWidget, QObject::tr("Not supported yet"),
            unsupportedMessages.join(QStringLiteral("\n")));
    }
    if (parentWidget && !errors.isEmpty())
    {
        QMessageBox::warning(parentWidget, QObject::tr("Some statistics could not be retrieved"),
            errors.join(QStringLiteral("\n")));
    }
}
