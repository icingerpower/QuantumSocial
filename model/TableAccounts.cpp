#include "TableAccounts.h"

TableAccounts::TableAccounts(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int TableAccounts::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0;
}

int TableAccounts::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 0;
}

QVariant TableAccounts::data(const QModelIndex &index, int role) const
{
    Q_UNUSED(index);
    Q_UNUSED(role);
    return QVariant();
}
