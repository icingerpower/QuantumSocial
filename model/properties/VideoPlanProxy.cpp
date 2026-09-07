#include "TreeProperties.h"

#include "VideoPlanProxy.h"

VideoPlanProxy::VideoPlanProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
}

void VideoPlanProxy::setVideo(TableVideos *videos, const QUuid &videoId)
{
    if (m_videos != videos)
    {
        if (m_videos)
        {
            disconnect(m_videos, nullptr, this, nullptr);
        }
        m_videos = videos;
        if (m_videos)
        {
            // A snapshot arriving (or a checkbox toggled through another
            // view) must refresh the check columns of this plan.
            connect(m_videos, &QAbstractItemModel::dataChanged, this,
                    [this]() { _emitChecksChanged(QModelIndex{}); });
        }
    }

    m_videoId = videoId;
    m_plannedIds.clear();
    if (m_videos)
    {
        if (const TableVideos::VideoRecord *record = m_videos->recordFromId(videoId))
        {
            m_plannedIds = QSet<QUuid>{record->propertyValueIds.begin(),
                                       record->propertyValueIds.end()};
        }
    }
    invalidateFilter();
    _emitChecksChanged(QModelIndex{});
}

QUuid VideoPlanProxy::videoId() const
{
    return m_videoId;
}

QVariant VideoPlanProxy::data(const QModelIndex &index, int role) const
{
    if (_isCheckColumn(index.column()))
    {
        if (role != Qt::CheckStateRole || !m_videos)
        {
            return QVariant{};
        }
        const QModelIndex nameIndex = index.siblingAtColumn(TreeProperties::IND_NAME);
        if (!nameIndex.data(TreeProperties::RoleIsValue).toBool())
        {
            return QVariant{};
        }
        const QUuid valueId = nameIndex.data(TreeProperties::RoleId).toUuid();
        const auto which = index.column() == TreeProperties::IND_GENERATED
            ? TableVideos::ValueCheck::Generated
            : TableVideos::ValueCheck::StatsFetched;
        return m_videos->isValueChecked(m_videoId, valueId, which)
            ? Qt::Checked : Qt::Unchecked;
    }
    return QSortFilterProxyModel::data(index, role);
}

bool VideoPlanProxy::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (_isCheckColumn(index.column()) && role == Qt::CheckStateRole && m_videos)
    {
        const QModelIndex nameIndex = index.siblingAtColumn(TreeProperties::IND_NAME);
        if (!nameIndex.data(TreeProperties::RoleIsValue).toBool())
        {
            return false;
        }
        const QUuid valueId = nameIndex.data(TreeProperties::RoleId).toUuid();
        const auto which = index.column() == TreeProperties::IND_GENERATED
            ? TableVideos::ValueCheck::Generated
            : TableVideos::ValueCheck::StatsFetched;
        m_videos->setValueChecked(m_videoId, valueId, which,
                                  value.toInt() == Qt::Checked);
        emit dataChanged(index, index, {Qt::CheckStateRole});
        return true;
    }
    return QSortFilterProxyModel::setData(index, value, role);
}

Qt::ItemFlags VideoPlanProxy::flags(const QModelIndex &index) const
{
    if (_isCheckColumn(index.column()))
    {
        Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        const QModelIndex nameIndex = index.siblingAtColumn(TreeProperties::IND_NAME);
        if (nameIndex.data(TreeProperties::RoleIsValue).toBool())
        {
            itemFlags |= Qt::ItemIsUserCheckable;
        }
        return itemFlags;
    }
    // The plan view is read-only elsewhere: values are edited in the main
    // properties view, not from inside a plan.
    return QSortFilterProxyModel::flags(index) & ~Qt::ItemIsEditable;
}

bool VideoPlanProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_plannedIds.isEmpty())
    {
        return false;
    }
    return _rowOrDescendantPlanned(
        sourceModel()->index(sourceRow, 0, sourceParent));
}

bool VideoPlanProxy::_isCheckColumn(int column) const
{
    return column == TreeProperties::IND_GENERATED || column == TreeProperties::IND_STATS;
}

bool VideoPlanProxy::_rowOrDescendantPlanned(const QModelIndex &sourceIndex) const
{
    if (m_plannedIds.contains(sourceIndex.data(TreeProperties::RoleId).toUuid()))
    {
        return true;
    }
    const int rows = sourceModel()->rowCount(sourceIndex);
    for (int row = 0; row < rows; ++row)
    {
        if (_rowOrDescendantPlanned(sourceModel()->index(row, 0, sourceIndex)))
        {
            return true;
        }
    }
    return false;
}

void VideoPlanProxy::_emitChecksChanged(const QModelIndex &parent)
{
    const int rows = rowCount(parent);
    if (rows == 0)
    {
        return;
    }
    emit dataChanged(index(0, TreeProperties::IND_GENERATED, parent),
                     index(rows - 1, TreeProperties::IND_STATS, parent),
                     {Qt::CheckStateRole});
    for (int row = 0; row < rows; ++row)
    {
        _emitChecksChanged(index(row, 0, parent));
    }
}
