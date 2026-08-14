#include "PaneAccounts.h"
#include "ui_PaneAccounts.h"

#include <algorithm>

#include <QHeaderView>
#include <QMessageBox>

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "../ComboBoxColumnDelegate.h"
#include "model/TableAccounts.h"
#include "model/accounts/AbstractSocialAccount.h"

PaneAccounts::PaneAccounts(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneAccounts)
    , m_model(nullptr)
{
    ui->setupUi(this);

    m_model = new TableAccounts(
        WorkingDirectoryManager::instance()->workingDir().absolutePath(), this);
    ui->tableViewSocialAccount->setModel(m_model);
    ui->tableViewSocialAccount->horizontalHeader()->setStretchLastSection(true);
    ui->tableViewSocialAccount->horizontalHeader()->resizeSection(TableAccounts::IND_URL, 300);

    QStringList typeNames;
    const auto &allAccounts = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();
    for (auto *account : allAccounts)
    {
        typeNames << account->getName();
    }
    auto *delegate = new ComboBoxColumnDelegate(
        {{TableAccounts::IND_TYPE, typeNames}}, this);
    ui->tableViewSocialAccount->setItemDelegate(delegate);

    connect(ui->buttonAdd, &QPushButton::clicked, this, &PaneAccounts::_addAccount);
    connect(ui->buttonRemove, &QPushButton::clicked, this, &PaneAccounts::_removeSelectedAccounts);
    connect(ui->buttonRetrieveStatistics, &QPushButton::clicked, this, &PaneAccounts::_retrieveStatistics);
}

PaneAccounts::~PaneAccounts()
{
    delete ui;
}

void PaneAccounts::_addAccount()
{
    m_model->addAccount();
}

void PaneAccounts::_removeSelectedAccounts()
{
    const auto &selRows = ui->tableViewSocialAccount->selectionModel()->selectedRows();
    if (selRows.isEmpty())
    {
        QMessageBox::information(this, tr("No selection"),
            tr("Select the account(s) to remove in the table first."));
        return;
    }

    QList<int> rows;
    for (const auto &index : selRows)
    {
        rows << index.row();
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows)
    {
        m_model->removeAccount(row);
    }
}

void PaneAccounts::_retrieveStatistics()
{
    QMessageBox::information(this, tr("Not implemented"),
        tr("Retrieving statistics is not implemented yet."));
}
