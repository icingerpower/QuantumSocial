// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

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
    connect(ui->buttonRetrieveStatistics, &QPushButton::clicked, this, [this]() {
        m_retrieveStatisticsTask = _retrieveStatistics();
    });
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

QCoro::Task<void> PaneAccounts::_retrieveStatistics()
{
    const auto &selRows = ui->tableViewSocialAccount->selectionModel()->selectedRows();
    QList<int> rows;
    if (selRows.isEmpty())
    {
        for (int row = 0; row < m_model->rowCount(); ++row)
        {
            rows << row;
        }
    }
    else
    {
        for (const auto &index : selRows)
        {
            rows << index.row();
        }
    }

    if (rows.isEmpty())
    {
        QMessageBox::information(this, tr("No account"),
            tr("Add at least one account first."));
        co_return;
    }

    setEnabled(false);
    QStringList errors;
    const auto &allAccounts = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();

    for (int row : rows)
    {
        const QString &typeName = m_model->data(m_model->index(row, TableAccounts::IND_TYPE)).toString();
        const QString &url = m_model->data(m_model->index(row, TableAccounts::IND_URL)).toString();
        if (url.isEmpty())
        {
            continue;
        }

        AbstractSocialAccount *account = nullptr;
        for (auto *candidate : allAccounts)
        {
            if (candidate->getName() == typeName)
            {
                account = candidate;
                break;
            }
        }
        if (!account)
        {
            errors << tr("%1: unknown account type \"%2\"").arg(url, typeName);
            continue;
        }

        const auto stats = co_await account->fetchStatistics(url);
        if (stats.followers >= 0)
        {
            m_model->setData(m_model->index(row, TableAccounts::IND_FOLLOWERS), stats.followers);
        }
        if (stats.views >= 0)
        {
            m_model->setData(m_model->index(row, TableAccounts::IND_VIEW), stats.views);
        }
        if (stats.likes >= 0)
        {
            m_model->setData(m_model->index(row, TableAccounts::IND_LIKES), stats.likes);
        }
        if (!stats.errorMessage.isEmpty())
        {
            errors << QStringLiteral("%1: %2").arg(url, stats.errorMessage);
        }
    }
    setEnabled(true);

    if (!errors.isEmpty())
    {
        QMessageBox::warning(this, tr("Some statistics could not be retrieved"),
            errors.join(QStringLiteral("\n")));
    }
}
