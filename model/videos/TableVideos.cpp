#include <algorithm>
#include <cmath>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "TableVideos.h"

namespace {

// Crockford base32: unambiguous alphabet (no I, L, O, U), so a code read
// back from a video caption or a burned-in watermark is hard to mistype.
const QString CROCKFORD = QStringLiteral("0123456789ABCDEFGHJKMNPQRSTVWXYZ");
const QString SHORTCODE_PREFIX = QStringLiteral("QS-");

QString encodeShortCode(qint64 number)
{
    QString digits;
    do
    {
        digits.prepend(CROCKFORD[number % 32]);
        number /= 32;
    } while (number > 0);
    while (digits.size() < 4)
    {
        digits.prepend(QLatin1Char('0'));
    }
    return SHORTCODE_PREFIX + digits;
}

// "#qs0001", "qs-0001" and "QS-0001" all normalize to "0001".
QString normalizeShortCode(const QString &code)
{
    QString normalized;
    for (const QChar &character : code)
    {
        if (character.isLetterOrNumber())
        {
            normalized += character.toUpper();
        }
    }
    if (normalized.startsWith(QLatin1String("QS")))
    {
        normalized = normalized.mid(2);
    }
    while (normalized.size() > 1 && normalized.startsWith(QLatin1Char('0')))
    {
        normalized = normalized.mid(1);
    }
    return normalized;
}

QJsonArray uuidsToJson(const QList<QUuid> &ids)
{
    QJsonArray array;
    for (const QUuid &id : ids)
    {
        array << id.toString(QUuid::WithoutBraces);
    }
    return array;
}

QJsonArray uuidsToJson(const QSet<QUuid> &ids)
{
    QList<QUuid> sorted{ids.begin(), ids.end()};
    std::sort(sorted.begin(), sorted.end());
    return uuidsToJson(sorted);
}

QList<QUuid> uuidsFromJson(const QJsonArray &array)
{
    QList<QUuid> ids;
    for (const QJsonValue &value : array)
    {
        const QUuid id = QUuid::fromString(value.toString());
        if (!id.isNull())
        {
            ids << id;
        }
    }
    return ids;
}

QJsonObject statsToJson(const AbstractSocialAccount::VideoStatistics &stats)
{
    return QJsonObject{
        {QStringLiteral("views"), stats.views},
        {QStringLiteral("likes"), stats.likes},
        {QStringLiteral("comments"), stats.comments},
        {QStringLiteral("shares"), stats.shares},
        {QStringLiteral("impressions"), stats.impressions},
        {QStringLiteral("watchTimeSeconds"), stats.watchTimeSeconds},
        {QStringLiteral("avgWatchPercent"), stats.avgWatchPercent},
        {QStringLiteral("follows"), stats.followsFromVideo},
    };
}

AbstractSocialAccount::VideoStatistics statsFromJson(const QJsonObject &obj)
{
    AbstractSocialAccount::VideoStatistics stats;
    stats.views = static_cast<qint64>(obj.value(QStringLiteral("views")).toDouble(-1));
    stats.likes = static_cast<qint64>(obj.value(QStringLiteral("likes")).toDouble(-1));
    stats.comments = static_cast<qint64>(obj.value(QStringLiteral("comments")).toDouble(-1));
    stats.shares = static_cast<qint64>(obj.value(QStringLiteral("shares")).toDouble(-1));
    stats.impressions = static_cast<qint64>(obj.value(QStringLiteral("impressions")).toDouble(-1));
    stats.watchTimeSeconds
        = static_cast<qint64>(obj.value(QStringLiteral("watchTimeSeconds")).toDouble(-1));
    stats.avgWatchPercent = obj.value(QStringLiteral("avgWatchPercent")).toDouble(-1.0);
    stats.followsFromVideo = static_cast<qint64>(obj.value(QStringLiteral("follows")).toDouble(-1));
    return stats;
}

// The age at which videos are compared: raw counts on these platforms have
// mostly plateaued by then, so sparse/late fetching barely hurts accuracy.
constexpr double CANONICAL_AGE_HOURS = 7.0 * 24.0;
// Snapshots younger than this cannot stand in for the canonical age.
constexpr double MIN_USABLE_AGE_HOURS = 5.0 * 24.0;

struct PublicationMetric
{
    double views = -1.0;
    double engagement = -1.0;
};

// Estimates the view count of one publication at the canonical age from
// whatever snapshots exist: log-time interpolation when snapshots bracket
// it (view growth is roughly logarithmic), else the snapshot nearest to the
// canonical age as long as it is old enough to be on the plateau.
bool metricAtCanonicalAge(const TableVideos::Publication &publication,
                          PublicationMetric &metric)
{
    if (!publication.publishDate.isValid())
    {
        return false;
    }

    // Never mix sources: public and analytics counts are not comparable.
    // Public is preferred (available for every video, not only our own).
    for (const auto wantedSource : {AbstractSocialAccount::StatSource::PublicView,
                                    AbstractSocialAccount::StatSource::Analytics})
    {
        struct Point
        {
            double ageHours;
            const TableVideos::StatSnapshot *snapshot;
        };
        QList<Point> points;
        for (const auto &snapshot : publication.snapshots)
        {
            if (snapshot.source != wantedSource || snapshot.stats.views < 0)
            {
                continue;
            }
            const double ageHours
                = publication.publishDate.msecsTo(snapshot.fetchDate) / 3'600'000.0;
            if (ageHours > 0.0)
            {
                points << Point{ageHours, &snapshot};
            }
        }
        if (points.isEmpty())
        {
            continue;
        }
        std::sort(points.begin(), points.end(),
                  [](const Point &a, const Point &b) { return a.ageHours < b.ageHours; });

        const Point *before = nullptr;
        const Point *after = nullptr;
        const Point *nearest = &points.first();
        for (const Point &point : points)
        {
            if (point.ageHours <= CANONICAL_AGE_HOURS)
            {
                before = &point;
            }
            else if (!after)
            {
                after = &point;
            }
            if (std::abs(point.ageHours - CANONICAL_AGE_HOURS)
                < std::abs(nearest->ageHours - CANONICAL_AGE_HOURS))
            {
                nearest = &point;
            }
        }

        if (before && after)
        {
            const double logT = std::log(CANONICAL_AGE_HOURS);
            const double logBefore = std::log(before->ageHours);
            const double logAfter = std::log(after->ageHours);
            const double fraction = logAfter > logBefore
                ? (logT - logBefore) / (logAfter - logBefore)
                : 0.0;
            metric.views = before->snapshot->stats.views
                + fraction * (after->snapshot->stats.views - before->snapshot->stats.views);
        }
        else if (nearest->ageHours >= MIN_USABLE_AGE_HOURS)
        {
            metric.views = nearest->snapshot->stats.views;
        }
        else
        {
            return false; // Only young snapshots so far — try again later.
        }

        const auto &stats = nearest->snapshot->stats;
        if (stats.views > 0)
        {
            const double interactions = std::max<qint64>(stats.likes, 0)
                + std::max<qint64>(stats.comments, 0)
                + std::max<qint64>(stats.shares, 0);
            metric.engagement = interactions / static_cast<double>(stats.views);
        }
        return true;
    }
    return false;
}

// Median where each sample can count more or less than one video (derived
// videos contribute with half weight).
double weightedMedian(QList<QPair<double, double>> samples)
{
    if (samples.isEmpty())
    {
        return -1.0;
    }
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const auto &sample : samples)
    {
        total += sample.second;
    }
    double cumulated = 0.0;
    for (const auto &sample : samples)
    {
        cumulated += sample.second;
        if (cumulated >= total / 2.0)
        {
            return sample.first;
        }
    }
    return samples.last().first;
}

} // namespace

const QStringList TableVideos::HEADER{
    "Code", "Created", "Derivation", "Values", "Publications", "Views", "Likes"};
const int TableVideos::IND_SHORTCODE{0};
const int TableVideos::IND_DATE{1};
const int TableVideos::IND_DERIVATION{2};
const int TableVideos::IND_VALUES{3};
const int TableVideos::IND_PUBLICATIONS{4};
const int TableVideos::IND_VIEWS{5};
const int TableVideos::IND_LIKES{6};

TableVideos::TableVideos(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
{
    m_filePath = QDir{workingDirectory}.absoluteFilePath(QStringLiteral("videos.json"));
    _loadFromFile();
}

TableVideos::~TableVideos() = default;

void TableVideos::setValueNameResolver(std::function<QString(const QUuid &)> resolver)
{
    m_valueNameResolver = std::move(resolver);
    if (!m_videos.isEmpty())
    {
        emit dataChanged(index(0, IND_VALUES), index(m_videos.size() - 1, IND_VALUES));
    }
}

QVariant TableVideos::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        return HEADER[section];
    }
    return QVariant{};
}

int TableVideos::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_videos.size();
}

int TableVideos::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return HEADER.size();
}

QVariant TableVideos::data(const QModelIndex &index, int role) const
{
    if (role != Qt::DisplayRole)
    {
        return QVariant{};
    }
    const VideoRecord &record = m_videos[index.row()];
    switch (index.column())
    {
    case 0: // IND_SHORTCODE (constant not usable in a case label)
        return record.shortCode;
    case 1: // IND_DATE
        return record.generatedDate.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
    case 2: // IND_DERIVATION
        return record.derivation;
    case 3: // IND_VALUES
    {
        QStringList names;
        for (const QUuid &valueId : record.propertyValueIds)
        {
            const QString name = m_valueNameResolver ? m_valueNameResolver(valueId) : QString{};
            names << (name.isEmpty() ? valueId.toString(QUuid::WithoutBraces).left(8) : name);
        }
        return names.join(QStringLiteral(", "));
    }
    case 4: // IND_PUBLICATIONS
    {
        QStringList platforms;
        for (const Publication &publication : record.publications)
        {
            platforms << publication.platformId;
        }
        return platforms.join(QStringLiteral(", "));
    }
    case 5: // IND_VIEWS
    case 6: // IND_LIKES
    {
        // Sum of each publication's latest snapshot; blank until data exists.
        qint64 total = -1;
        for (const Publication &publication : record.publications)
        {
            const StatSnapshot *latest = nullptr;
            for (const StatSnapshot &snapshot : publication.snapshots)
            {
                if (!latest || snapshot.fetchDate > latest->fetchDate)
                {
                    latest = &snapshot;
                }
            }
            if (!latest)
            {
                continue;
            }
            const qint64 value = index.column() == 5 ? latest->stats.views : latest->stats.likes;
            if (value >= 0)
            {
                total = std::max<qint64>(total, 0) + value;
            }
        }
        return total >= 0 ? QVariant{total} : QVariant{};
    }
    default:
        return QVariant{};
    }
}

QUuid TableVideos::addVideo(const QList<QUuid> &propertyValueIds,
                            const QList<QUuid> &sourceVideoIds,
                            const QString &derivation,
                            const QUuid &projectId)
{
    VideoRecord record;
    record.id = QUuid::createUuid();
    record.projectId = projectId;
    record.shortCode = encodeShortCode(m_nextShortCode++);
    record.generatedDate = QDateTime::currentDateTimeUtc();
    record.derivation = derivation;
    record.sourceVideoIds = sourceVideoIds;
    record.propertyValueIds = propertyValueIds;

    // Derived videos inherit their sources' recipe: the union is stored
    // explicitly so it stays true even if the source records change later.
    for (const QUuid &sourceId : sourceVideoIds)
    {
        if (const VideoRecord *source = recordFromId(sourceId))
        {
            for (const QUuid &valueId : source->propertyValueIds)
            {
                if (!record.propertyValueIds.contains(valueId))
                {
                    record.propertyValueIds << valueId;
                }
            }
        }
    }

    const int row = m_videos.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_videos << record;
    endInsertRows();
    _saveInFile();
    return record.id;
}

const TableVideos::VideoRecord *TableVideos::recordAt(int row) const
{
    return row >= 0 && row < m_videos.size() ? &m_videos[row] : nullptr;
}

const TableVideos::VideoRecord *TableVideos::recordFromId(const QUuid &videoId) const
{
    return const_cast<TableVideos *>(this)->_recordFromId(videoId);
}

const TableVideos::VideoRecord *TableVideos::recordFromShortCode(const QString &shortCode) const
{
    const QString wanted = normalizeShortCode(shortCode);
    if (wanted.isEmpty())
    {
        return nullptr;
    }
    for (const VideoRecord &record : m_videos)
    {
        if (normalizeShortCode(record.shortCode) == wanted)
        {
            return &record;
        }
    }
    return nullptr;
}

QList<const TableVideos::VideoRecord *> TableVideos::recordsForProject(
    const QUuid &projectId) const
{
    QList<const VideoRecord *> records;
    for (const VideoRecord &record : m_videos)
    {
        if (record.projectId == projectId)
        {
            records << &record;
        }
    }
    std::sort(records.begin(), records.end(),
              [](const VideoRecord *a, const VideoRecord *b) {
        return a->generatedDate > b->generatedDate;
    });
    return records;
}

int TableVideos::rowOfId(const QUuid &videoId) const
{
    for (int row = 0; row < m_videos.size(); ++row)
    {
        if (m_videos[row].id == videoId)
        {
            return row;
        }
    }
    return -1;
}

void TableVideos::addPublication(const QUuid &videoId, const QString &platformId,
                                 const QString &postUrl, const QDateTime &publishDate,
                                 qint64 followersAtPublish)
{
    VideoRecord *record = _recordFromId(videoId);
    if (!record)
    {
        return;
    }
    Publication publication;
    publication.platformId = platformId;
    publication.postUrl = postUrl;
    publication.publishDate = publishDate;
    publication.followersAtPublish = followersAtPublish;
    record->publications << publication;
    _emitRowChanged(rowOfId(videoId));
    _saveInFile();
}

void TableVideos::addSnapshot(const QUuid &videoId, const QString &postUrl,
                              AbstractSocialAccount::StatSource source,
                              const AbstractSocialAccount::VideoStatistics &stats)
{
    VideoRecord *record = _recordFromId(videoId);
    if (!record)
    {
        return;
    }
    for (Publication &publication : record->publications)
    {
        if (publication.postUrl != postUrl)
        {
            continue;
        }
        StatSnapshot snapshot;
        snapshot.fetchDate = QDateTime::currentDateTimeUtc();
        snapshot.source = source;
        snapshot.stats = stats;
        publication.snapshots << snapshot;

        // Having real data auto-ticks the plan view's "stats" checkboxes.
        for (const QUuid &valueId : record->propertyValueIds)
        {
            record->statsValueIds.insert(valueId);
        }
        _emitRowChanged(rowOfId(videoId));
        _saveInFile();
        return;
    }
}

void TableVideos::setValueChecked(const QUuid &videoId, const QUuid &valueId,
                                  ValueCheck which, bool checked)
{
    VideoRecord *record = _recordFromId(videoId);
    if (!record)
    {
        return;
    }
    QSet<QUuid> &ids = which == ValueCheck::Generated
        ? record->generatedValueIds
        : record->statsValueIds;
    if (checked == ids.contains(valueId))
    {
        return;
    }
    if (checked)
    {
        ids.insert(valueId);
    }
    else
    {
        ids.remove(valueId);
    }
    _emitRowChanged(rowOfId(videoId));
    _saveInFile();
}

bool TableVideos::isValueChecked(const QUuid &videoId, const QUuid &valueId,
                                 ValueCheck which) const
{
    const VideoRecord *record = recordFromId(videoId);
    if (!record)
    {
        return false;
    }
    return which == ValueCheck::Generated
        ? record->generatedValueIds.contains(valueId)
        : record->statsValueIds.contains(valueId);
}

QList<TableVideos::DuePublication> TableVideos::duePublications(const QDateTime &now) const
{
    QList<DuePublication> due;
    for (const VideoRecord &record : m_videos)
    {
        for (const Publication &publication : record.publications)
        {
            if (publication.postUrl.isEmpty() || !publication.publishDate.isValid())
            {
                continue;
            }
            const double ageHours = publication.publishDate.msecsTo(now) / 3'600'000.0;
            if (ageHours <= 0.0)
            {
                continue;
            }
            const double intervalHours = ageHours < 7 * 24 ? 24.0
                : ageHours < 60 * 24 ? 7.0 * 24.0
                : 30.0 * 24.0;

            QDateTime lastFetch;
            for (const StatSnapshot &snapshot : publication.snapshots)
            {
                if (!lastFetch.isValid() || snapshot.fetchDate > lastFetch)
                {
                    lastFetch = snapshot.fetchDate;
                }
            }
            if (!lastFetch.isValid()
                || lastFetch.msecsTo(now) / 3'600'000.0 >= intervalHours)
            {
                due << DuePublication{record.id, publication.platformId, publication.postUrl};
            }
        }
    }
    return due;
}

TableVideos::AggregateStats TableVideos::aggregateForValue(const QUuid &valueId) const
{
    AggregateStats aggregate;
    QList<QPair<double, double>> normViewsSamples;    // (value, weight)
    QList<QPair<double, double>> engagementSamples;

    for (const VideoRecord &record : m_videos)
    {
        if (!record.propertyValueIds.contains(valueId))
        {
            continue;
        }
        ++aggregate.videoCount;
        if (!aggregate.lastUsed.isValid() || record.generatedDate > aggregate.lastUsed)
        {
            aggregate.lastUsed = record.generatedDate;
        }

        const double weight
            = record.derivation == QLatin1String("original") || record.derivation.isEmpty()
            ? 1.0 : 0.5;
        for (const Publication &publication : record.publications)
        {
            PublicationMetric metric;
            if (!metricAtCanonicalAge(publication, metric))
            {
                continue;
            }
            if (metric.views >= 0.0 && publication.followersAtPublish > 0)
            {
                normViewsSamples << qMakePair(
                    metric.views / publication.followersAtPublish, weight);
            }
            if (metric.engagement >= 0.0)
            {
                engagementSamples << qMakePair(metric.engagement, weight);
            }
        }
    }

    aggregate.medianNormViews = weightedMedian(normViewsSamples);
    aggregate.medianEngagement = weightedMedian(engagementSamples);
    return aggregate;
}

bool TableVideos::hasRetrievedStatistics(const QUuid &videoId) const
{
    const VideoRecord *record = recordFromId(videoId);
    if (!record)
    {
        return false;
    }
    for (const Publication &publication : record->publications)
    {
        if (!publication.snapshots.isEmpty())
        {
            return true;
        }
    }
    return false;
}

bool TableVideos::removeVideo(const QUuid &videoId)
{
    if (hasRetrievedStatistics(videoId))
    {
        return false;
    }
    const int row = rowOfId(videoId);
    if (row < 0)
    {
        return false;
    }
    beginRemoveRows(QModelIndex{}, row, row);
    m_videos.removeAt(row);
    endRemoveRows();
    _saveInFile();
    return true;
}

TableVideos::VideoRecord *TableVideos::_recordFromId(const QUuid &videoId)
{
    for (VideoRecord &record : m_videos)
    {
        if (record.id == videoId)
        {
            return &record;
        }
    }
    return nullptr;
}

void TableVideos::_emitRowChanged(int row)
{
    if (row >= 0)
    {
        emit dataChanged(index(row, 0), index(row, HEADER.size() - 1));
    }
}

void TableVideos::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    m_nextShortCode = static_cast<qint64>(
        root.value(QStringLiteral("nextShortCode")).toDouble(1));
    for (const QJsonValue &videoValue : root.value(QStringLiteral("videos")).toArray())
    {
        const QJsonObject videoObj = videoValue.toObject();
        VideoRecord record;
        record.id = QUuid::fromString(videoObj.value(QStringLiteral("id")).toString());
        if (record.id.isNull())
        {
            continue;
        }
        record.projectId
            = QUuid::fromString(videoObj.value(QStringLiteral("projectId")).toString());
        record.shortCode = videoObj.value(QStringLiteral("shortCode")).toString();
        record.generatedDate = QDateTime::fromString(
            videoObj.value(QStringLiteral("generatedDate")).toString(), Qt::ISODate);
        record.derivation = videoObj.value(QStringLiteral("derivation")).toString();
        record.sourceVideoIds
            = uuidsFromJson(videoObj.value(QStringLiteral("sourceVideoIds")).toArray());
        record.propertyValueIds
            = uuidsFromJson(videoObj.value(QStringLiteral("propertyValueIds")).toArray());
        const auto generated
            = uuidsFromJson(videoObj.value(QStringLiteral("generatedValueIds")).toArray());
        record.generatedValueIds = QSet<QUuid>{generated.begin(), generated.end()};
        const auto fetched
            = uuidsFromJson(videoObj.value(QStringLiteral("statsValueIds")).toArray());
        record.statsValueIds = QSet<QUuid>{fetched.begin(), fetched.end()};

        for (const QJsonValue &pubValue : videoObj.value(QStringLiteral("publications")).toArray())
        {
            const QJsonObject pubObj = pubValue.toObject();
            Publication publication;
            publication.platformId = pubObj.value(QStringLiteral("platformId")).toString();
            publication.postUrl = pubObj.value(QStringLiteral("postUrl")).toString();
            publication.publishDate = QDateTime::fromString(
                pubObj.value(QStringLiteral("publishDate")).toString(), Qt::ISODate);
            publication.followersAtPublish = static_cast<qint64>(
                pubObj.value(QStringLiteral("followersAtPublish")).toDouble(-1));
            for (const QJsonValue &snapValue
                 : pubObj.value(QStringLiteral("snapshots")).toArray())
            {
                const QJsonObject snapObj = snapValue.toObject();
                StatSnapshot snapshot;
                snapshot.fetchDate = QDateTime::fromString(
                    snapObj.value(QStringLiteral("fetchDate")).toString(), Qt::ISODate);
                snapshot.source = AbstractSocialAccount::statSourceFromKey(
                    snapObj.value(QStringLiteral("source")).toString());
                snapshot.stats = statsFromJson(snapObj);
                publication.snapshots << snapshot;
            }
            record.publications << publication;
        }
        m_videos << record;
    }
}

void TableVideos::_saveInFile()
{
    QJsonArray videos;
    for (const VideoRecord &record : m_videos)
    {
        QJsonArray publications;
        for (const Publication &publication : record.publications)
        {
            QJsonArray snapshots;
            for (const StatSnapshot &snapshot : publication.snapshots)
            {
                QJsonObject snapObj = statsToJson(snapshot.stats);
                snapObj.insert(QStringLiteral("fetchDate"),
                               snapshot.fetchDate.toString(Qt::ISODate));
                snapObj.insert(QStringLiteral("source"),
                               AbstractSocialAccount::statSourceKey(snapshot.source));
                snapshots << snapObj;
            }
            publications << QJsonObject{
                {QStringLiteral("platformId"), publication.platformId},
                {QStringLiteral("postUrl"), publication.postUrl},
                {QStringLiteral("publishDate"), publication.publishDate.toString(Qt::ISODate)},
                {QStringLiteral("followersAtPublish"), publication.followersAtPublish},
                {QStringLiteral("snapshots"), snapshots},
            };
        }
        videos << QJsonObject{
            {QStringLiteral("id"), record.id.toString(QUuid::WithoutBraces)},
            {QStringLiteral("projectId"), record.projectId.toString(QUuid::WithoutBraces)},
            {QStringLiteral("shortCode"), record.shortCode},
            {QStringLiteral("generatedDate"), record.generatedDate.toString(Qt::ISODate)},
            {QStringLiteral("derivation"), record.derivation},
            {QStringLiteral("sourceVideoIds"), uuidsToJson(record.sourceVideoIds)},
            {QStringLiteral("propertyValueIds"), uuidsToJson(record.propertyValueIds)},
            {QStringLiteral("generatedValueIds"), uuidsToJson(record.generatedValueIds)},
            {QStringLiteral("statsValueIds"), uuidsToJson(record.statsValueIds)},
            {QStringLiteral("publications"), publications},
        };
    }

    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument{QJsonObject{
        {QStringLiteral("nextShortCode"), m_nextShortCode},
        {QStringLiteral("videos"), videos},
    }}.toJson(QJsonDocument::Indented));
    file.close();
}
