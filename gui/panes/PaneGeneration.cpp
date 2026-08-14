#include "PaneGeneration.h"
#include "ui_PaneGeneration.h"

PaneGeneration::PaneGeneration(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneGeneration)
{
    ui->setupUi(this);
}

PaneGeneration::~PaneGeneration()
{
    delete ui;
}
