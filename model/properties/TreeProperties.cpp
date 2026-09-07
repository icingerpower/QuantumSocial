#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "TreeProperties.h"

namespace {

QJsonObject nodeToJson(const TreeProperties::PropertyNode *node)
{
    QJsonArray children;
    for (const TreeProperties::PropertyNode *child : node->children)
    {
        children << nodeToJson(child);
    }
    return QJsonObject{
        {QStringLiteral("id"), node->id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("name"), node->name},
        {QStringLiteral("prompt"), node->promptFragment},
        {QStringLiteral("created"), node->createdDate.toString(Qt::ISODate)},
        {QStringLiteral("origin"), node->origin},
        {QStringLiteral("children"), children},
    };
}

TreeProperties::PropertyNode *nodeFromJson(const QJsonObject &obj,
                                           TreeProperties::PropertyNode *parent)
{
    auto *node = new TreeProperties::PropertyNode;
    node->id = QUuid::fromString(obj.value(QStringLiteral("id")).toString());
    if (node->id.isNull())
    {
        node->id = QUuid::createUuid();
    }
    node->name = obj.value(QStringLiteral("name")).toString();
    node->promptFragment = obj.value(QStringLiteral("prompt")).toString();
    node->createdDate = QDateTime::fromString(
        obj.value(QStringLiteral("created")).toString(), Qt::ISODate);
    node->origin = obj.value(QStringLiteral("origin")).toString();
    node->parent = parent;
    for (const QJsonValue &childValue : obj.value(QStringLiteral("children")).toArray())
    {
        node->children << nodeFromJson(childValue.toObject(), node);
    }
    return node;
}

TreeProperties::PropertyNode *findById(TreeProperties::PropertyNode *node, const QUuid &id)
{
    if (node->id == id)
    {
        return node;
    }
    for (TreeProperties::PropertyNode *child : node->children)
    {
        if (TreeProperties::PropertyNode *found = findById(child, id))
        {
            return found;
        }
    }
    return nullptr;
}

} // namespace

const QStringList TreeProperties::HEADER{
    "Name", "Prompt", "Origin", "Videos", "Views/Follower", "Engagement",
    "Last used", "Generated", "Stats"};
const int TreeProperties::IND_NAME{0};
const int TreeProperties::IND_PROMPT{1};
const int TreeProperties::IND_ORIGIN{2};
const int TreeProperties::IND_VIDEO_COUNT{3};
const int TreeProperties::IND_NORM_VIEWS{4};
const int TreeProperties::IND_ENGAGEMENT{5};
const int TreeProperties::IND_LAST_USED{6};
const int TreeProperties::IND_GENERATED{7};
const int TreeProperties::IND_STATS{8};

TreeProperties::PropertyNode::~PropertyNode()
{
    qDeleteAll(children);
}

TreeProperties::TreeProperties(const QString &filePath, QObject *parent)
    : QAbstractItemModel(parent)
    , m_filePath(filePath)
    , m_root(std::make_unique<PropertyNode>())
{
    _loadFromFile();
}

TreeProperties::~TreeProperties() = default;

void TreeProperties::setVideosModel(TableVideos *videos)
{
    m_videos = videos;
    if (!m_videos)
    {
        return;
    }
    const auto invalidate = [this]() { _invalidateAggregates(); };
    connect(m_videos, &QAbstractItemModel::dataChanged, this, invalidate);
    connect(m_videos, &QAbstractItemModel::rowsInserted, this, invalidate);
    connect(m_videos, &QAbstractItemModel::rowsRemoved, this, invalidate);
    connect(m_videos, &QAbstractItemModel::modelReset, this, invalidate);
    _invalidateAggregates();
}

QModelIndex TreeProperties::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
    {
        return QModelIndex{};
    }
    PropertyNode *parentNode = _nodeFromIndex(parent);
    return createIndex(row, column, parentNode->children[row]);
}

QModelIndex TreeProperties::parent(const QModelIndex &child) const
{
    if (!child.isValid())
    {
        return QModelIndex{};
    }
    PropertyNode *node = static_cast<PropertyNode *>(child.internalPointer());
    PropertyNode *parentNode = node->parent;
    if (!parentNode || parentNode == m_root.get())
    {
        return QModelIndex{};
    }
    return indexFromNode(parentNode);
}

int TreeProperties::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
    {
        return 0;
    }
    return _nodeFromIndex(parent)->children.size();
}

int TreeProperties::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return HEADER.size();
}

QVariant TreeProperties::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
    {
        return QVariant{};
    }
    PropertyNode *node = static_cast<PropertyNode *>(index.internalPointer());
    const bool isValue = node->parent && node->parent != m_root.get();

    if (role == RoleId)
    {
        return QVariant::fromValue(node->id);
    }
    if (role == RoleIsValue)
    {
        return isValue;
    }
    if (role != Qt::DisplayRole && role != Qt::EditRole)
    {
        return QVariant{};
    }

    switch (index.column())
    {
    case 0: // IND_NAME
        return node->name;
    case 1: // IND_PROMPT
        return node->promptFragment;
    case 2: // IND_ORIGIN
        return node->origin;
    default:
        break;
    }

    // Statistics columns only make sense on testable values, and only for
    // display (they are computed, not edited).
    if (!isValue || role != Qt::DisplayRole)
    {
        return QVariant{};
    }
    const TableVideos::AggregateStats *aggregate = _aggregateForNode(node);
    if (!aggregate)
    {
        return QVariant{};
    }
    switch (index.column())
    {
    case 3: // IND_VIDEO_COUNT
        return aggregate->videoCount > 0 ? QVariant{aggregate->videoCount} : QVariant{};
    case 4: // IND_NORM_VIEWS
        return aggregate->medianNormViews >= 0.0
            ? QVariant{QString::number(aggregate->medianNormViews, 'f', 2)}
            : QVariant{};
    case 5: // IND_ENGAGEMENT
        return aggregate->medianEngagement >= 0.0
            ? QVariant{QStringLiteral("%1 %")
                  .arg(QString::number(aggregate->medianEngagement * 100.0, 'f', 1))}
            : QVariant{};
    case 6: // IND_LAST_USED
        return aggregate->lastUsed.isValid()
            ? QVariant{aggregate->lastUsed.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"))}
            : QVariant{};
    default:
        return QVariant{};
    }
}

bool TreeProperties::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || role != Qt::EditRole)
    {
        return false;
    }
    PropertyNode *node = static_cast<PropertyNode *>(index.internalPointer());
    if (index.column() == IND_NAME)
    {
        node->name = value.toString();
    }
    else if (index.column() == IND_PROMPT)
    {
        node->promptFragment = value.toString();
    }
    else
    {
        return false;
    }
    _saveInFile();
    emit dataChanged(index, index, {role, Qt::DisplayRole});
    return true;
}

Qt::ItemFlags TreeProperties::flags(const QModelIndex &index) const
{
    Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == IND_NAME || index.column() == IND_PROMPT)
    {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

QVariant TreeProperties::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        return HEADER[section];
    }
    return QVariant{};
}

QModelIndex TreeProperties::addProperty(const QString &name, const QString &origin)
{
    auto *node = new PropertyNode;
    node->id = QUuid::createUuid();
    node->name = name;
    node->createdDate = QDateTime::currentDateTimeUtc();
    node->origin = origin;
    node->parent = m_root.get();

    const int row = m_root->children.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_root->children << node;
    endInsertRows();
    _saveInFile();
    return createIndex(row, 0, node);
}

QModelIndex TreeProperties::addValue(const QModelIndex &property, const QString &name,
                                     const QString &promptFragment,
                                     const QString &origin)
{
    if (!property.isValid())
    {
        return QModelIndex{};
    }
    PropertyNode *parentNode = static_cast<PropertyNode *>(property.internalPointer());

    auto *node = new PropertyNode;
    node->id = QUuid::createUuid();
    node->name = name;
    node->promptFragment = promptFragment;
    node->createdDate = QDateTime::currentDateTimeUtc();
    node->origin = origin;
    node->parent = parentNode;

    const int row = parentNode->children.size();
    beginInsertRows(property.siblingAtColumn(0), row, row);
    parentNode->children << node;
    endInsertRows();
    _saveInFile();
    return createIndex(row, 0, node);
}

void TreeProperties::removeNode(const QModelIndex &index)
{
    if (!index.isValid())
    {
        return;
    }
    PropertyNode *node = static_cast<PropertyNode *>(index.internalPointer());
    PropertyNode *parentNode = node->parent;
    const int row = parentNode->children.indexOf(node);
    beginRemoveRows(parent(index), row, row);
    parentNode->children.removeAt(row);
    delete node;
    endRemoveRows();
    _saveInFile();
}

void TreeProperties::archiveTo(TreeProperties &destination, const QModelIndex &index)
{
    if (&destination == this || !index.isValid())
    {
        return;
    }
    PropertyNode *node = static_cast<PropertyNode *>(index.internalPointer());
    PropertyNode *sourceParent = node->parent;
    const bool isValue = sourceParent && sourceParent != m_root.get();

    // Detach from this model (without deleting: the subtree moves).
    const int row = sourceParent->children.indexOf(node);
    beginRemoveRows(parent(index), row, row);
    sourceParent->children.removeAt(row);
    node->parent = nullptr;
    endRemoveRows();
    _saveInFile();

    if (isValue)
    {
        // Keep the value in context: mirror its parent property (same id)
        // at the destination's top level if it is not already there.
        PropertyNode *mirror = findById(destination.m_root.get(), sourceParent->id);
        if (!mirror)
        {
            mirror = new PropertyNode;
            mirror->id = sourceParent->id;
            mirror->name = sourceParent->name;
            mirror->promptFragment = sourceParent->promptFragment;
            mirror->createdDate = sourceParent->createdDate;
            mirror->origin = sourceParent->origin;
            mirror->parent = destination.m_root.get();
            const int mirrorRow = destination.m_root->children.size();
            destination.beginInsertRows(QModelIndex{}, mirrorRow, mirrorRow);
            destination.m_root->children << mirror;
            destination.endInsertRows();
        }
        const int childRow = mirror->children.size();
        destination.beginInsertRows(destination.indexFromNode(mirror), childRow, childRow);
        node->parent = mirror;
        mirror->children << node;
        destination.endInsertRows();
    }
    else
    {
        // A whole property: merge into an existing mirror if one was created
        // by earlier per-value archiving, otherwise move the node itself.
        PropertyNode *mirror = findById(destination.m_root.get(), node->id);
        if (mirror)
        {
            if (!node->children.isEmpty())
            {
                const int firstRow = mirror->children.size();
                destination.beginInsertRows(destination.indexFromNode(mirror),
                    firstRow, firstRow + node->children.size() - 1);
                for (PropertyNode *child : node->children)
                {
                    child->parent = mirror;
                    mirror->children << child;
                }
                node->children.clear();
                destination.endInsertRows();
            }
            delete node;
        }
        else
        {
            const int destRow = destination.m_root->children.size();
            destination.beginInsertRows(QModelIndex{}, destRow, destRow);
            node->parent = destination.m_root.get();
            destination.m_root->children << node;
            destination.endInsertRows();
        }
    }
    destination._saveInFile();
}

TreeProperties::PropertyNode *TreeProperties::nodeFromId(const QUuid &id) const
{
    if (id.isNull())
    {
        return nullptr;
    }
    for (PropertyNode *child : m_root->children)
    {
        if (PropertyNode *found = findById(child, id))
        {
            return found;
        }
    }
    return nullptr;
}

QModelIndex TreeProperties::indexFromNode(PropertyNode *node) const
{
    if (!node || node == m_root.get() || !node->parent)
    {
        return QModelIndex{};
    }
    return createIndex(node->parent->children.indexOf(node), 0, node);
}

QList<TreeProperties::PropertyNode *> TreeProperties::topLevelNodes() const
{
    return m_root->children;
}

TreeProperties::PropertyNode *TreeProperties::_nodeFromIndex(const QModelIndex &index) const
{
    return index.isValid()
        ? static_cast<PropertyNode *>(index.internalPointer())
        : m_root.get();
}

const TableVideos::AggregateStats *TreeProperties::_aggregateForNode(PropertyNode *node) const
{
    if (!m_videos)
    {
        return nullptr;
    }
    auto it = m_aggregateCache.find(node->id);
    if (it == m_aggregateCache.end())
    {
        it = m_aggregateCache.insert(node->id, m_videos->aggregateForValue(node->id));
    }
    return &it.value();
}

void TreeProperties::_invalidateAggregates()
{
    m_aggregateCache.clear();
    _emitAggregatesChanged(QModelIndex{});
}

void TreeProperties::_emitAggregatesChanged(const QModelIndex &parent)
{
    const int rows = rowCount(parent);
    if (rows == 0)
    {
        return;
    }
    emit dataChanged(index(0, IND_VIDEO_COUNT, parent),
                     index(rows - 1, IND_LAST_USED, parent), {Qt::DisplayRole});
    for (int row = 0; row < rows; ++row)
    {
        _emitAggregatesChanged(index(row, 0, parent));
    }
}

void TreeProperties::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    for (const QJsonValue &nodeValue : root.value(QStringLiteral("nodes")).toArray())
    {
        m_root->children << nodeFromJson(nodeValue.toObject(), m_root.get());
    }
}

void TreeProperties::_saveInFile()
{
    QJsonArray nodes;
    for (const PropertyNode *node : m_root->children)
    {
        nodes << nodeToJson(node);
    }
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument{QJsonObject{{QStringLiteral("nodes"), nodes}}}
        .toJson(QJsonDocument::Indented));
    file.close();
}
