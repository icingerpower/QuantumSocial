#include "DialogGenerateAgain.h"
#include "ui_DialogGenerateAgain.h"

DialogGenerateAgain::DialogGenerateAgain(const QString &configuration, QWidget *parent)
    : QDialog(parent), ui(new Ui::DialogGenerateAgain)
{
    ui->setupUi(this);
    ui->labelConfiguration->setText(configuration);
}

DialogGenerateAgain::~DialogGenerateAgain()
{
    delete ui;
}

int DialogGenerateAgain::generationCount() const
{
    return ui->spinCount->value();
}
