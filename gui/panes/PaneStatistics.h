#ifndef PANESTATISTICS_H
#define PANESTATISTICS_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneStatistics; }
QT_END_NAMESPACE

class TableVideos;
class StatisticsScheduler;

// Receives the shared models; the widgets and their wiring are defined in
// the .ui file / by hand depending on the wanted workflow. The scheduler
// exposes checkNow() / fetchVideoNow() and the runStarted()/runFinished()
// signals for whatever controls end up here.
class PaneStatistics : public QWidget
{
    Q_OBJECT

public:
    explicit PaneStatistics(TableVideos *videos, StatisticsScheduler *scheduler,
                            QWidget *parent = nullptr);
    ~PaneStatistics();

private:
    Ui::PaneStatistics *ui;
    TableVideos *m_videos;
    StatisticsScheduler *m_scheduler;
};

#endif // PANESTATISTICS_H
