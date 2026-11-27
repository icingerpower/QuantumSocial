// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneStatistics.h"
#include "ui_PaneStatistics.h"

#include <QDesktopServices>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "model/TableAccounts.h"
#include "model/TableProjects.h"
#include "model/videos/SocialStatistics.h"
#include "model/videos/StatisticsScheduler.h"
#include "model/videos/TableVideoStatistics.h"
#include "model/videos/TableVideos.h"

PaneStatistics::PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                               QWidget *parent)
    : PaneStatistics(videos, scheduler, nullptr, nullptr, parent)
{
}

PaneStatistics::PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                               TableAccounts *accounts, TableProjects *projects,
                               QWidget *parent,
                               PreferredHashtags *hashtags,
                               FavoriteHooks *favoriteHooks,
                               ResearchStore *researchStore)
    : QWidget(parent)
    , ui(new Ui::PaneStatistics)
    , m_videos(videos)
    , m_scheduler(scheduler)
    , m_accounts(accounts)
    , m_projects(projects)
    , m_hashtags(hashtags)
    , m_favoriteHooks(favoriteHooks)
    , m_researchStore(researchStore)
    , m_model(nullptr)
{
    ui->setupUi(this);

    const QString workDir = WorkingDirectoryManager::instance()->workingDir().absolutePath();
    if (!m_accounts)
    {
        m_accounts = new TableAccounts(workDir, this);
    }
    if (!m_projects)
    {
        m_projects = new TableProjects(workDir, this);
    }

    m_model = new TableVideoStatistics(m_videos, m_projects, this);
    ui->tableViewStatistics->setModel(m_model);
    ui->tableViewStatistics->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableViewStatistics->verticalHeader()->setVisible(false);
    ui->tableViewStatistics->verticalHeader()->setMinimumSectionSize(
        TableVideoStatistics::PREVIEW_HEIGHT + 8);
    ui->tableViewStatistics->verticalHeader()->setDefaultSectionSize(
        TableVideoStatistics::PREVIEW_HEIGHT + 8);
    ui->tableViewStatistics->verticalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);

    QHeaderView *header = ui->tableViewStatistics->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->resizeSection(TableVideoStatistics::IND_PREVIEW, 140);
    header->resizeSection(TableVideoStatistics::IND_NAME, 200);
    header->resizeSection(TableVideoStatistics::IND_TYPE, 110);
    header->resizeSection(TableVideoStatistics::IND_CREATED, 140);
    SocialStatistics::applyColumnWidths(header, TableVideoStatistics::IND_FIRST_STAT);
    header->setStretchLastSection(true);

    ui->tableViewStatistics->setSortingEnabled(true);
    ui->tableViewStatistics->sortByColumn(TableVideoStatistics::IND_VIEWS, Qt::DescendingOrder);

    connect(ui->buttonRetrieve, &QPushButton::clicked, this, [this]() {
        m_retrieveStatisticsTask = _retrieveStatistics();
    });

    auto *btnOpenPost = new QPushButton(tr("Open Post in Browser"), this);
    btnOpenPost->setObjectName(QStringLiteral("buttonOpenStatPostInBrowser"));
    btnOpenPost->setToolTip(tr("Open the selected published post URL (or double-click any row) in your browser."));
    ui->horizontalLayout->insertWidget(1, btnOpenPost);
    connect(btnOpenPost, &QPushButton::clicked, this, &PaneStatistics::_openSelectedPostInBrowser);
    connect(ui->tableViewStatistics, &QTableView::doubleClicked,
            this, [this](const QModelIndex &) { _openSelectedPostInBrowser(); });
}

PaneStatistics::~PaneStatistics()
{
    delete ui;
}

TableVideoStatistics *PaneStatistics::statisticsModel() const
{
    return m_model;
}

QCoro::Task<void> PaneStatistics::_retrieveStatistics()
{
    co_await SocialStatistics::retrieveFromAccounts(
        m_accounts, m_videos, this, m_hashtags, m_favoriteHooks, m_researchStore);
}

void PaneStatistics::_openSelectedPostInBrowser()
{
    if (!m_model || !m_videos)
    {
        return;
    }
    const QModelIndex cur = ui->tableViewStatistics->currentIndex();
    if (!cur.isValid())
    {
        QMessageBox::information(this, tr("No post selected"),
            tr("Select a published post in the Statistics table first."));
        return;
    }
    const QUuid vid = m_model->videoIdAt(cur.row());
    const auto *rec = m_videos->recordFromId(vid);
    if (!rec || rec->publications.isEmpty())
    {
        return;
    }
    QString bestUrl;
    for (const auto &pub : rec->publications)
    {
        if (!pub.postUrl.isEmpty() && !pub.postUrl.contains(QLatin1Char('#')))
        {
            bestUrl = pub.postUrl;
            break;
        }
    }
    if (bestUrl.isEmpty())
    {
        bestUrl = rec->publications.first().postUrl;
    }
    if (!bestUrl.isEmpty())
    {
        QDesktopServices::openUrl(QUrl{bestUrl});
    }
}
