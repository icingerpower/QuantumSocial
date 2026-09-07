#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "AbstractVideoGenerator.h"

#include "TableGenerationSettings.h"

const QStringList TableGenerationSettings::HEADER{"Generator", "Setting", "Value"};
const int TableGenerationSettings::IND_GENERATOR{0};
const int TableGenerationSettings::IND_SETTING{1};
const int TableGenerationSettings::IND_VALUE{2};

TableGenerationSettings::TableGenerationSettings(const QString &workingDirectory,
                                                 QObject *parent)
    : QAbstractTableModel(parent)
{
    m_filePath = QDir{workingDirectory}.absoluteFilePath(
        QStringLiteral("video_generation_settings.json"));

    const auto &generators = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    for (auto it = generators.cbegin(); it != generators.cend(); ++it)
    {
        for (const auto &spec : it.value()->availableSettings())
        {
            Row row;
            row.generatorId = it.key();
            row.generatorName = it.value()->getName();
            row.key = spec.key;
            row.label = spec.label;
            row.defaultValue = spec.defaultValue;
            row.value = spec.defaultValue;
            m_rows << row;
        }
    }
    _loadFromFile();
}

QVariant TableGenerationSettings::headerData(int section, Qt::Orientation orientation,
                                             int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        return HEADER[section];
    }
    return QVariant{};
}

int TableGenerationSettings::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_rows.size();
}

int TableGenerationSettings::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return HEADER.size();
}

QVariant TableGenerationSettings::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::EditRole)
    {
        return QVariant{};
    }
    const Row &row = m_rows[index.row()];
    switch (index.column())
    {
    case 0: // IND_GENERATOR
        return row.generatorName;
    case 1: // IND_SETTING
        return row.label;
    case 2: // IND_VALUE
        return row.value;
    default:
        return QVariant{};
    }
}

bool TableGenerationSettings::setData(const QModelIndex &index, const QVariant &value,
                                      int role)
{
    if (role != Qt::EditRole || index.column() != IND_VALUE)
    {
        return false;
    }
    Row &row = m_rows[index.row()];
    // The default's type defines the setting's type: an int setting stays an
    // int whatever the delegate hands back.
    QVariant converted = value;
    if (!converted.convert(row.defaultValue.metaType()))
    {
        return false;
    }
    row.value = converted;
    _saveInFile();
    emit dataChanged(index, index, {role, Qt::DisplayRole});
    return true;
}

Qt::ItemFlags TableGenerationSettings::flags(const QModelIndex &index) const
{
    Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == IND_VALUE)
    {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

QVariantMap TableGenerationSettings::settingsFor(const QString &generatorId) const
{
    QVariantMap settings;
    for (const Row &row : m_rows)
    {
        if (row.generatorId == generatorId)
        {
            settings.insert(row.key, row.value);
        }
    }
    return settings;
}

void TableGenerationSettings::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    for (Row &row : m_rows)
    {
        const QJsonObject generatorObj = root.value(row.generatorId).toObject();
        if (!generatorObj.contains(row.key))
        {
            continue;
        }
        QVariant saved = generatorObj.value(row.key).toVariant();
        if (saved.convert(row.defaultValue.metaType()))
        {
            row.value = saved;
        }
    }
}

void TableGenerationSettings::_saveInFile()
{
    QJsonObject root;
    for (const Row &row : m_rows)
    {
        QJsonObject generatorObj = root.value(row.generatorId).toObject();
        generatorObj.insert(row.key, QJsonValue::fromVariant(row.value));
        root.insert(row.generatorId, generatorObj);
    }
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
    file.close();
}
