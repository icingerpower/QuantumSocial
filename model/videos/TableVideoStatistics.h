#ifndef TABLEVIDEOSTATISTICS_H
#define TABLEVIDEOSTATISTICS_H

#include <QAbstractTableModel>
#include <QHash>
#include <QPixmap>
#include <QStringList>
#include <QUuid>

#include "SocialStatistics.h"
#include "TableVideos.h"

class TableProjects;

// Table model for PaneStatistics: lists every video that was published and
// located (has retrieved statistics snapshots in TableVideos), sorted by
// view count descending by default.
//
// Columns:
//   0: Image preview (regenerated or source image; up to 2 images merged
//      horizontally when the video/project has more than one source image)
//   1: Name
//   2: Video type
//   3: Created date
//   4+: Statistics columns shared with TreeProperties via SocialStatistics
class TableVideoStatistics : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Roles
    {
        RoleVideoId = Qt::UserRole,
        RoleShortCode,
        RoleProjectId,
        RolePreviewImageCount,
        RolePreviewImagePaths,
    };

    static const int IND_PREVIEW;
    static const int IND_NAME;
    static const int IND_TYPE;
    static const int IND_CREATED;
    static const int IND_FIRST_STAT;
    static const int IND_VIEWS;
    static const int IND_LIKES;
    static const int IND_COMMENTS;
    static const int IND_SHARES;
    static const int IND_NORM_VIEWS;
    static const int IND_ENGAGEMENT;

    static constexpr int PREVIEW_HEIGHT = 64;
    static constexpr int PREVIEW_MAX_SINGLE_WIDTH = 120;
    static constexpr int PREVIEW_MERGE_GAP = 2;

    explicit TableVideoStatistics(TableVideos *videos,
                                  TableProjects *projects = nullptr,
                                  QObject *parent = nullptr);
    ~TableVideoStatistics() override;

    void setProjectsModel(TableProjects *projects);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    void sort(int column, Qt::SortOrder order = Qt::DescendingOrder) override;

    QUuid videoIdAt(int row) const;
    QStringList previewImagePathsAt(int row) const;
    QPixmap previewPixmapAt(int row) const;

    static QPixmap buildPreviewPixmap(const QStringList &imagePaths);

private:
    struct RowEntry
    {
        QUuid videoId;
        TableVideos::AggregateStats stats;
    };

    struct CachedPreview
    {
        QStringList imagePaths;
        QPixmap pixmap;
    };

    static const QStringList HEADER;

    TableVideos *m_videos = nullptr;
    TableProjects *m_projects = nullptr;
    QList<RowEntry> m_rows;
    int m_sortColumn;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;
    mutable QHash<QUuid, CachedPreview> m_previewCache;

    void _rebuild();
    void _sortRows();
    QString _formatName(const TableVideos::VideoRecord &record) const;
    QString _formatVideoType(const TableVideos::VideoRecord &record) const;
    QString _formatCreatedDate(const TableVideos::VideoRecord &record) const;
    QStringList _resolvePreviewImagePaths(const TableVideos::VideoRecord &record) const;
    const CachedPreview &_cachedPreviewFor(const TableVideos::VideoRecord &record) const;
};

#endif // TABLEVIDEOSTATISTICS_H
