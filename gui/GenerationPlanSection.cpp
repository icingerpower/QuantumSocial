#include "GenerationPlanSection.h"
#include "ui_GenerationPlanSection.h"

#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

GenerationPlanSection::GenerationPlanSection(QWidget *parent)
    : QGroupBox(parent)
    , ui(new Ui::GenerationPlanSection)
{
    ui->setupUi(this);
}

GenerationPlanSection::~GenerationPlanSection()
{
    delete ui;
}

QWidget *GenerationPlanSection::cliRow() const { return ui->cliRow; }
QComboBox *GenerationPlanSection::cliCombo() const { return ui->comboCli; }
QStackedWidget *GenerationPlanSection::strategyStack() const { return ui->stackStrategies; }

QList<QRadioButton *> GenerationPlanSection::strategyButtons() const
{
    return {ui->radioAnimate, ui->radioReimagine, ui->radioCreative};
}

QComboBox *GenerationPlanSection::savedPromptCombo(int strategy) const
{
    return QList<QComboBox *>{ui->comboSaved0, ui->comboSaved1, ui->comboSaved2}.value(strategy);
}

QPushButton *GenerationPlanSection::loadPromptButton(int strategy) const
{
    return QList<QPushButton *>{ui->buttonLoad0, ui->buttonLoad1, ui->buttonLoad2}.value(strategy);
}

QPushButton *GenerationPlanSection::savePromptButton(int strategy) const
{
    return QList<QPushButton *>{ui->buttonSave0, ui->buttonSave1, ui->buttonSave2}.value(strategy);
}

QPlainTextEdit *GenerationPlanSection::promptEdit(int strategy) const
{
    return QList<QPlainTextEdit *>{ui->editPrompt0, ui->editPrompt1, ui->editPrompt2}.value(strategy);
}

QLabel *GenerationPlanSection::propertyLabel(int strategy) const
{
    return QList<QLabel *>{ui->labelProperties0, ui->labelProperties1,
                           ui->labelProperties2}.value(strategy);
}

QScrollArea *GenerationPlanSection::propertyScroll(int strategy) const
{
    return QList<QScrollArea *>{ui->scrollProperties0, ui->scrollProperties1,
                                ui->scrollProperties2}.value(strategy);
}

QVBoxLayout *GenerationPlanSection::propertyLayout(int strategy) const
{
    return QList<QVBoxLayout *>{ui->layoutProperties0, ui->layoutProperties1,
                                ui->layoutProperties2}.value(strategy);
}

QLabel *GenerationPlanSection::previewLabel(int strategy) const
{
    return QList<QLabel *>{ui->labelPreview0, ui->labelPreview1,
                           ui->labelPreview2}.value(strategy);
}
