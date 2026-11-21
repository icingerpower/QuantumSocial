#ifndef SOCIALSTATISTICS_H
#define SOCIALSTATISTICS_H

#include <QString>
#include <QStringList>
#include <QVariant>

#include <QCoro/QCoroTask>

#include "TableVideos.h"

class FavoriteHooks;
class PreferredHashtags;
class QHeaderView;
class QWidget;
class TableAccounts;

// Shared abstraction for social-media video statistics across PaneProperties
// (TreeProperties) and PaneStatistics (TableVideoStatistics):
//   1. Defines the canonical statistics columns (headers, default widths,
//      display formatting, and numeric sort values) so adding a metric or
//      updating how platform stats are displayed happens in one place.
//   2. Centralizes the "Retrieve" workflow that scans registered accounts in
//      TableAccounts, matches published posts back to TableVideos records,
//      and records publications + snapshots.
class SocialStatistics
{
public:
    enum class StatColumn
    {
        Views = 0,
        Likes,
        Comments,
        Shares,
        NormViews,
        Engagement,
        Count
    };

    static constexpr int columnCount()
    {
        return static_cast<int>(StatColumn::Count);
    }

    static QString header(int statColumnIndex);
    static QStringList headers();
    static int defaultColumnWidth(int statColumnIndex);
    static void applyColumnWidths(QHeaderView *header, int firstStatColumnIndex);

    static QVariant displayValue(const TableVideos::AggregateStats &stats, int statColumnIndex);
    static double numericSortValue(const TableVideos::AggregateStats &stats, int statColumnIndex);

    static bool hasAnyVideoStat(const AbstractSocialAccount::VideoStatistics &stats);
    static AbstractSocialAccount *resolveSocialAccount(const QString &typeOrId);
    static const TableVideos::VideoRecord *matchVideoRecord(
        const TableVideos *videos,
        const AbstractSocialAccount::AccountPostStatistics &post);

    static QCoro::Task<void> retrieveFromAccounts(TableAccounts *accounts,
                                                  TableVideos *videos,
                                                  QWidget *parentWidget = nullptr,
                                                  PreferredHashtags *hashtags = nullptr,
                                                  FavoriteHooks *favoriteHooks = nullptr);
};

#endif // SOCIALSTATISTICS_H
