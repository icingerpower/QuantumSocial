// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneStatistics.h"
#include "ui_PaneStatistics.h"

#include <QHeaderView>
#include <QPushButton>

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
                               QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneStatistics)
    , m_videos(videos)
    , m_scheduler(scheduler)
    , m_accounts(accounts)
    , m_projects(projects)
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
    co_await SocialStatistics::retrieveFromAccounts(m_accounts, m_videos, this);
}
