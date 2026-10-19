#include "PreferredHashtags.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

const int PreferredHashtags::IND_TAG;
const int PreferredHashtags::IND_SOURCE;
const QString PreferredHashtags::SOURCE_HUMAN{QStringLiteral("Human")};
const QString PreferredHashtags::SOURCE_AI{QStringLiteral("AI")};
const QStringList PreferredHashtags::HEADER{QStringLiteral("Hashtag"), QStringLiteral("Human / AI")};

PreferredHashtags::PreferredHashtags(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
    , m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("preferred_hashtags.json")))
{
    _loadFromFile();
}

int PreferredHashtags::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return m_items.size();
}

int PreferredHashtags::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return HEADER.size();
}

QVariant PreferredHashtags::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
    {
        return QVariant{};
    }

    if (role == Qt::DisplayRole || role == Qt::EditRole)
    {
        if (index.column() == IND_TAG)
        {
            return m_items[index.row()].tag;
        }
        if (index.column() == IND_SOURCE)
        {
            return m_items[index.row()].source;
        }
    }
    return QVariant{};
}

bool PreferredHashtags::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size() || role != Qt::EditRole)
    {
        return false;
    }

    if (index.column() == IND_TAG)
    {
        QString clean = value.toString().trimmed();
        if (!clean.isEmpty() && !clean.startsWith(QLatin1Char('#')))
        {
            clean.prepend(QLatin1Char('#'));
        }
        m_items[index.row()].tag = clean;
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

QVariant PreferredHashtags::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 && section < HEADER.size())
    {
        return HEADER[section];
    }
    return QVariant{};
}

Qt::ItemFlags PreferredHashtags::flags(const QModelIndex &index) const
{
    if (!index.isValid())
    {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool PreferredHashtags::removeRows(int row, int count, const QModelIndex &parent)
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

QStringList PreferredHashtags::hashtags(bool onlyHuman) const
{
    QStringList result;
    result.reserve(m_items.size());
    for (const auto &item : m_items)
    {
        if (!item.tag.isEmpty())
        {
            if (!onlyHuman || item.source.compare(SOURCE_HUMAN, Qt::CaseInsensitive) == 0)
            {
                result << item.tag;
            }
        }
    }
    return result;
}

QString PreferredHashtags::asPromptSection(bool onlyHuman) const
{
    const QStringList tags = hashtags(onlyHuman);
    if (tags.isEmpty())
    {
        return QString{};
    }
    const QString label = onlyHuman
        ? QStringLiteral("Curated human preferred hashtags for reference (style, niche, and topic reference — generate new, complementary hashtags in a similar spirit):\n%1\n")
        : QStringLiteral("Existing preferred hashtags for reference (style, niche, and topic reference — generate new, complementary hashtags in a similar spirit):\n%1\n");
    return label.arg(tags.join(QStringLiteral(" ")));
}

void PreferredHashtags::setHashtags(const QStringList &hashtags)
{
    beginResetModel();
    QList<Item> normalized;
    QSet<QString> seen;
    for (const QString &tag : hashtags)
    {
        QString clean = tag.trimmed();
        if (clean.isEmpty())
        {
            continue;
        }
        if (!clean.startsWith(QLatin1Char('#')))
        {
            clean.prepend(QLatin1Char('#'));
        }
        const QString key = clean.toLower();
        if (seen.contains(key))
        {
            continue;
        }
        seen.insert(key);

        QString src = SOURCE_HUMAN;
        for (const auto &it : m_items)
        {
            if (it.tag.compare(clean, Qt::CaseInsensitive) == 0)
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

void PreferredHashtags::addHashtag(const QString &tag, const QString &source)
{
    QString clean = tag.trimmed();
    if (clean.isEmpty())
    {
        return;
    }
    if (!clean.startsWith(QLatin1Char('#')))
    {
        clean.prepend(QLatin1Char('#'));
    }
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].tag.compare(clean, Qt::CaseInsensitive) == 0)
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

void PreferredHashtags::addRow(const QString &source)
{
    const int row = m_items.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_items.append({QStringLiteral("#"), source});
    endInsertRows();
    _saveInFile();
}

void PreferredHashtags::removeHashtag(int row)
{
    removeRow(row);
}

QStringList PreferredHashtags::parse(const QString &freeformText)
{
    static const QRegularExpression separators{QStringLiteral("[\\s,]+")};
    return freeformText.split(separators, Qt::SkipEmptyParts);
}

void PreferredHashtags::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    m_items.clear();
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : array)
    {
        if (value.isString())
        {
            const QString tag = value.toString().trimmed();
            if (!tag.isEmpty())
            {
                m_items.append({tag, SOURCE_HUMAN});
            }
        }
        else if (value.isObject())
        {
            const QJsonObject obj = value.toObject();
            const QString tag = obj[QStringLiteral("tag")].toString().trimmed();
            const QString source = obj[QStringLiteral("source")].toString(SOURCE_HUMAN);
            if (!tag.isEmpty())
            {
                m_items.append({tag, source});
            }
        }
    }
}

void PreferredHashtags::_saveInFile() const
{
    QJsonArray array;
    for (const auto &item : m_items)
    {
        QJsonObject obj;
        obj[QStringLiteral("tag")] = item.tag;
        obj[QStringLiteral("source")] = item.source;
        array.append(obj);
    }
    QFile file{m_filePath};
    if (file.open(QFile::WriteOnly))
    {
        file.write(QJsonDocument(array).toJson());
    }
}
