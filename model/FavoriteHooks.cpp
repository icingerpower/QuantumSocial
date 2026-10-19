#include "FavoriteHooks.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

const int FavoriteHooks::IND_HOOK;
const int FavoriteHooks::IND_SOURCE;
const QString FavoriteHooks::SOURCE_HUMAN{QStringLiteral("Human")};
const QString FavoriteHooks::SOURCE_AI{QStringLiteral("AI")};
const QStringList FavoriteHooks::HEADER{QStringLiteral("Hook"), QStringLiteral("Human / AI")};

FavoriteHooks::FavoriteHooks(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
    , m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("favorite_hooks.md")))
{
    _loadFromFile();
}

int FavoriteHooks::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return m_items.size();
}

int FavoriteHooks::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return HEADER.size();
}

QVariant FavoriteHooks::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
    {
        return QVariant{};
    }

    if (role == Qt::DisplayRole || role == Qt::EditRole)
    {
        if (index.column() == IND_HOOK)
        {
            return m_items[index.row()].hook;
        }
        if (index.column() == IND_SOURCE)
        {
            return m_items[index.row()].source;
        }
    }
    return QVariant{};
}

bool FavoriteHooks::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size() || role != Qt::EditRole)
    {
        return false;
    }

    if (index.column() == IND_HOOK)
    {
        m_items[index.row()].hook = value.toString().trimmed();
    }
    else if (index.column() == IND_SOURCE)
    {
        m_items[index.row()].source = value.toString();
    }
    else
    {
        return false;
    }

    _saveInFile();
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

QVariant FavoriteHooks::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 && section < HEADER.size())
    {
        return HEADER[section];
    }
    return QVariant{};
}

Qt::ItemFlags FavoriteHooks::flags(const QModelIndex &index) const
{
    if (!index.isValid())
    {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool FavoriteHooks::removeRows(int row, int count, const QModelIndex &parent)
{
    if (row < 0 || row + count > m_items.size() || count <= 0 || parent.isValid())
    {
        return false;
    }

    beginRemoveRows(parent, row, row + count - 1);
    for (int i = 0; i < count; ++i)
    {
        m_items.removeAt(row);
    }
    endRemoveRows();
    _saveInFile();
    return true;
}

QStringList FavoriteHooks::hooks(bool onlyHuman) const
{
    QStringList result;
    result.reserve(m_items.size());
    for (const auto &item : m_items)
    {
        if (!item.hook.isEmpty())
        {
            if (!onlyHuman || item.source.compare(SOURCE_HUMAN, Qt::CaseInsensitive) == 0)
            {
                result << item.hook;
            }
        }
    }
    return result;
}

void FavoriteHooks::setHooks(const QStringList &hooks)
{
    beginResetModel();
    QList<Item> normalized;
    for (const QString &hook : hooks)
    {
        const QString clean = hook.trimmed();
        if (clean.isEmpty())
        {
            continue;
        }
        bool dup = false;
        for (const auto &item : normalized)
        {
            if (item.hook.compare(clean, Qt::CaseInsensitive) == 0)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }

        QString src = SOURCE_HUMAN;
        for (const auto &it : m_items)
        {
            if (it.hook.compare(clean, Qt::CaseInsensitive) == 0)
            {
                src = it.source;
                break;
            }
        }
        normalized.append({clean, src});
    }
    m_items = normalized;
    endResetModel();
    _saveInFile();
}

void FavoriteHooks::addHook(const QString &hook, const QString &source)
{
    const QString clean = hook.trimmed();
    if (clean.isEmpty())
    {
        return;
    }
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].hook.compare(clean, Qt::CaseInsensitive) == 0)
        {
            if (m_items[i].source != source)
            {
                m_items[i].source = source;
                _saveInFile();
                emit dataChanged(index(i, IND_SOURCE), index(i, IND_SOURCE),
                                 {Qt::DisplayRole, Qt::EditRole});
            }
            return;
        }
    }

    const int row = m_items.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_items.append({clean, source});
    endInsertRows();
    _saveInFile();
}

void FavoriteHooks::addRow(const QString &source)
{
    const int row = m_items.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_items.append({QString{}, source});
    endInsertRows();
    _saveInFile();
}

void FavoriteHooks::removeHook(int row)
{
    removeRow(row);
}

QString FavoriteHooks::asPromptSection(bool onlyHuman) const
{
    if (m_items.isEmpty())
    {
        return QString{};
    }
    QStringList bullets;
    for (const auto &item : m_items)
    {
        if (!item.hook.isEmpty())
        {
            if (!onlyHuman || item.source.compare(SOURCE_HUMAN, Qt::CaseInsensitive) == 0)
            {
                bullets << QStringLiteral("- %1").arg(item.hook);
            }
        }
    }
    if (bullets.isEmpty())
    {
        return QString{};
    }
    const QString label = onlyHuman
        ? QStringLiteral(
            "Curated human hooks that performed well before (style/tone/structure inspiration "
            "— write NEW hooks in a similar spirit, do not just repeat these "
            "verbatim):\n%1\n")
        : QStringLiteral(
            "Hooks that performed well before (style/tone/structure inspiration "
            "— write NEW hooks in a similar spirit, do not just repeat these "
            "verbatim):\n%1\n");
    return label.arg(bullets.join(QStringLiteral("\n")));
}

void FavoriteHooks::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    m_items.clear();
    QTextStream stream{&file};
    for (const QString &rawLine : stream.readAll().split(QLatin1Char('\n')))
    {
        const QString trimmed = rawLine.trimmed();
        if (!trimmed.startsWith(QLatin1String("- ")))
        {
            continue;
        }

        QString rest = trimmed.mid(2).trimmed();
        QString source = SOURCE_HUMAN;
        if (rest.startsWith(QLatin1String("[AI]"), Qt::CaseInsensitive))
        {
            source = SOURCE_AI;
            rest = rest.mid(4).trimmed();
        }
        else if (rest.startsWith(QLatin1String("[Human]"), Qt::CaseInsensitive))
        {
            source = SOURCE_HUMAN;
            rest = rest.mid(7).trimmed();
        }

        if (!rest.isEmpty())
        {
            m_items.append({rest, source});
        }
    }
}

void FavoriteHooks::_saveInFile() const
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    QTextStream stream{&file};
    stream << "# Favorite hooks\n\n"
              "Example hooks proven to perform well — harvested from your "
              "top-ranked videos (Statistics tab), or from anywhere else "
              "you've seen a hook that worked — fed to the CLI as style/"
              "tone inspiration when it suggests new hooks. Edit or delete "
              "lines freely.\n\n";
    for (const auto &item : m_items)
    {
        stream << "- [" << item.source << "] " << item.hook << "\n";
    }
}
