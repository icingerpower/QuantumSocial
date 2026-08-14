#include "PaneStatistics.h"
#include "ui_PaneStatistics.h"

PaneStatistics::PaneStatistics(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneStatistics)
{
    ui->setupUi(this);
}

PaneStatistics::~PaneStatistics()
{
    delete ui;
}
