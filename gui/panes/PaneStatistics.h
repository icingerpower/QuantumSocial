#ifndef PANESTATISTICS_H
#define PANESTATISTICS_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneStatistics; }
QT_END_NAMESPACE

class PaneStatistics : public QWidget
{
    Q_OBJECT

public:
    explicit PaneStatistics(QWidget *parent = nullptr);
    ~PaneStatistics();

private:
    Ui::PaneStatistics *ui;
};

#endif // PANESTATISTICS_H
