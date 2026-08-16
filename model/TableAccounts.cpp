#include <QDir>
#include <QFile>
#include <QTextStream>

#include "accounts/AbstractSocialAccount.h"

#include "TableAccounts.h"

namespace {
const QString COL_SEP = QStringLiteral(",");
}

const QStringList TableAccounts::HEADER{"Type", "Url", "View", "Followers", "Likes"};
const int TableAccounts::IND_TYPE{0};
const int TableAccounts::IND_URL{1};
const int TableAccounts::IND_VIEW{2};
const int TableAccounts::IND_FOLLOWERS{3};
const int TableAccounts::IND_LIKES{4};

TableAccounts::TableAccounts(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
{
    m_filePath = QDir{workingDirectory}.absoluteFilePath(QStringLiteral("social_accounts.csv"));
    _loadFromFile();
}

QVariant TableAccounts::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        return HEADER[section];
    }
    return QVariant{};
}

int TableAccounts::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_listOfVariantList.size();
}

int TableAccounts::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return HEADER.size();
}

QVariant TableAccounts::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DisplayRole || role == Qt::EditRole)
    {
        return m_listOfVariantList[index.row()][index.column()];
    }
    return QVariant{};
}

bool TableAccounts::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::EditRole)
    {
        m_listOfVariantList[index.row()][index.column()] = value;
        _saveInFile();
        emit dataChanged(index, index, {role});
        return true;
    }
    return false;
}

Qt::ItemFlags TableAccounts::flags(const QModelIndex &) const
{
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

void TableAccounts::addAccount()
{
    const auto &allAccounts = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();
    const QString defaultType = allAccounts.isEmpty()
        ? QString{}
        : allAccounts.first()->getName();

    const int row = m_listOfVariantList.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_listOfVariantList << QVariantList{defaultType, QString{}, 0, 0, 0};
    endInsertRows();
    _saveInFile();
}

void TableAccounts::removeAccount(int row)
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return;
    }
    beginRemoveRows(QModelIndex{}, row, row);
    m_listOfVariantList.removeAt(row);
    endRemoveRows();
    _saveInFile();
}

void TableAccounts::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }

    QTextStream stream{&file};
    const auto &lines = stream.readAll().split(QStringLiteral("\n"));
    file.close();

    // First line is the header, skip it.
    for (int i = 1; i < lines.size(); ++i)
    {
        if (lines[i].trimmed().isEmpty())
        {
            continue;
        }
        const auto &elements = lines[i].split(COL_SEP);
        if (elements.size() <= IND_FOLLOWERS)
        {
            continue;
        }
        // Files saved before the "Likes" column existed only have 4 columns;
        // default the missing one to 0 instead of dropping the row.
        const int likes = elements.size() > IND_LIKES ? elements[IND_LIKES].toInt() : 0;
        m_listOfVariantList << QVariantList{
            elements[IND_TYPE]
            , elements[IND_URL]
            , elements[IND_VIEW].toInt()
            , elements[IND_FOLLOWERS].toInt()
            , likes
        };
    }
}

void TableAccounts::_saveInFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }

    QTextStream stream{&file};
    stream << HEADER.join(COL_SEP);
    for (const auto &variantList : m_listOfVariantList)
    {
        QStringList elements;
        for (const auto &value : variantList)
        {
            elements << value.toString();
        }
        stream << "\n" + elements.join(COL_SEP);
    }
    file.close();
}
