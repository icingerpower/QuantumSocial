#ifndef PANESTATISTICS_H
#define PANESTATISTICS_H

#include <QWidget>

#include <QCoro/QCoroTask>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneStatistics; }
QT_END_NAMESPACE

class StatisticsScheduler;
class TableAccounts;
class TableProjects;
class TableVideos;
class TableVideoStatistics;

class PaneStatistics : public QWidget
{
    Q_OBJECT

public:
    explicit PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                            QWidget *parent = nullptr);
    explicit PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                            TableAccounts *accounts, TableProjects *projects = nullptr,
                            QWidget *parent = nullptr);
    ~PaneStatistics();

    TableVideoStatistics *statisticsModel() const;

private:
    Ui::PaneStatistics *ui;
    TableVideos *m_videos;
    StatisticsScheduler *m_scheduler;
    TableAccounts *m_accounts;
    TableProjects *m_projects;
    TableVideoStatistics *m_model;
    QCoro::Task<void> m_retrieveStatisticsTask;

    QCoro::Task<void> _retrieveStatistics();
};

#endif // PANESTATISTICS_H
