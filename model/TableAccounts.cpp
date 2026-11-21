#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSaveFile>

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
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 && section < HEADER.size())
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
    if (index.isValid() && index.row() < rowCount() && index.column() < columnCount()
        && (role == Qt::DisplayRole || role == Qt::EditRole))
    {
        return m_listOfVariantList[index.row()][index.column()];
    }
    return QVariant{};
}

bool TableAccounts::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (index.isValid() && index.row() < rowCount() && index.column() < columnCount()
        && role == Qt::EditRole)
    {
        const QVariant previous = m_listOfVariantList[index.row()][index.column()];
        if (index.column() <= IND_URL && (value.toString().contains(',') || value.toString().contains('\n') || value.toString().contains('\r')))
            return false;
        m_listOfVariantList[index.row()][index.column()] = value;
        if (!_saveInFile())
        {
            m_listOfVariantList[index.row()][index.column()] = previous;
            return false;
        }
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
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
    m_accountIds << QUuid::createUuid();
    endInsertRows();
    if (!_saveInFile())
    {
        beginRemoveRows({}, row, row);
        m_listOfVariantList.removeLast();
        m_accountIds.removeLast();
        endRemoveRows();
    }
}

void TableAccounts::removeAccount(int row)
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return;
    }
    const auto previous = m_listOfVariantList[row];
    const auto id = m_accountIds[row];
    beginRemoveRows(QModelIndex{}, row, row);
    m_listOfVariantList.removeAt(row);
    m_accountIds.removeAt(row);
    endRemoveRows();
    if (!_saveInFile())
    {
        beginInsertRows({}, row, row);
        m_listOfVariantList.insert(row, previous);
        m_accountIds.insert(row, id);
        endInsertRows();
    }
}

QUuid TableAccounts::accountId(int row) const
{
    return row >= 0 && row < m_accountIds.size() ? m_accountIds[row] : QUuid{};
}

int TableAccounts::rowOfId(const QUuid &id) const
{
    return m_accountIds.indexOf(id);
}

void TableAccounts::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        if (file.exists()) { m_readOnly = true; m_lastError = file.errorString(); }
        return;
    }

    QTextStream stream{&file};
    const auto &lines = stream.readAll().split(QStringLiteral("\n"));
    file.close();

    bool needsIds = false;
    // The optional trailing Id preserves the existing five visible columns.
    for (int i = 1; i < lines.size(); ++i)
    {
        if (lines[i].trimmed().isEmpty())
        {
            continue;
        }
        const auto &elements = lines[i].split(COL_SEP);
        if (elements.size() <= IND_FOLLOWERS)
        {
            m_readOnly = true;
            m_lastError = tr("Account file contains an incomplete row. It has been left untouched.");
            return;
        }
        // Files saved before the "Likes" column existed only have 4 columns;
        // default the missing one to 0 instead of dropping the row.
        const qint64 likes = elements.size() > IND_LIKES ? elements[IND_LIKES].toLongLong() : 0;
        m_listOfVariantList << QVariantList{
            elements[IND_TYPE]
            , elements[IND_URL]
            , elements[IND_VIEW].toLongLong()
            , elements[IND_FOLLOWERS].toLongLong()
            , likes
        };
        QUuid id(elements.value(5));
        if (id.isNull() || m_accountIds.contains(id))
        {
            id = QUuid::createUuid();
            needsIds = true;
        }
        m_accountIds << id;
    }
    if (needsIds) _saveInFile();
}

bool TableAccounts::_saveInFile()
{
    if (m_readOnly) { emit persistenceError(m_lastError); return false; }
    QSaveFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        m_lastError = file.errorString();
        emit persistenceError(m_lastError);
        return false;
    }

    QTextStream stream{&file};
    stream << HEADER.join(COL_SEP) << ",Id";
    for (int row = 0; row < m_listOfVariantList.size(); ++row)
    {
        QStringList elements;
        for (const auto &value : m_listOfVariantList[row])
        {
            elements << value.toString();
        }
        elements << m_accountIds[row].toString(QUuid::WithoutBraces);
        stream << "\n" + elements.join(COL_SEP);
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit())
    {
        m_lastError = file.errorString();
        emit persistenceError(m_lastError);
        return false;
    }
    m_lastError.clear();
    return true;
}
