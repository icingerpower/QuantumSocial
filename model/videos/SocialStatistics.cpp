// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "SocialStatistics.h"
#include "model/research/InspirationAdaptationMethod.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QWidget>

#include <algorithm>

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "../FavoriteHooks.h"
#include "../PreferredHashtags.h"
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

QJsonObject SocialStatistics::VideoTextExperimentSummary::toJson() const
{
    QJsonArray winnersArr;
    for (const auto &w : winningTexts)
    {
        winnersArr.append(QJsonObject{
            {QStringLiteral("shortCode"), w.shortCode},
            {QStringLiteral("text"), w.text},
            {QStringLiteral("style"), w.styleId},
            {QStringLiteral("views"), w.views},
            {QStringLiteral("likes"), w.likes}
        });
    }
    QJsonArray stylesArr;
    for (const auto &st : styleBreakdown)
    {
        stylesArr.append(QJsonObject{
            {QStringLiteral("style"), st.styleId},
            {QStringLiteral("count"), st.count},
            {QStringLiteral("avgViews"), st.avgViews},
            {QStringLiteral("avgEngagementPct"), st.avgEngagementPct},
            {QStringLiteral("bestViews"), st.bestViews}
        });
    }
    return QJsonObject{
        {QStringLiteral("withTextCount"), withTextCount},
        {QStringLiteral("withTextAvgViews"), withTextAvgViews},
        {QStringLiteral("withTextAvgEngagementPct"), withTextAvgEngagementPct},
        {QStringLiteral("withTextBestViews"), withTextBestViews},
        {QStringLiteral("withoutTextCount"), withoutTextCount},
        {QStringLiteral("withoutTextAvgViews"), withoutTextAvgViews},
        {QStringLiteral("withoutTextAvgEngagementPct"), withoutTextAvgEngagementPct},
        {QStringLiteral("withoutTextBestViews"), withoutTextBestViews},
        {QStringLiteral("recommendation"), recommendation},
        {QStringLiteral("winningStyle"), winningStyle},
        {QStringLiteral("styleBreakdown"), stylesArr},
        {QStringLiteral("winningTexts"), winnersArr}
    };
}

QString SocialStatistics::VideoTextExperimentSummary::humanSummary() const
{
    if (withTextCount == 0 && withoutTextCount == 0)
    {
        return QObject::tr("Video Text A/B: 0 measured videos yet — exploring ~50/50 With vs Without native TikTok text and rotating 4 curated styles.");
    }
    QString verdict;
    if (recommendation == QLatin1String("prefer_text"))
    {
        verdict = QObject::tr("Native TikTok text is outperforming -> favoring text overlays (~75%) while keeping ~25% clean holdout");
    }
    else if (recommendation == QLatin1String("prefer_clean"))
    {
        verdict = QObject::tr("Clean unobstructed video is outperforming -> favoring clean video (~75%) while testing fresh hooks (~25%)");
    }
    else if (recommendation == QLatin1String("balanced"))
    {
        verdict = QObject::tr("Both perform similarly -> alternating ~50/50 by creative fit");
    }
    else
    {
        verdict = QObject::tr("Gathering baseline A/B sample -> alternating ~50/50");
    }
    const QString styleNote = !winningStyle.isEmpty()
        ? QObject::tr(" | Top style: %1").arg(winningStyle)
        : (!styleBreakdown.isEmpty()
               ? QObject::tr(" | Styles tested: %1").arg(styleBreakdown.size())
               : QString{});
    return QObject::tr("Video Text A/B: With text=%1 video(s) (avg %2 views, %3% eng) vs Clean=%4 video(s) (avg %5 views, %6% eng)%8 — %7.")
        .arg(withTextCount)
        .arg(qRound64(withTextAvgViews))
        .arg(QString::number(withTextAvgEngagementPct, 'f', 1))
        .arg(withoutTextCount)
        .arg(qRound64(withoutTextAvgViews))
        .arg(QString::number(withoutTextAvgEngagementPct, 'f', 1))
        .arg(verdict, styleNote);
}

QString SocialStatistics::VideoTextExperimentSummary::plannerPromptAddendum() const
{
    QString out = QStringLiteral("\nMeasured TikTok Native Video Text ('Aa Texte') A/B Performance on this account:\n");
    out += QStringLiteral("- Videos WITH native TikTok text overlay: %1 measured | avg views=%2 | best views=%3 | avg engagement=%4%\n")
        .arg(withTextCount)
        .arg(qRound64(withTextAvgViews))
        .arg(withTextBestViews)
        .arg(QString::number(withTextAvgEngagementPct, 'f', 2));
    out += QStringLiteral("- Videos WITHOUT text overlay (clean video): %1 measured | avg views=%2 | best views=%3 | avg engagement=%4%\n")
        .arg(withoutTextCount)
        .arg(qRound64(withoutTextAvgViews))
        .arg(withoutTextBestViews)
        .arg(QString::number(withoutTextAvgEngagementPct, 'f', 2));

    out += QStringLiteral("- Available curated fashion-safe \"videoTextStyle\" options (choose the best visual fit + A/B test over time):\n"
                          "  1. \"classic_black_pill\"   — Bold white Classic text on rounded black pill (high contrast, great on bright marble/daylight)\n"
                          "  2. \"classic_white_pill\"   — Bold black Classic text on crisp white pill (trendy OOTD/fashion creator look, great on dark evening scenes)\n"
                          "  3. \"elegance_serif\"       — Editorial Serif white text with subtle dark stroke+shadow, no box (luxury Vogue vibe)\n"
                          "  4. \"classic_clean_stroke\" — Bold white Classic text with clean dark outline, no box (minimal footprint over dress/heels)\n");

    if (!styleBreakdown.isEmpty())
    {
        out += QStringLiteral("- Measured performance by videoTextStyle so far:\n");
        for (const auto &st : styleBreakdown)
        {
            out += QStringLiteral("  * %1: %2 video(s) | avg views=%3 | best=%4 | eng=%5%\n")
                .arg(st.styleId)
                .arg(st.count)
                .arg(qRound64(st.avgViews))
                .arg(st.bestViews)
                .arg(QString::number(st.avgEngagementPct, 'f', 2));
        }
        if (!winningStyle.isEmpty())
        {
            out += QStringLiteral("  -> Leading style: \"%1\" (use ~60-70% of the time when adding text, while rotating the other 3 styles ~30-40% to keep testing).\n")
                .arg(winningStyle);
        }
        else
        {
            out += QStringLiteral("  -> Rotate across all 4 styles based on scene brightness/mood so each style accumulates measured data.\n");
        }
    }

    if (!winningTexts.isEmpty())
    {
        out += QStringLiteral("- Highest-performing video overlay texts measured so far (imitate their brevity/curiosity style, never copy verbatim):\n");
        for (int i = 0; i < qMin<int>(5, winningTexts.size()); ++i)
        {
            const auto &w = winningTexts[i];
            out += QStringLiteral("  * \"%1\" [style=%2] (%3 views, %4 likes)\n")
                .arg(w.text, w.styleId).arg(w.views).arg(w.likes);
        }
    }

    if (recommendation == QLatin1String("prefer_text"))
    {
        out += QStringLiteral("- Strategy directive: Native TikTok text overlays are winning! Set \"addVideoText\": true on ~75-80% of VIDEO items (using a punchy 4-8 word curiosity/story \"videoOverlayText\"), and false on ~20-25% as a clean control.\n");
    }
    else if (recommendation == QLatin1String("prefer_clean"))
    {
        out += QStringLiteral("- Strategy directive: Clean unobstructed videos currently get more views! Set \"addVideoText\": false on ~75-80% of VIDEO items, and test \"addVideoText\": true on ~20-25% with a sharper, shorter 4-7 word hook to see if it beats the baseline.\n");
    }
    else
    {
        out += QStringLiteral("- Strategy directive: Alternate ~50/50 between \"addVideoText\": true and \"addVideoText\": false across VIDEO items so we measure and improve what works best on this account. Always still provide a concise 4-8 word \"videoOverlayText\" suggestion even when \"addVideoText\" is false so the user can toggle it on if desired.\n");
    }
    return out;
}

SocialStatistics::VideoTextExperimentSummary SocialStatistics::summarizeVideoTextExperiment(
    const TableVideos *videos,
    FavoriteHooks *favoriteHooksToPromote)
{
    VideoTextExperimentSummary summary;
    summary.recommendation = QStringLiteral("explore");
    if (!videos)
    {
        return summary;
    }

    // Build fast shortCode -> generation directory lookup across both current
    // (projects/generations/<folder>/<shortCode>) and legacy (projects/<id>/generations/<shortCode>) layouts
    QHash<QString, QString> genDirByCode;
    const auto indexGenerationsInDir = [&](const QDir &gensDir) {
        if (!gensDir.exists())
        {
            return;
        }
        const QStringList genEntries = gensDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &ge : genEntries)
        {
            const QString fullPath = gensDir.filePath(ge);
            const QString extracted = AbstractSocialAccount::extractShortCode(ge);
            if (!extracted.isEmpty())
            {
                genDirByCode.insert(extracted.toUpper(), fullPath);
            }
            else
            {
                genDirByCode.insert(ge.toUpper(), fullPath);
            }
        }
    };

    const QDir wd = WorkingDirectoryManager::instance()->workingDir();
    const QDir projectsRoot{wd.filePath(QStringLiteral("projects"))};
    if (projectsRoot.exists())
    {
        const QDir currentGensRoot{projectsRoot.filePath(QStringLiteral("generations"))};
        if (currentGensRoot.exists())
        {
            const QStringList projGenFolders = currentGensRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &pgf : projGenFolders)
            {
                indexGenerationsInDir(QDir{currentGensRoot.filePath(pgf)});
            }
        }
        const QStringList projDirs = projectsRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &pd : projDirs)
        {
            if (pd == QLatin1String("generations"))
            {
                continue;
            }
            indexGenerationsInDir(QDir{projectsRoot.filePath(pd + QStringLiteral("/generations"))});
        }
    }

    struct StyleAccum
    {
        int count = 0;
        double viewsSum = 0.0;
        double engSum = 0.0;
        qint64 bestViews = 0;
    };
    QHash<QString, StyleAccum> styleAccums;

    double withViewsSum = 0.0;
    double withEngSum = 0.0;
    double withoutViewsSum = 0.0;
    double withoutEngSum = 0.0;

    for (int r = 0; r < videos->rowCount(); ++r)
    {
        const auto *rec = videos->recordAt(r);
        if (!rec || (!rec->published && rec->publications.isEmpty()))
        {
            continue;
        }

        // Find highest measured views + likes/comments/shares across snapshots of this record
        qint64 bestViews = -1;
        qint64 bestLikes = 0;
        qint64 bestComments = 0;
        qint64 bestShares = 0;
        for (const auto &pub : rec->publications)
        {
            for (const auto &snap : pub.snapshots)
            {
                if (snap.stats.views >= 0 && snap.stats.views >= bestViews)
                {
                    bestViews = snap.stats.views;
                    bestLikes = qMax<qint64>(0, snap.stats.likes);
                    bestComments = qMax<qint64>(0, snap.stats.comments);
                    bestShares = qMax<qint64>(0, snap.stats.shares);
                }
            }
        }
        if (bestViews < 0)
        {
            continue;
        }

        const QString gPath = genDirByCode.value(rec->shortCode.toUpper());
        if (gPath.isEmpty())
        {
            continue;
        }
        const QDir gDir{gPath};
        const bool hasMp4 = gDir.exists(QStringLiteral("final_video.mp4"))
            || !gDir.entryList({QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm")}, QDir::Files).isEmpty();
        if (!hasMp4)
        {
            continue; // Exclude photo carousels / static images so video-text A/B compares videos fairly
        }

        bool hasVideoText = false;
        QString overlayText;
        QString overlayStyle;
        QFile dnaFile{gDir.filePath(QStringLiteral("creative_dna.json"))};
        if (dnaFile.open(QIODevice::ReadOnly))
        {
            const QJsonObject dna = QJsonDocument::fromJson(dnaFile.readAll()).object();
            hasVideoText = dna.value(QStringLiteral("tiktokVideoTextAdded")).toBool(false);
            overlayText = dna.value(QStringLiteral("tiktokVideoText")).toString().trimmed();
            overlayStyle = dna.value(QStringLiteral("tiktokVideoTextStyle")).toString().trimmed();
        }
        if (hasVideoText && overlayStyle.isEmpty())
        {
            overlayStyle = QStringLiteral("classic_black_pill");
        }

        const double engPct = bestViews > 0
            ? (100.0 * double(bestLikes + bestComments + bestShares) / double(bestViews))
            : 0.0;

        if (hasVideoText)
        {
            ++summary.withTextCount;
            withViewsSum += double(bestViews);
            withEngSum += engPct;
            summary.withTextBestViews = qMax(summary.withTextBestViews, bestViews);

            auto &sa = styleAccums[overlayStyle];
            ++sa.count;
            sa.viewsSum += double(bestViews);
            sa.engSum += engPct;
            sa.bestViews = qMax(sa.bestViews, bestViews);

            if (!overlayText.isEmpty())
            {
                summary.winningTexts.append(WinningVideoTextEntry{
                    rec->shortCode, overlayText, overlayStyle, bestViews, bestLikes
                });
            }
        }
        else
        {
            ++summary.withoutTextCount;
            withoutViewsSum += double(bestViews);
            withoutEngSum += engPct;
            summary.withoutTextBestViews = qMax(summary.withoutTextBestViews, bestViews);
        }
    }

    for (auto it = styleAccums.constBegin(); it != styleAccums.constEnd(); ++it)
    {
        const StyleAccum &sa = it.value();
        if (sa.count <= 0)
        {
            continue;
        }
        summary.styleBreakdown.append(VideoTextStyleStats{
            it.key(),
            sa.count,
            sa.viewsSum / double(sa.count),
            sa.engSum / double(sa.count),
            sa.bestViews
        });
    }
    std::sort(summary.styleBreakdown.begin(), summary.styleBreakdown.end(),
              [](const VideoTextStyleStats &a, const VideoTextStyleStats &b) {
                  if (a.avgViews != b.avgViews) return a.avgViews > b.avgViews;
                  return a.bestViews > b.bestViews;
              });
    if (summary.styleBreakdown.size() >= 2
        && summary.styleBreakdown[0].count >= 1
        && summary.styleBreakdown[0].avgViews >= summary.styleBreakdown[1].avgViews * 1.10)
    {
        summary.winningStyle = summary.styleBreakdown[0].styleId;
    }

    if (summary.withTextCount > 0)
    {
        summary.withTextAvgViews = withViewsSum / double(summary.withTextCount);
        summary.withTextAvgEngagementPct = withEngSum / double(summary.withTextCount);
    }
    if (summary.withoutTextCount > 0)
    {
        summary.withoutTextAvgViews = withoutViewsSum / double(summary.withoutTextCount);
        summary.withoutTextAvgEngagementPct = withoutEngSum / double(summary.withoutTextCount);
    }

    std::sort(summary.winningTexts.begin(), summary.winningTexts.end(),
              [](const WinningVideoTextEntry &a, const WinningVideoTextEntry &b) {
                  if (a.views != b.views) return a.views > b.views;
                  return a.likes > b.likes;
              });
    if (summary.winningTexts.size() > 8)
    {
        summary.winningTexts.resize(8);
    }

    if (summary.withTextCount >= 2 && summary.withoutTextCount >= 2)
    {
        if (summary.withTextAvgViews >= summary.withoutTextAvgViews * 1.10)
        {
            summary.recommendation = QStringLiteral("prefer_text");
        }
        else if (summary.withoutTextAvgViews > summary.withTextAvgViews * 1.10)
        {
            summary.recommendation = QStringLiteral("prefer_clean");
        }
        else
        {
            summary.recommendation = QStringLiteral("balanced");
        }
    }
    else
    {
        summary.recommendation = QStringLiteral("explore");
    }

    if (favoriteHooksToPromote && !summary.winningTexts.isEmpty())
    {
        const double baselineViews = qMax(25.0, qMax(summary.withTextAvgViews, summary.withoutTextAvgViews));
        for (int i = 0; i < qMin<int>(3, summary.winningTexts.size()); ++i)
        {
            if (double(summary.winningTexts[i].views) >= baselineViews)
            {
                favoriteHooksToPromote->addHook(summary.winningTexts[i].text, FavoriteHooks::SOURCE_AI);
            }
        }
    }

    return summary;
}

namespace
{
double medianOfDoubles(QList<double> vals)
{
    if (vals.isEmpty())
    {
        return 0.0;
    }
    std::sort(vals.begin(), vals.end());
    const int n = vals.size();
    return (n % 2 == 1) ? vals[n / 2] : 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

QHash<QString, QString> indexAllGenerationDirsInWorkingDir()
{
    QHash<QString, QString> genDirByCode;
    const auto indexGenerationsInDir = [&](const QDir &gensDir) {
        if (!gensDir.exists())
        {
            return;
        }
        const QStringList genEntries = gensDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &ge : genEntries)
        {
            const QString fullPath = gensDir.filePath(ge);
            const QString extracted = AbstractSocialAccount::extractShortCode(ge);
            if (!extracted.isEmpty())
            {
                genDirByCode.insert(extracted.toUpper(), fullPath);
            }
            else
            {
                genDirByCode.insert(ge.toUpper(), fullPath);
            }
        }
    };

    const QDir wd = WorkingDirectoryManager::instance()->workingDir();
    const QDir projectsRoot{wd.filePath(QStringLiteral("projects"))};
    if (projectsRoot.exists())
    {
        const QDir currentGensRoot{projectsRoot.filePath(QStringLiteral("generations"))};
        if (currentGensRoot.exists())
        {
            const QStringList projGenFolders = currentGensRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &pgf : projGenFolders)
            {
                indexGenerationsInDir(QDir{currentGensRoot.filePath(pgf)});
            }
        }
        const QStringList projDirs = projectsRoot.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &pd : projDirs)
        {
            if (pd == QLatin1String("generations"))
            {
                continue;
            }
            indexGenerationsInDir(QDir{projectsRoot.filePath(pd + QStringLiteral("/generations"))});
        }
    }
    return genDirByCode;
}
} // namespace

QJsonObject SocialStatistics::ReferenceExperimentSummary::toJson() const
{
    QJsonArray stratArr;
    for (const auto &s : strategyBreakdown)
    {
        stratArr.append(QJsonObject{
            {QStringLiteral("strategyId"), s.strategyId},
            {QStringLiteral("count"), s.count},
            {QStringLiteral("medianScore"), s.medianScore},
            {QStringLiteral("avgViews"), s.avgViews},
            {QStringLiteral("avgEngagementPct"), s.avgEngagementPct},
            {QStringLiteral("bestViews"), s.bestViews}
        });
    }
    QJsonArray refArr;
    for (const auto &r : references)
    {
        refArr.append(QJsonObject{
            {QStringLiteral("referenceId"), r.referenceId},
            {QStringLiteral("referenceUrl"), r.referenceUrl},
            {QStringLiteral("publishedCount"), r.publishedCount},
            {QStringLiteral("medianScore"), r.medianScore},
            {QStringLiteral("avgViews"), r.avgViews},
            {QStringLiteral("bestViews"), r.bestViews},
            {QStringLiteral("swapCount"), r.swapCount},
            {QStringLiteral("swapMedianScore"), r.swapMedianScore},
            {QStringLiteral("variantCount"), r.variantCount},
            {QStringLiteral("variantMedianScore"), r.variantMedianScore},
            {QStringLiteral("bestStrategy"), r.bestStrategy},
            {QStringLiteral("isWinner"), r.isWinner}
        });
    }
    return QJsonObject{
        {QStringLiteral("totalMeasuredPosts"), totalMeasuredPosts},
        {QStringLiteral("accountMedianScore"), accountMedianScore},
        {QStringLiteral("winningStrategy"), winningStrategy},
        {QStringLiteral("strategyBreakdown"), stratArr},
        {QStringLiteral("references"), refArr}
    };
}

QString SocialStatistics::ReferenceExperimentSummary::humanSummary() const
{
    if (totalMeasuredPosts <= 0)
    {
        return QStringLiteral("Reference & Variant A/B: 0 measured posts yet (will compare strict product swap vs scene/style variants).");
    }
    QStringList parts;
    for (const auto &s : strategyBreakdown)
    {
        parts << QStringLiteral("%1=%2 avg views (n=%3)")
                     .arg(s.strategyId)
                     .arg(qRound64(s.avgViews))
                     .arg(s.count);
    }
    int winnersCount = 0;
    for (const auto &r : references)
    {
        if (r.isWinner) ++winnersCount;
    }
    return QStringLiteral("Reference & Variant A/B (%1 measured): %2 · %3 winning source reference(s)%4.")
        .arg(totalMeasuredPosts)
        .arg(parts.join(QStringLiteral(", ")))
        .arg(winnersCount)
        .arg(winningStrategy.isEmpty() ? QString{} : QStringLiteral(" · leader=%1").arg(winningStrategy));
}

QString SocialStatistics::ReferenceExperimentSummary::plannerPromptAddendum() const
{
    if (totalMeasuredPosts <= 0)
    {
        return QString{};
    }
    QString out = QStringLiteral(
        "\nSOURCE VIDEO & ADAPTATION MODE LEARNING (%1 measured published post(s)):\n")
        .arg(totalMeasuredPosts);
    for (const auto &s : strategyBreakdown)
    {
        const auto *method = InspirationAdaptationMethod::all().value(s.strategyId, nullptr);
        const QString label = method
            ? QStringLiteral("%1 (%2)").arg(s.strategyId, method->name())
            : s.strategyId;
        out += QStringLiteral("  - %1: n=%2, avg views=%3, best views=%4, avg engagement=%5%\n")
            .arg(label)
            .arg(s.count)
            .arg(qRound64(s.avgViews))
            .arg(s.bestViews)
            .arg(QString::number(s.avgEngagementPct, 'f', 1));
    }
    if (!winningStrategy.isEmpty())
    {
        out += QStringLiteral("  - Winning adaptation strategy so far: %1 (favor this ~70% while continuing ~30% variant exploration).\n")
            .arg(winningStrategy);
    }
    int listed = 0;
    for (const auto &r : references)
    {
        if (!r.isWinner || listed >= 5)
        {
            continue;
        }
        ++listed;
        out += QStringLiteral("  - Proven winning source video %1: avg views=%2 (n=%3, best mode=%4)\n")
            .arg(r.referenceUrl.isEmpty() ? r.referenceId : r.referenceUrl)
            .arg(qRound64(r.avgViews))
            .arg(r.publishedCount)
            .arg(r.bestStrategy.isEmpty() ? QStringLiteral("reference_swap") : r.bestStrategy);
    }
    return out;
}

SocialStatistics::ReferenceExperimentSummary SocialStatistics::summarizeReferenceExperiment(
    const TableVideos *videos)
{
    ReferenceExperimentSummary summary;
    if (!videos)
    {
        return summary;
    }

    const QHash<QString, QString> genDirByCode = indexAllGenerationDirsInWorkingDir();

    struct StratAccum
    {
        QList<double> scores;
        double viewsSum = 0.0;
        double engPctSum = 0.0;
        qint64 bestViews = 0;
    };
    struct RefAccum
    {
        QString referenceUrl;
        QList<double> allScores;
        QHash<QString, QList<double>> scoresByMethod;
        double viewsSum = 0.0;
        qint64 bestViews = 0;
    };

    QHash<QString, StratAccum> stratMap;
    QHash<QString, RefAccum> refMap;
    QList<double> allAccountScores;

    for (int r = 0; r < videos->rowCount(); ++r)
    {
        const auto *rec = videos->recordAt(r);
        if (!rec || (!rec->published && rec->publications.isEmpty()))
        {
            continue;
        }

        double bestNormScore = -1.0;
        qint64 bestViews = -1;
        qint64 bestLikes = 0;
        qint64 bestComments = 0;
        qint64 bestShares = 0;
        for (const auto &pub : rec->publications)
        {
            for (const auto &snap : pub.snapshots)
            {
                if (snap.stats.views < 0)
                {
                    continue;
                }
                const double views = qMax<qint64>(0, snap.stats.views);
                const double savesShares = qMax<qint64>(0, snap.stats.shares);
                const double likesComments = qMax<qint64>(0, snap.stats.likes)
                    + qMax<qint64>(0, snap.stats.comments);
                const double denom = pub.followersAtPublish > 0
                    ? double(pub.followersAtPublish)
                    : 1000.0;
                const double score = (0.45 * views + 2.5 * savesShares + 1.0 * likesComments) / denom;
                if (score > bestNormScore)
                {
                    bestNormScore = score;
                }
                if (snap.stats.views >= bestViews)
                {
                    bestViews = snap.stats.views;
                    bestLikes = qMax<qint64>(0, snap.stats.likes);
                    bestComments = qMax<qint64>(0, snap.stats.comments);
                    bestShares = qMax<qint64>(0, snap.stats.shares);
                }
            }
        }
        if (bestNormScore < 0.0 || bestViews < 0)
        {
            continue;
        }

        const QString gPath = genDirByCode.value(rec->shortCode.toUpper());
        if (gPath.isEmpty())
        {
            continue;
        }
        QFile dnaFile{QDir{gPath}.filePath(QStringLiteral("creative_dna.json"))};
        if (!dnaFile.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QJsonObject dna = QJsonDocument::fromJson(dnaFile.readAll()).object();
        const QString refId = dna.value(QStringLiteral("referenceId")).toString().trimmed();
        const QString refUrl = dna.value(QStringLiteral("referenceUrl")).toString().trimmed();
        QString strategy = dna.value(QStringLiteral("strategy")).toString().trimmed();
        if (strategy.isEmpty())
        {
            strategy = refId.isEmpty() ? QStringLiteral("editorial_ab") : QStringLiteral("reference_swap");
        }

        const double engPct = bestViews > 0
            ? (100.0 * double(bestLikes + bestComments + bestShares) / double(bestViews))
            : 0.0;

        ++summary.totalMeasuredPosts;
        allAccountScores.append(bestNormScore);

        auto &sa = stratMap[strategy];
        sa.scores.append(bestNormScore);
        sa.viewsSum += double(bestViews);
        sa.engPctSum += engPct;
        sa.bestViews = qMax(sa.bestViews, bestViews);

        if (!refId.isEmpty())
        {
            auto &ra = refMap[refId];
            if (ra.referenceUrl.isEmpty() && !refUrl.isEmpty())
            {
                ra.referenceUrl = refUrl;
            }
            ra.allScores.append(bestNormScore);
            ra.scoresByMethod[strategy].append(bestNormScore);
            ra.viewsSum += double(bestViews);
            ra.bestViews = qMax(ra.bestViews, bestViews);
        }
    }

    summary.accountMedianScore = medianOfDoubles(allAccountScores);

    for (auto it = stratMap.cbegin(); it != stratMap.cend(); ++it)
    {
        const auto &sa = it.value();
        if (sa.scores.isEmpty()) continue;
        AdaptationStrategyStats st;
        st.strategyId = it.key();
        st.count = sa.scores.size();
        st.medianScore = medianOfDoubles(sa.scores);
        st.avgViews = sa.viewsSum / double(st.count);
        st.avgEngagementPct = sa.engPctSum / double(st.count);
        st.bestViews = sa.bestViews;
        summary.strategyBreakdown.append(st);
    }
    std::sort(summary.strategyBreakdown.begin(), summary.strategyBreakdown.end(),
              [](const AdaptationStrategyStats &a, const AdaptationStrategyStats &b) {
                  if (a.medianScore != b.medianScore)
                  {
                      return a.medianScore > b.medianScore;
                  }
                  return a.avgViews > b.avgViews;
              });

    if (summary.strategyBreakdown.size() >= 2
        && summary.strategyBreakdown[0].count >= 2
        && summary.strategyBreakdown[0].medianScore >= summary.strategyBreakdown[1].medianScore * 1.15)
    {
        summary.winningStrategy = summary.strategyBreakdown[0].strategyId;
    }

    for (auto it = refMap.cbegin(); it != refMap.cend(); ++it)
    {
        const auto &ra = it.value();
        if (ra.allScores.isEmpty()) continue;
        ReferencePerformance rp;
        rp.referenceId = it.key();
        rp.referenceUrl = ra.referenceUrl;
        rp.publishedCount = ra.allScores.size();
        rp.medianScore = medianOfDoubles(ra.allScores);
        rp.avgViews = ra.viewsSum / double(rp.publishedCount);
        rp.bestViews = ra.bestViews;
        const QList<double> swapList = ra.scoresByMethod.value(QStringLiteral("reference_swap"));
        rp.swapCount = swapList.size();
        rp.swapMedianScore = swapList.isEmpty() ? -1.0 : medianOfDoubles(swapList);

        QList<double> nonSwapScores;
        double topModeScore = -1.0;
        for (auto mit = ra.scoresByMethod.cbegin(); mit != ra.scoresByMethod.cend(); ++mit)
        {
            if (mit.value().isEmpty())
            {
                continue;
            }
            const double med = medianOfDoubles(mit.value());
            if (med > topModeScore)
            {
                topModeScore = med;
                rp.bestStrategy = mit.key();
            }
            if (mit.key() != QLatin1String("reference_swap"))
            {
                nonSwapScores.append(mit.value());
            }
        }
        rp.variantCount = nonSwapScores.size();
        rp.variantMedianScore = nonSwapScores.isEmpty() ? -1.0 : medianOfDoubles(nonSwapScores);
        rp.isWinner = (rp.medianScore > 0.0 && rp.medianScore >= summary.accountMedianScore);
        summary.references.append(rp);
    }
    std::sort(summary.references.begin(), summary.references.end(),
              [](const ReferencePerformance &a, const ReferencePerformance &b) {
                  if (a.medianScore != b.medianScore)
                  {
                      return a.medianScore > b.medianScore;
                  }
                  return a.bestViews > b.bestViews;
              });

    return summary;
}

QCoro::Task<void> SocialStatistics::retrieveFromAccounts(TableAccounts *accounts,
                                                         TableVideos *videos,
                                                         QWidget *parentWidget,
                                                         PreferredHashtags *hashtags,
                                                         FavoriteHooks *favoriteHooks)
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
    QHash<QString, QJsonObject> discoveredTopPostsByUrl;
    {
        const QDir wd = WorkingDirectoryManager::instance()->workingDir();
        QFile existingTop{wd.filePath(QStringLiteral("account_top_posts.json"))};
        if (existingTop.open(QIODevice::ReadOnly))
        {
            for (const QJsonValue &v : QJsonDocument::fromJson(existingTop.readAll()).object().value(QStringLiteral("posts")).toArray())
            {
                const QJsonObject po = v.toObject();
                const QString u = po.value(QStringLiteral("postUrl")).toString();
                if (!u.isEmpty())
                {
                    discoveredTopPostsByUrl.insert(u, po);
                }
            }
        }
    }
    int harvestedHashtags = 0;
    int harvestedHooks = 0;

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

        // Compute median views across this account's returned posts so we automatically
        // harvest proven winning #hashtags & opening hooks into the Inspiration bank!
        QList<qint64> accountViewList;
        for (const auto &p : accountResult.posts)
        {
            if (p.stats.views >= 0)
            {
                accountViewList << p.stats.views;
            }
        }
        std::sort(accountViewList.begin(), accountViewList.end());
        const qint64 medianAccountViews = accountViewList.isEmpty()
            ? 100
            : accountViewList[accountViewList.size() / 2];
        const qint64 winnerViewThreshold = qMax<qint64>(80, medianAccountViews);

        static const QRegularExpression hashtagExtractRe{QStringLiteral("(#[A-Za-z0-9_]+)")};
        static const QRegularExpression qsTagRe{QStringLiteral("^#?qs[-_]?[0-9a-z]+$"),
                                                 QRegularExpression::CaseInsensitiveOption};

        for (const auto &post : accountResult.posts)
        {
            if (!post.postUrl.isEmpty() && post.stats.views >= 0)
            {
                QString cleanText = post.caption;
                cleanText.remove(QRegularExpression{QStringLiteral("(?:#\\s*)?\\bqs[-_]?[0-9a-z]{4,}\\b"),
                                                    QRegularExpression::CaseInsensitiveOption});
                QStringList postTags;
                auto tagIt = hashtagExtractRe.globalMatch(cleanText);
                while (tagIt.hasNext())
                {
                    const QString t = tagIt.next().captured(1);
                    if (!qsTagRe.match(t).hasMatch()
                        && t.compare(QLatin1String("#milestone"), Qt::CaseInsensitive) != 0)
                    {
                        postTags << t.toLower();
                    }
                }
                QString bodyWithoutTags = cleanText;
                bodyWithoutTags.remove(hashtagExtractRe);
                bodyWithoutTags = bodyWithoutTags.simplified();

                QString openingHook;
                const int qMark = bodyWithoutTags.indexOf(QLatin1Char('?'));
                const int dotPos = bodyWithoutTags.indexOf(QStringLiteral(". "));
                const int dashPos = bodyWithoutTags.indexOf(QStringLiteral(" — "));
                int splitAt = -1;
                for (int cand : {qMark >= 0 ? qMark + 1 : -1, dotPos >= 0 ? dotPos + 1 : -1, dashPos})
                {
                    if (cand >= 14 && (splitAt < 0 || cand < splitAt))
                    {
                        splitAt = cand;
                    }
                }
                openingHook = (splitAt > 0 ? bodyWithoutTags.left(splitAt) : bodyWithoutTags.left(95)).trimmed();

                discoveredTopPostsByUrl.insert(post.postUrl, QJsonObject{
                    {QStringLiteral("platformId"), account->getId()},
                    {QStringLiteral("accountUrl"), entry.url},
                    {QStringLiteral("postUrl"), post.postUrl},
                    {QStringLiteral("shortCode"), post.shortCode},
                    {QStringLiteral("caption"), post.caption.trimmed()},
                    {QStringLiteral("hook"), openingHook},
                    {QStringLiteral("hashtags"), QJsonArray::fromStringList(postTags)},
                    {QStringLiteral("views"), post.stats.views},
                    {QStringLiteral("likes"), post.stats.likes},
                    {QStringLiteral("comments"), post.stats.comments},
                    {QStringLiteral("shares"), post.stats.shares},
                    {QStringLiteral("followers"), followers},
                    {QStringLiteral("fetchedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}
                });

                if (post.stats.views >= winnerViewThreshold)
                {
                    if (hashtags)
                    {
                        const QStringList existingTags = hashtags->hashtags(false);
                        for (const QString &wt : postTags)
                        {
                            bool known = false;
                            for (const QString &et : existingTags)
                            {
                                if (et.compare(wt, Qt::CaseInsensitive) == 0)
                                {
                                    known = true;
                                    break;
                                }
                            }
                            if (!known)
                            {
                                hashtags->addHashtag(wt, PreferredHashtags::SOURCE_AI);
                                ++harvestedHashtags;
                            }
                        }
                    }
                    if (favoriteHooks && openingHook.size() >= 15 && openingHook.size() <= 110)
                    {
                        const int beforeCnt = favoriteHooks->rowCount();
                        favoriteHooks->addHook(openingHook, FavoriteHooks::SOURCE_AI);
                        if (favoriteHooks->rowCount() > beforeCnt)
                        {
                            ++harvestedHooks;
                        }
                    }
                }
            }

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
                const bool isPostUrl = !pub.postUrl.isEmpty()
                    && !pub.postUrl.contains(QLatin1Char('#'))
                    && (pub.postUrl.contains(QLatin1String("/video/"), Qt::CaseInsensitive)
                        || pub.postUrl.contains(QLatin1String("/photo/"), Qt::CaseInsensitive)
                        || pub.postUrl.contains(QLatin1String("/pin/"), Qt::CaseInsensitive)
                        || pub.postUrl.contains(QLatin1String("/shorts/"), Qt::CaseInsensitive)
                        || pub.postUrl.contains(QLatin1String("/reel/"), Qt::CaseInsensitive)
                        || pub.postUrl.contains(QLatin1String("watch?v="), Qt::CaseInsensitive));
                if (isPostUrl && !refreshedPostUrls.contains(pub.postUrl))
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

    const VideoTextExperimentSummary videoTextExp = summarizeVideoTextExperiment(videos, favoriteHooks);
    appendRetrieveStatsLog(videoTextExp.humanSummary());
    const ReferenceExperimentSummary refExp = summarizeReferenceExperiment(videos);
    appendRetrieveStatsLog(refExp.humanSummary());

    if (!discoveredTopPostsByUrl.isEmpty()
        || videoTextExp.withTextCount > 0
        || videoTextExp.withoutTextCount > 0
        || refExp.totalMeasuredPosts > 0)
    {
        QList<QJsonObject> sortedPosts = discoveredTopPostsByUrl.values();
        std::sort(sortedPosts.begin(), sortedPosts.end(),
                  [](const QJsonObject &a, const QJsonObject &b) {
                      return a.value(QStringLiteral("views")).toInteger()
                           > b.value(QStringLiteral("views")).toInteger();
                  });
        QJsonArray outArr;
        for (int i = 0; i < qMin<int>(60, sortedPosts.size()); ++i)
        {
            outArr.append(sortedPosts[i]);
        }
        const QDir wd = WorkingDirectoryManager::instance()->workingDir();
        if (wd.exists())
        {
            QSaveFile sf{wd.filePath(QStringLiteral("account_top_posts.json"))};
            if (sf.open(QIODevice::WriteOnly))
            {
                sf.write(QJsonDocument{QJsonObject{
                    {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                    {QStringLiteral("posts"), outArr},
                    {QStringLiteral("videoTextExperiment"), videoTextExp.toJson()},
                    {QStringLiteral("referenceExperiment"), refExp.toJson()}
                }}.toJson(QJsonDocument::Indented));
                sf.commit();
            }
        }
    }
    if (harvestedHashtags > 0 || harvestedHooks > 0)
    {
        appendRetrieveStatsLog(
            QStringLiteral("Harvested %1 winning hashtag(s) and %2 winning hook(s) into Inspiration bank.")
                .arg(harvestedHashtags)
                .arg(harvestedHooks));
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
