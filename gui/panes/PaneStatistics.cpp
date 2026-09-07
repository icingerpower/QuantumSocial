#include "PaneStatistics.h"
#include "ui_PaneStatistics.h"

#include "model/videos/StatisticsScheduler.h"
#include "model/videos/TableVideos.h"

PaneStatistics::PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                               QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneStatistics)
    , m_videos(videos)
    , m_scheduler(scheduler)
{
    ui->setupUi(this);
}

PaneStatistics::~PaneStatistics()
{
    delete ui;
}
