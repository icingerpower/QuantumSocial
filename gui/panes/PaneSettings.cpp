#include "PaneSettings.h"
#include "ui_PaneSettings.h"

#include <QHeaderView>

#include "AvailableCliTable.h"
#include "model/videogen/TableGenerationSettings.h"

PaneSettings::PaneSettings(TableGenerationSettings *generationSettings,
                           AvailableCliTable *availableClis,
                           QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneSettings)
    , m_generationSettings(generationSettings)
    , m_availableClis(availableClis ? availableClis : new AvailableCliTable(this))
{
    ui->setupUi(this);

    ui->tableViewClis->setModel(m_availableClis);
    ui->tableViewClis->horizontalHeader()->setStretchLastSection(true);
    ui->tableViewClis->resizeColumnToContents(AvailableCliTable::ColName);

    ui->tableViewGenerationSettings->setModel(m_generationSettings);
    ui->tableViewGenerationSettings->horizontalHeader()->setSectionResizeMode(
        TableGenerationSettings::IND_SETTING, QHeaderView::Stretch);
    ui->tableViewGenerationSettings->resizeColumnToContents(
        TableGenerationSettings::IND_GENERATOR);
}

PaneSettings::~PaneSettings()
{
    delete ui;
}
