#ifndef PANEACCOUNTS_H
#define PANEACCOUNTS_H

#include <QWidget>

#include <QCoro/QCoroTask>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneAccounts; }
QT_END_NAMESPACE

class TableAccounts;

class PaneAccounts : public QWidget
{
    Q_OBJECT

public:
    explicit PaneAccounts(QWidget *parent = nullptr);
    ~PaneAccounts();

private slots:
    void _addAccount();
    void _removeSelectedAccounts();

private:
    Ui::PaneAccounts *ui;
    TableAccounts *m_model;
    QCoro::Task<void> m_retrieveStatisticsTask;

    QCoro::Task<void> _retrieveStatistics();
};

#endif // PANEACCOUNTS_H
