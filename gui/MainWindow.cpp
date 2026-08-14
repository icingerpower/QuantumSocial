#include "MainWindow.h"
#include "./ui_MainWindow.h"
#include "panes/PaneAccounts.h"
#include "panes/PaneGeneration.h"
#include "panes/PaneStatistics.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->tabWidget->addTab(new PaneAccounts(this), tr("Accounts"));
    ui->tabWidget->addTab(new PaneGeneration(this), tr("Generation"));
    ui->tabWidget->addTab(new PaneStatistics(this), tr("Statistics"));
}

MainWindow::~MainWindow()
{
    delete ui;
}
