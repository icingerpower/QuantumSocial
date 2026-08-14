#ifndef PANEACCOUNTS_H
#define PANEACCOUNTS_H

#include <QWidget>

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
    void _retrieveStatistics();

private:
    Ui::PaneAccounts *ui;
    TableAccounts *m_model;
};

#endif // PANEACCOUNTS_H
