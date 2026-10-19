// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneProperties.h"
#include "ui_PaneProperties.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextStream>
#include <QTreeView>
#include <QVBoxLayout>

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "model/TableAccounts.h"
#include "model/accounts/AbstractSocialAccount.h"
#include "model/properties/TreeProperties.h"
#include "model/videos/TableVideos.h"

namespace {

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

bool hasAnyVideoStat(const AbstractSocialAccount::VideoStatistics &stats)
{
    return stats.views >= 0 || stats.likes >= 0 || stats.comments >= 0
        || stats.shares >= 0 || stats.impressions >= 0
        || stats.watchTimeSeconds >= 0 || stats.avgWatchPercent >= 0.0
        || stats.followsFromVideo >= 0;
}

AbstractSocialAccount *resolveSocialAccount(const QString &typeOrId)
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

const TableVideos::VideoRecord *matchVideoRecord(
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
            QStringLiteral(R"((?:#\s*qs[-_]?|\bqs-)([0-9a-hjkmnp-tv-z]{4,})\b)"),
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

} // namespace

PaneProperties::PaneProperties(TreeProperties *properties, TreeProperties *archive,
                               QWidget *parent)
    : PaneProperties(properties, archive, nullptr, nullptr, parent)
{
}

PaneProperties::PaneProperties(TreeProperties *properties, TreeProperties *archive,
                               TableVideos *videos, TableAccounts *accounts,
                               QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneProperties)
    , m_properties(properties)
    , m_archive(archive)
    , m_videos(videos)
    , m_accounts(accounts)
{
    ui->setupUi(this);

    if (!m_accounts)
    {
        m_accounts = new TableAccounts(
            WorkingDirectoryManager::instance()->workingDir().absolutePath(), this);
    }

    auto *layout = new QVBoxLayout{this};
    auto *label = new QLabel{
        tr("The A/B property catalog — statistics accumulate as generated "
           "videos are published and their stats fetched. \"Origin\" shows "
           "whether a property/value was added manually, suggested by the "
           "CLI, or grounded in analyzed reference images (see \"Bootstrap "
           "from reference images...\" on the Generation page's properties "
           "list, or the working-directory setup step)."), this};
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *splitter = new QSplitter{Qt::Vertical, this};

    auto *activeGroup = new QGroupBox{tr("Active properties"), splitter};
    auto *activeLayout = new QVBoxLayout{activeGroup};
    auto *activeButtonRow = new QHBoxLayout{};
    auto *buttonAddProperty = new QPushButton{tr("Add property..."), activeGroup};
    auto *buttonAddValue = new QPushButton{tr("Add value..."), activeGroup};
    auto *buttonArchive = new QPushButton{tr("Archive"), activeGroup};
    m_buttonRetrieveStatistics = new QPushButton{tr("Retrieve statistics"), activeGroup};
    m_buttonRetrieveStatistics->setObjectName(QStringLiteral("buttonRetrieveStatistics"));
    connect(buttonAddProperty, &QPushButton::clicked, this, &PaneProperties::_addProperty);
    connect(buttonAddValue, &QPushButton::clicked, this, &PaneProperties::_addValue);
    connect(buttonArchive, &QPushButton::clicked, this, &PaneProperties::_archiveSelected);
    connect(m_buttonRetrieveStatistics, &QPushButton::clicked, this, [this]() {
        m_retrieveStatisticsTask = _retrieveStatistics();
    });
    activeButtonRow->addWidget(buttonAddProperty);
    activeButtonRow->addWidget(buttonAddValue);
    activeButtonRow->addWidget(buttonArchive);
    activeButtonRow->addWidget(m_buttonRetrieveStatistics);
    activeButtonRow->addStretch(1);
    activeLayout->addLayout(activeButtonRow);
    m_treeActive = new QTreeView{activeGroup};
    activeLayout->addWidget(m_treeActive);
    splitter->addWidget(activeGroup);

    auto *archiveGroup = new QGroupBox{tr("Archived properties"), splitter};
    auto *archiveLayout = new QVBoxLayout{archiveGroup};
    auto *archiveButtonRow = new QHBoxLayout{};
    auto *buttonDelete = new QPushButton{tr("Delete permanently"), archiveGroup};
    connect(buttonDelete, &QPushButton::clicked, this, &PaneProperties::_deleteArchived);
    archiveButtonRow->addWidget(buttonDelete);
    archiveButtonRow->addStretch(1);
    archiveLayout->addLayout(archiveButtonRow);
    m_treeArchive = new QTreeView{archiveGroup};
    archiveLayout->addWidget(m_treeArchive);
    splitter->addWidget(archiveGroup);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    _setupTree(m_treeActive, m_properties);
    _setupTree(m_treeArchive, m_archive);
}

PaneProperties::~PaneProperties()
{
    delete ui;
}

void PaneProperties::_setupTree(QTreeView *tree, TreeProperties *model)
{
    tree->setModel(model);
    tree->expandAll();
    // ResizeToContents/Stretch sections cannot be dragged by the user at
    // all — Interactive for every column (with sensible starting widths),
    // plus stretchLastSection so the last VISIBLE column still absorbs
    // extra space, keeps every column both sensibly sized AND resizable.
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->setColumnWidth(TreeProperties::IND_NAME, 180);
    tree->setColumnWidth(TreeProperties::IND_PROMPT, 260);
    tree->setColumnWidth(TreeProperties::IND_ORIGIN, 95);
    tree->setColumnWidth(TreeProperties::IND_VIDEO_COUNT, 65);
    tree->setColumnWidth(TreeProperties::IND_VIEWS, 80);
    tree->setColumnWidth(TreeProperties::IND_LIKES, 75);
    tree->setColumnWidth(TreeProperties::IND_COMMENTS, 85);
    tree->setColumnWidth(TreeProperties::IND_SHARES, 100);
    tree->setColumnWidth(TreeProperties::IND_NORM_VIEWS, 110);
    tree->setColumnWidth(TreeProperties::IND_ENGAGEMENT, 95);
    tree->header()->setStretchLastSection(true);
    tree->setColumnHidden(TreeProperties::IND_GENERATED, true);
    tree->setColumnHidden(TreeProperties::IND_STATS, true);
    QObject::connect(model, &TreeProperties::modelReset, tree, &QTreeView::expandAll);
    QObject::connect(model, &TreeProperties::rowsInserted, tree, &QTreeView::expandAll);
}

QCoro::Task<void> PaneProperties::_retrieveStatistics()
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
    if (m_accounts)
    {
        for (int row = 0; row < m_accounts->rowCount(); ++row)
        {
            const QString typeName
                = m_accounts->data(m_accounts->index(row, TableAccounts::IND_TYPE)).toString().trimmed();
            const QString url
                = m_accounts->data(m_accounts->index(row, TableAccounts::IND_URL)).toString().trimmed();
            const qint64 followers
                = m_accounts->data(m_accounts->index(row, TableAccounts::IND_FOLLOWERS)).toLongLong();
            if (!url.isEmpty())
            {
                accountRows << AccountRowInfo{row, typeName, url, followers};
            }
        }
    }

    if (accountRows.isEmpty())
    {
        appendRetrieveStatsLog(QStringLiteral("No accounts registered in TableAccounts; aborting."));
        QMessageBox::information(this, tr("No account"),
            tr("Add at least one account in the Accounts tab first."));
        co_return;
    }

    QStringList knownShortCodes;
    if (m_videos)
    {
        for (int row = 0; row < m_videos->rowCount(); ++row)
        {
            if (const auto *record = m_videos->recordAt(row))
            {
                if (!record->shortCode.isEmpty())
                {
                    knownShortCodes << record->shortCode;
                }
            }
        }
    }
    appendRetrieveStatsLog(QStringLiteral("Accounts to query: %1 | Known video shortCodes in TableVideos: %2")
                               .arg(accountRows.size())
                               .arg(knownShortCodes.size()));

    setEnabled(false);
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
            const QString msg = tr("%1: unknown account type \"%2\"").arg(entry.url, entry.typeName);
            appendRetrieveStatsLog(QStringLiteral("ERROR: ") + msg);
            errors << msg;
            continue;
        }

        if (!account->supportsAccountVideoStatistics())
        {
            const QString msg = tr("%1 (%2): retrieving published video statistics is not supported yet.")
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
            const QString msg = tr("%1 (%2): %3")
                .arg(account->getName(), entry.url,
                     accountResult.errorMessage.isEmpty()
                         ? tr("retrieving published video statistics is not supported yet.")
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
        if (accountResult.followers >= 0 && m_accounts)
        {
            m_accounts->setData(m_accounts->index(entry.row, TableAccounts::IND_FOLLOWERS),
                                accountResult.followers);
            followers = accountResult.followers;
        }
        if (accountResult.views >= 0 && m_accounts)
        {
            m_accounts->setData(m_accounts->index(entry.row, TableAccounts::IND_VIEW),
                                accountResult.views);
        }
        if (accountResult.likes >= 0 && m_accounts)
        {
            m_accounts->setData(m_accounts->index(entry.row, TableAccounts::IND_LIKES),
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
            const QString warnMsg = tr("%1: 0 published videos were returned (see %2 for details).")
                .arg(entry.url, primaryRetrieveStatsLogPath());
            appendRetrieveStatsLog(QStringLiteral("WARNING: ") + warnMsg);
            errors << warnMsg;
        }

        if (!m_videos)
        {
            appendRetrieveStatsLog(QStringLiteral("WARNING: m_videos is null; cannot match posts to videos."));
            continue;
        }

        for (const auto &post : accountResult.posts)
        {
            const TableVideos::VideoRecord *record = matchVideoRecord(m_videos, post);
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

            m_videos->addPublication(videoId, account->getId(), postUrl,
                                     publishDate, followers > 0 ? followers : -1);
            if (hasAnyVideoStat(post.stats))
            {
                m_videos->addSnapshot(videoId, postUrl, post.source, post.stats);
            }
            refreshedPostUrls.insert(postUrl);
            ++totalMatchedVideos;
        }
    }

    // Also refresh any already-attached publication URLs on published videos
    // that were not covered by the profile page scan above.
    if (m_videos)
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
        for (int row = 0; row < m_videos->rowCount(); ++row)
        {
            const TableVideos::VideoRecord *record = m_videos->recordAt(row);
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
                m_videos->addPublication(target.videoId, account->getId(), target.postUrl,
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
                m_videos->addSnapshot(target.videoId, target.postUrl,
                                      AbstractSocialAccount::StatSource::PublicView, stats);
            }
        }
    }

    appendRetrieveStatsLog(
        QStringLiteral("=== Retrieve statistics finished: matchedVideos=%1, unsupported=%2, errors=%3 ===")
            .arg(totalMatchedVideos)
            .arg(unsupportedMessages.size())
            .arg(errors.size()));

    setEnabled(true);

    if (!unsupportedMessages.isEmpty())
    {
        QMessageBox::information(this, tr("Not supported yet"),
            unsupportedMessages.join(QStringLiteral("\n")));
    }
    if (!errors.isEmpty())
    {
        QMessageBox::warning(this, tr("Some statistics could not be retrieved"),
            errors.join(QStringLiteral("\n")));
    }
}

void PaneProperties::_addProperty()
{
    const QString name = QInputDialog::getText(this, tr("Add property"),
        tr("Property name (e.g. \"Background\"):"));
    if (name.trimmed().isEmpty())
    {
        return;
    }
    m_treeActive->setCurrentIndex(m_properties->addProperty(name.trimmed()));
}

void PaneProperties::_addValue()
{
    QModelIndex property = m_treeActive->currentIndex().siblingAtColumn(0);
    if (!property.isValid())
    {
        QMessageBox::information(this, tr("No property selected"),
            tr("Select a property (or one of its values) in the active "
               "list first."));
        return;
    }
    if (property.data(TreeProperties::RoleIsValue).toBool())
    {
        property = property.parent();
    }
    const QString name = QInputDialog::getText(this, tr("Add value"),
        tr("Value name (e.g. \"Street background\"):"));
    if (name.trimmed().isEmpty())
    {
        return;
    }
    const QString fragment = QInputDialog::getMultiLineText(this,
        tr("Add value"), tr("Prompt fragment injected into generation:"));
    const QModelIndex value
        = m_properties->addValue(property, name.trimmed(), fragment.trimmed());
    m_treeActive->expand(property);
    m_treeActive->setCurrentIndex(value);
}

void PaneProperties::_archiveSelected()
{
    const QModelIndex index = m_treeActive->currentIndex().siblingAtColumn(0);
    if (!index.isValid())
    {
        QMessageBox::information(this, tr("Nothing selected"),
            tr("Select a property or value in the active list first."));
        return;
    }
    m_properties->archiveTo(*m_archive, index);
}

void PaneProperties::_deleteArchived()
{
    const QModelIndex index = m_treeArchive->currentIndex().siblingAtColumn(0);
    if (!index.isValid())
    {
        QMessageBox::information(this, tr("Nothing selected"),
            tr("Select a property or value in the archived list first."));
        return;
    }
    if (QMessageBox::question(this, tr("Delete?"),
            tr("Delete \"%1\" permanently? This cannot be undone.")
                .arg(index.data().toString()))
        == QMessageBox::Yes)
    {
        m_archive->removeNode(index);
    }
}
