#ifndef TREEPROPERTIES_H
#define TREEPROPERTIES_H

#include <QAbstractItemModel>
#include <QDateTime>
#include <QHash>
#include <QUuid>

#include <memory>

#include "../videos/TableVideos.h"

// Catalog of the video-generation properties being A/B-tested: top-level
// nodes are properties (e.g. "Background"), their children are the testable
// values (e.g. "Warm modern background", "Street background").
//
// Every node has a stable UUID; video records reference values by that id
// only, so nodes can be renamed or moved to the archive instance without
// corrupting history. Two instances live side by side (active + archive,
// each with its own JSON file) and archiveTo() moves subtrees between them
// — in both directions, so archiving is always reversible.
//
// The statistics columns are NOT stored here: they are computed on demand
// from the TableVideos ground truth (setVideosModel) and only cached for
// display. This model is in charge of definitions, nothing else.
class TreeProperties : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles
    {
        RoleId = Qt::UserRole,  // QUuid of the node
        RoleIsValue,            // true for testable values (depth >= 2)
    };

    static const int IND_NAME;
    static const int IND_PROMPT;
    static const int IND_ORIGIN;
    static const int IND_VIDEO_COUNT;
    static const int IND_NORM_VIEWS;
    static const int IND_ENGAGEMENT;
    static const int IND_LAST_USED;
    // The two plan-progress columns are declared by this model but always
    // empty here: VideoPlanProxy serves their checkboxes from the video
    // record it is bound to (progress is per video, not per value). Main
    // tree views should hide them.
    static const int IND_GENERATED;
    static const int IND_STATS;

    struct PropertyNode
    {
        QUuid id;
        QString name;
        QString promptFragment;   // text injected into the generation prompt
        QDateTime createdDate;
        // How this node came to exist: "manual" (added from the properties
        // list), "cli" (an ordinary suggestion round proposed it), or
        // "image-bootstrap" (proposed while analyzing user-picked reference
        // images — see PaneGeneration's _suggestPlan/_runPropertyBootstrap).
        // Empty for nodes saved before this field existed.
        QString origin;
        PropertyNode *parent = nullptr;
        QList<PropertyNode *> children;

        ~PropertyNode();
    };

    explicit TreeProperties(const QString &filePath, QObject *parent = nullptr);
    ~TreeProperties() override;

    // Source of the statistics columns; also connects invalidation so the
    // aggregates refresh whenever new snapshots arrive.
    void setVideosModel(TableVideos *videos);

    QModelIndex index(int row, int column,
                      const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    QModelIndex addProperty(const QString &name,
                           const QString &origin = QStringLiteral("manual"));
    QModelIndex addValue(const QModelIndex &property, const QString &name,
                         const QString &promptFragment,
                         const QString &origin = QStringLiteral("manual"));
    void removeNode(const QModelIndex &index);

    // Moves the subtree into the destination instance, ids intact. When a
    // single value is archived, its parent property is mirrored in the
    // destination (same id) so the value stays in context; when a whole
    // property is archived into a destination that already mirrors it, the
    // children are merged under the existing mirror.
    void archiveTo(TreeProperties &destination, const QModelIndex &index);

    PropertyNode *nodeFromId(const QUuid &id) const;
    QModelIndex indexFromNode(PropertyNode *node) const;
    // The properties (top-level nodes) — used by PropertySampler to walk
    // the catalog without going through model indexes.
    QList<PropertyNode *> topLevelNodes() const;

private:
    static const QStringList HEADER;

    QString m_filePath;
    std::unique_ptr<PropertyNode> m_root;
    TableVideos *m_videos = nullptr;
    mutable QHash<QUuid, TableVideos::AggregateStats> m_aggregateCache;

    PropertyNode *_nodeFromIndex(const QModelIndex &index) const;
    const TableVideos::AggregateStats *_aggregateForNode(PropertyNode *node) const;
    void _invalidateAggregates();
    void _emitAggregatesChanged(const QModelIndex &parent);
    void _loadFromFile();
    void _saveInFile();
};

#endif // TREEPROPERTIES_H
