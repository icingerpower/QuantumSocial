#ifndef TABLEVIDEOS_H
#define TABLEVIDEOS_H

#include <QAbstractTableModel>
#include <QDateTime>
#include <QSet>
#include <QUuid>

#include <functional>

#include "../accounts/AbstractSocialAccount.h"

// Ground truth of everything the app has generated: for each video its
// identity (short code), the property values it was made of, its lineage,
// where it was published, and every statistics snapshot ever fetched.
//
// The property tree (TreeProperties) only stores definitions — all facts
// live here, so archiving/renaming a property never corrupts history, and
// per-value aggregates can always be recomputed from raw snapshots.
//
// Persistence is one JSON file (videos.json) in the working directory,
// rewritten on every change like the other models. Records and snapshots
// are the training data for every future CLI planning decision, so they are
// never deleted by the app EXCEPT through removeVideo(), which itself
// refuses a record once it has retrieved statistics (published and
// actually measured) — see hasRetrievedStatistics().
class TableVideos : public QAbstractTableModel
{
    Q_OBJECT

public:
    // One statistics reading of one publication at one moment. Snapshots
    // accumulate at whatever irregular times fetching happens; comparison
    // metrics are interpolated to canonical ages afterwards, so no fetch
    // schedule is ever "missed".
    struct StatSnapshot
    {
        QDateTime fetchDate;
        AbstractSocialAccount::StatSource source
            = AbstractSocialAccount::StatSource::PublicView;
        AbstractSocialAccount::VideoStatistics stats;
    };

    // One place the video was published. platformId is normally a key of
    // ALL_SOCIAL_ACCOUNTS(), but free-form labels are allowed for platforms
    // the app cannot fetch yet — the record is kept now, stats come later.
    struct Publication
    {
        QString platformId;
        QString postUrl;
        QDateTime publishDate;
        qint64 followersAtPublish = -1;
        QList<StatSnapshot> snapshots;
    };

    struct VideoRecord
    {
        QUuid id;
        QUuid projectId;                 // the TableProjects row this came from
        QString shortCode;              // e.g. "QS-0001" — burned into the video
        QDateTime generatedDate;
        QString derivation;             // "original"|"compilation"|"remix"|"repost"
        QList<QUuid> sourceVideoIds;    // lineage: best videos this was built from
        QList<QUuid> propertyValueIds;  // the recipe (TreeProperties value ids)
        QSet<QUuid> generatedValueIds;  // per-value "generated" checkbox state
        QSet<QUuid> statsValueIds;      // per-value "stats fetched" checkbox state
        QList<Publication> publications;
        bool published = false;
    };

    // Which per-value checkbox of the plan view is being read/written.
    enum class ValueCheck
    {
        Generated,
        StatsFetched,
    };

    // Cross-video aggregate for one property value, computed from raw
    // snapshots. -1 means "not enough data yet". Derived videos
    // (compilations, remixes...) count with half weight: their success is
    // partly attributable to composition, not to the values themselves.
    struct AggregateStats
    {
        int videoCount = 0;
        QDateTime lastUsed;
        double medianNormViews = -1.0;   // views at ~7d / followers at publish
        double medianEngagement = -1.0;  // (likes+comments+shares) / views at ~7d
    };

    struct DuePublication
    {
        QUuid videoId;
        QString platformId;
        QString postUrl;
    };

    static const int IND_SHORTCODE;
    static const int IND_DATE;
    static const int IND_DERIVATION;
    static const int IND_VALUES;
    static const int IND_PUBLICATIONS;
    static const int IND_VIEWS;
    static const int IND_LIKES;

    explicit TableVideos(const QString &workingDirectory, QObject *parent = nullptr);
    ~TableVideos() override;

    // Resolves a property-value id to its display name (searching both the
    // active and the archive tree). Kept as a callback so this model stays
    // independent from TreeProperties.
    void setValueNameResolver(std::function<QString(const QUuid &)> resolver);

    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // Creates a record with the next short code; derived videos inherit
    // their sources' property values in addition to the ones given.
    // projectId links it to a TableProjects row (null when generated outside
    // any project, kept for backward-compatible callers).
    QUuid addVideo(const QList<QUuid> &propertyValueIds,
                   const QList<QUuid> &sourceVideoIds = {},
                   const QString &derivation = QStringLiteral("original"),
                   const QUuid &projectId = QUuid{});

    const VideoRecord *recordAt(int row) const;
    const VideoRecord *recordFromId(const QUuid &videoId) const;
    // Tolerant lookup: case-insensitive, "QS-0001", "qs0001" and "#qs0001"
    // all match — the code may come back from a caption in the wild.
    const VideoRecord *recordFromShortCode(const QString &shortCode) const;
    int rowOfId(const QUuid &videoId) const;
    // Every record generated from the given project, most recent first —
    // drives the per-project "Generations" view.
    QList<const VideoRecord *> recordsForProject(const QUuid &projectId) const;

    bool isPublished(const QUuid &videoId) const;
    void setPublished(const QUuid &videoId, bool published);

    // Publications can be attached at any moment, months after generation
    // (e.g. a best video republished elsewhere): the snapshot scheduler
    // picks them up automatically.
    void addPublication(const QUuid &videoId, const QString &platformId,
                        const QString &postUrl, const QDateTime &publishDate,
                        qint64 followersAtPublish);
    void addSnapshot(const QUuid &videoId, const QString &postUrl,
                     AbstractSocialAccount::StatSource source,
                     const AbstractSocialAccount::VideoStatistics &stats);

    void setValueChecked(const QUuid &videoId, const QUuid &valueId,
                         ValueCheck which, bool checked);
    bool isValueChecked(const QUuid &videoId, const QUuid &valueId,
                        ValueCheck which) const;

    // Publications whose latest snapshot is older than an age-widening
    // threshold: daily during the first week, weekly during the first two
    // months, monthly after. This is what makes fetching opportunistic —
    // whenever the check runs, everything overdue is caught up.
    QList<DuePublication> duePublications(
        const QDateTime &now = QDateTime::currentDateTimeUtc()) const;

    AggregateStats aggregateForValue(const QUuid &valueId) const;

    // True once at least one publication of this video has at least one
    // fetched statistics snapshot — the signal that it was actually
    // published and measured, so its recipe is real training data.
    bool hasRetrievedStatistics(const QUuid &videoId) const;
    // The one way a record can be deleted at all (see the class comment):
    // removes it outright — no "kept but fileless" placeholder — but ONLY
    // when hasRetrievedStatistics() is false. Returns false (no-op) if the
    // record doesn't exist or is protected; the caller decides what to do
    // with that video's files in either case.
    bool removeVideo(const QUuid &videoId);

private:
    static const QStringList HEADER;

    QString m_filePath;
    QList<VideoRecord> m_videos;
    qint64 m_nextShortCode = 1;
    std::function<QString(const QUuid &)> m_valueNameResolver;

    VideoRecord *_recordFromId(const QUuid &videoId);
    void _emitRowChanged(int row);
    void _loadFromFile();
    void _saveInFile();
};

#endif // TABLEVIDEOS_H
