#include "TableVideoStatistics.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSize>

#include "../TableProjects.h"
#include "../videogen/ProductShowcaseMultiVideoWorkflow.h"
#include "../videogen/VideoGenerationRecipe.h"

namespace {

const QString GENERATED_IMAGE_NAME = QStringLiteral("generation_source.png");
const QString GENERATED_IMAGE_NAME_2 = QStringLiteral("generation_source2.png");
const QString WORKFLOW_MANIFEST_NAME = QStringLiteral("multi_video_workflow.json");
const QString RECIPE_CONFIG_NAME = QStringLiteral("generation_config.json");

QList<QDir> candidateGenerationDirs(const TableProjects *projects, int projRow,
                                    const QUuid &projectId, const QString &shortCode)
{
    QList<QDir> dirs;
    if (!projects || projRow < 0 || shortCode.isEmpty())
    {
        return dirs;
    }
    const QDir projDir = projects->projectDir(projRow);
    const QDir genBaseDir = projects->projectGenerationsDir(projRow);
    const QString idString = projectId.toString(QUuid::WithoutBraces);

    const QStringList candidates{
        genBaseDir.absoluteFilePath(QStringLiteral("%1/%2").arg(shortCode, idString)),
        genBaseDir.absoluteFilePath(QStringLiteral("%1/temp").arg(shortCode)),
        projDir.absoluteFilePath(QStringLiteral("generations/%1/temp").arg(shortCode)),
        genBaseDir.absoluteFilePath(shortCode),
        projDir.absoluteFilePath(QStringLiteral("generations/%1").arg(shortCode)),
    };
    for (const QString &path : candidates)
    {
        const QDir dir{path};
        if (dir.exists())
        {
            dirs << dir;
        }
    }
    return dirs;
}

QString findFileInDirs(const QList<QDir> &dirs, const QString &fileName)
{
    for (const QDir &dir : dirs)
    {
        const QString filePath = dir.absoluteFilePath(fileName);
        if (QFileInfo::exists(filePath))
        {
            return filePath;
        }
    }
    return QString{};
}

QString findPatternInDirs(const QList<QDir> &dirs, const QStringList &nameFilters)
{
    for (const QDir &dir : dirs)
    {
        const QStringList matches = dir.entryList(nameFilters, QDir::Files, QDir::Name);
        if (!matches.isEmpty())
        {
            return dir.absoluteFilePath(matches.first());
        }
    }
    return QString{};
}

QString findLatestGeneratedImageInProject(const TableProjects *projects,
                                          const TableVideos *videos,
                                          int projRow,
                                          const QUuid &projectId,
                                          const QString &fileName)
{
    if (!projects || projRow < 0)
    {
        return QString{};
    }
    if (videos)
    {
        for (const auto *rec : videos->recordsForProject(projectId))
        {
            if (!rec)
            {
                continue;
            }
            const auto dirs = candidateGenerationDirs(projects, projRow, projectId, rec->shortCode);
            const QString found = findFileInDirs(dirs, fileName);
            if (!found.isEmpty())
            {
                return found;
            }
        }
    }
    const QString legacy = projects->projectDir(projRow).absoluteFilePath(fileName);
    return QFileInfo::exists(legacy) ? legacy : QString{};
}

} // namespace

const QStringList TableVideoStatistics::HEADER = []() {
    QStringList h{QStringLiteral("Preview"), QStringLiteral("Name"),
                  QStringLiteral("Video type"), QStringLiteral("Created")};
    h.append(SocialStatistics::headers());
    return h;
}();

const int TableVideoStatistics::IND_PREVIEW{0};
const int TableVideoStatistics::IND_NAME{1};
const int TableVideoStatistics::IND_TYPE{2};
const int TableVideoStatistics::IND_CREATED{3};
const int TableVideoStatistics::IND_FIRST_STAT{4};
const int TableVideoStatistics::IND_VIEWS{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::Views)};
const int TableVideoStatistics::IND_LIKES{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::Likes)};
const int TableVideoStatistics::IND_COMMENTS{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::Comments)};
const int TableVideoStatistics::IND_SHARES{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::Shares)};
const int TableVideoStatistics::IND_NORM_VIEWS{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::NormViews)};
const int TableVideoStatistics::IND_ENGAGEMENT{
    IND_FIRST_STAT + static_cast<int>(SocialStatistics::StatColumn::Engagement)};

TableVideoStatistics::TableVideoStatistics(TableVideos *videos,
                                           TableProjects *projects,
                                           QObject *parent)
    : QAbstractTableModel(parent)
    , m_videos(videos)
    , m_projects(projects)
    , m_sortColumn(IND_VIEWS)
    , m_sortOrder(Qt::DescendingOrder)
{
    if (m_videos)
    {
        const auto refresh = [this]() { _rebuild(); };
        connect(m_videos, &QAbstractItemModel::dataChanged, this, refresh);
        connect(m_videos, &QAbstractItemModel::rowsInserted, this, refresh);
        connect(m_videos, &QAbstractItemModel::rowsRemoved, this, refresh);
        connect(m_videos, &QAbstractItemModel::modelReset, this, refresh);
    }
    setProjectsModel(m_projects);
    _rebuild();
}

TableVideoStatistics::~TableVideoStatistics() = default;

void TableVideoStatistics::setProjectsModel(TableProjects *projects)
{
    if (m_projects)
    {
        disconnect(m_projects, nullptr, this, nullptr);
    }
    m_projects = projects;
    if (m_projects)
    {
        const auto refresh = [this]() { _rebuild(); };
        connect(m_projects, &QAbstractItemModel::dataChanged, this, refresh);
        connect(m_projects, &QAbstractItemModel::rowsInserted, this, refresh);
        connect(m_projects, &QAbstractItemModel::rowsRemoved, this, refresh);
        connect(m_projects, &QAbstractItemModel::modelReset, this, refresh);
    }
    _rebuild();
}

int TableVideoStatistics::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return m_rows.size();
}

int TableVideoStatistics::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return HEADER.size();
}

QVariant TableVideoStatistics::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal
        && section >= 0 && section < HEADER.size())
    {
        return HEADER[section];
    }
    return QVariant{};
}

QVariant TableVideoStatistics::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()
        || index.column() < 0 || index.column() >= HEADER.size())
    {
        return QVariant{};
    }

    const RowEntry &entry = m_rows[index.row()];
    const TableVideos::VideoRecord *record
        = m_videos ? m_videos->recordFromId(entry.videoId) : nullptr;
    if (!record)
    {
        return QVariant{};
    }

    if (role == RoleVideoId)
    {
        return QVariant::fromValue(record->id);
    }
    if (role == RoleShortCode)
    {
        return record->shortCode;
    }
    if (role == RoleProjectId)
    {
        return QVariant::fromValue(record->projectId);
    }
    if (role == RolePreviewImageCount)
    {
        return _cachedPreviewFor(*record).imagePaths.size();
    }
    if (role == RolePreviewImagePaths)
    {
        return _cachedPreviewFor(*record).imagePaths;
    }

    if (index.column() == IND_PREVIEW)
    {
        if (role == Qt::DecorationRole)
        {
            const QPixmap &pixmap = _cachedPreviewFor(*record).pixmap;
            return pixmap.isNull() ? QVariant{} : QVariant{pixmap};
        }
        if (role == Qt::SizeHintRole)
        {
            const QPixmap &pixmap = _cachedPreviewFor(*record).pixmap;
            return !pixmap.isNull()
                ? QVariant{pixmap.size() + QSize(12, 8)}
                : QVariant{QSize(90, PREVIEW_HEIGHT + 8)};
        }
        return QVariant{};
    }

    if (role != Qt::DisplayRole)
    {
        return QVariant{};
    }

    if (index.column() == IND_NAME)
    {
        return _formatName(*record);
    }
    if (index.column() == IND_TYPE)
    {
        return _formatVideoType(*record);
    }
    if (index.column() == IND_CREATED)
    {
        return _formatCreatedDate(*record);
    }
    if (index.column() >= IND_FIRST_STAT
        && index.column() < IND_FIRST_STAT + SocialStatistics::columnCount())
    {
        return SocialStatistics::displayValue(entry.stats, index.column() - IND_FIRST_STAT);
    }
    return QVariant{};
}

void TableVideoStatistics::sort(int column, Qt::SortOrder order)
{
    if (column < 0 || column >= HEADER.size())
    {
        return;
    }
    m_sortColumn = column;
    m_sortOrder = order;
    beginResetModel();
    _sortRows();
    endResetModel();
}

QUuid TableVideoStatistics::videoIdAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row].videoId : QUuid{};
}

QStringList TableVideoStatistics::previewImagePathsAt(int row) const
{
    if (row < 0 || row >= m_rows.size() || !m_videos)
    {
        return {};
    }
    const auto *record = m_videos->recordFromId(m_rows[row].videoId);
    return record ? _cachedPreviewFor(*record).imagePaths : QStringList{};
}

QPixmap TableVideoStatistics::previewPixmapAt(int row) const
{
    if (row < 0 || row >= m_rows.size() || !m_videos)
    {
        return QPixmap{};
    }
    const auto *record = m_videos->recordFromId(m_rows[row].videoId);
    return record ? _cachedPreviewFor(*record).pixmap : QPixmap{};
}

QPixmap TableVideoStatistics::buildPreviewPixmap(const QStringList &imagePaths)
{
    QList<QImage> scaledImages;
    for (const QString &path : imagePaths)
    {
        if (scaledImages.size() >= 2)
        {
            break;
        }
        if (path.isEmpty() || !QFileInfo::exists(path))
        {
            continue;
        }
        QImage image{path};
        if (image.isNull())
        {
            continue;
        }
        QImage scaled = image.scaled(QSize(PREVIEW_MAX_SINGLE_WIDTH, PREVIEW_HEIGHT),
                                     Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
        if (!scaled.isNull())
        {
            scaledImages << scaled;
        }
    }

    if (scaledImages.isEmpty())
    {
        return QPixmap{};
    }
    if (scaledImages.size() == 1)
    {
        return QPixmap::fromImage(scaledImages.first());
    }

    const QImage &left = scaledImages[0];
    const QImage &right = scaledImages[1];
    const int totalWidth = left.width() + PREVIEW_MERGE_GAP + right.width();
    const int maxHeight = std::max(left.height(), right.height());

    QImage merged(totalWidth, maxHeight, QImage::Format_ARGB32_Premultiplied);
    merged.fill(Qt::transparent);

    QPainter painter(&merged);
    painter.drawImage(0, (maxHeight - left.height()) / 2, left);
    painter.drawImage(left.width() + PREVIEW_MERGE_GAP,
                      (maxHeight - right.height()) / 2, right);
    painter.end();

    return QPixmap::fromImage(merged);
}

void TableVideoStatistics::_rebuild()
{
    beginResetModel();
    m_rows.clear();
    m_previewCache.clear();

    if (m_videos)
    {
        for (int row = 0; row < m_videos->rowCount(); ++row)
        {
            const TableVideos::VideoRecord *record = m_videos->recordAt(row);
            if (!record || !m_videos->hasRetrievedStatistics(record->id))
            {
                continue;
            }
            m_rows << RowEntry{record->id, TableVideos::aggregateForRecord(*record)};
        }
    }
    _sortRows();
    endResetModel();
}

void TableVideoStatistics::_sortRows()
{
    std::stable_sort(m_rows.begin(), m_rows.end(),
                     [this](const RowEntry &a, const RowEntry &b) {
        const auto *recA = m_videos ? m_videos->recordFromId(a.videoId) : nullptr;
        const auto *recB = m_videos ? m_videos->recordFromId(b.videoId) : nullptr;

        auto compareTieBreak = [&]() {
            if (a.stats.totalViews != b.stats.totalViews)
            {
                return a.stats.totalViews > b.stats.totalViews;
            }
            if (a.stats.totalLikes != b.stats.totalLikes)
            {
                return a.stats.totalLikes > b.stats.totalLikes;
            }
            if (recA && recB && recA->generatedDate != recB->generatedDate)
            {
                return recA->generatedDate > recB->generatedDate;
            }
            const QString codeA = recA ? recA->shortCode : QString{};
            const QString codeB = recB ? recB->shortCode : QString{};
            return codeA < codeB;
        };

        if (m_sortColumn >= IND_FIRST_STAT
            && m_sortColumn < IND_FIRST_STAT + SocialStatistics::columnCount())
        {
            const int statIdx = m_sortColumn - IND_FIRST_STAT;
            const double valA = SocialStatistics::numericSortValue(a.stats, statIdx);
            const double valB = SocialStatistics::numericSortValue(b.stats, statIdx);
            if (valA != valB)
            {
                return m_sortOrder == Qt::DescendingOrder ? (valA > valB) : (valA < valB);
            }
            return compareTieBreak();
        }

        if (m_sortColumn == IND_NAME && recA && recB)
        {
            const int cmp = _formatName(*recA).compare(_formatName(*recB), Qt::CaseInsensitive);
            if (cmp != 0)
            {
                return m_sortOrder == Qt::DescendingOrder ? (cmp > 0) : (cmp < 0);
            }
            return compareTieBreak();
        }

        if (m_sortColumn == IND_TYPE && recA && recB)
        {
            const int cmp = _formatVideoType(*recA).compare(_formatVideoType(*recB), Qt::CaseInsensitive);
            if (cmp != 0)
            {
                return m_sortOrder == Qt::DescendingOrder ? (cmp > 0) : (cmp < 0);
            }
            return compareTieBreak();
        }

        if (m_sortColumn == IND_CREATED && recA && recB)
        {
            if (recA->generatedDate != recB->generatedDate)
            {
                return m_sortOrder == Qt::DescendingOrder
                    ? (recA->generatedDate > recB->generatedDate)
                    : (recA->generatedDate < recB->generatedDate);
            }
            return compareTieBreak();
        }

        return compareTieBreak();
    });
}

QString TableVideoStatistics::_formatName(const TableVideos::VideoRecord &record) const
{
    QString projectName;
    if (m_projects && !record.projectId.isNull())
    {
        const int projRow = m_projects->rowOfId(record.projectId);
        if (projRow >= 0)
        {
            projectName = m_projects->data(
                m_projects->index(projRow, TableProjects::IND_NAME)).toString().trimmed();
        }
    }
    if (projectName.isEmpty())
    {
        return record.shortCode;
    }
    if (record.shortCode.isEmpty() || projectName == record.shortCode)
    {
        return projectName;
    }
    return QStringLiteral("%1 (%2)").arg(projectName, record.shortCode);
}

QString TableVideoStatistics::_formatVideoType(const TableVideos::VideoRecord &record) const
{
    if (m_projects && !record.projectId.isNull())
    {
        const int projRow = m_projects->rowOfId(record.projectId);
        if (projRow >= 0)
        {
            return TableProjects::formatTypeLabel(m_projects->projectType(projRow));
        }
    }
    return TableProjects::formatTypeLabel(
        record.derivation.isEmpty() ? QStringLiteral("single") : record.derivation);
}

QString TableVideoStatistics::_formatCreatedDate(const TableVideos::VideoRecord &record) const
{
    if (record.generatedDate.isValid())
    {
        return record.generatedDate.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
    }
    if (m_projects && !record.projectId.isNull())
    {
        const int projRow = m_projects->rowOfId(record.projectId);
        if (projRow >= 0)
        {
            return m_projects->data(
                m_projects->index(projRow, TableProjects::IND_CREATED)).toString();
        }
    }
    return QString{};
}

QStringList TableVideoStatistics::_resolvePreviewImagePaths(
    const TableVideos::VideoRecord &record) const
{
    if (!m_projects || record.projectId.isNull())
    {
        return {};
    }
    const int projRow = m_projects->rowOfId(record.projectId);
    if (projRow < 0)
    {
        return {};
    }

    const QList<QDir> genDirs = candidateGenerationDirs(
        m_projects, projRow, record.projectId, record.shortCode);

    // Load saved recipe if present in this generation's folder/temp folder.
    VideoGenerationRecipe recipe;
    for (const QDir &dir : genDirs)
    {
        QString err;
        if (dir.exists(RECIPE_CONFIG_NAME) && VideoGenerationRecipe::load(dir, &recipe, &err))
        {
            break;
        }
    }

    // Slot 1: prefer regenerated image over source image.
    QString slot1 = findFileInDirs(genDirs, GENERATED_IMAGE_NAME);
    if (slot1.isEmpty())
    {
        slot1 = findPatternInDirs(genDirs, {QStringLiteral("repeat_source_1.*")});
    }
    if (slot1.isEmpty() && !recipe.imagePaths.isEmpty()
        && QFileInfo::exists(recipe.imagePaths.at(0)))
    {
        slot1 = recipe.imagePaths.at(0);
    }
    if (slot1.isEmpty())
    {
        slot1 = findLatestGeneratedImageInProject(
            m_projects, m_videos, projRow, record.projectId, GENERATED_IMAGE_NAME);
    }
    if (slot1.isEmpty())
    {
        slot1 = findPatternInDirs(genDirs, {QStringLiteral("source.*")});
    }
    if (slot1.isEmpty())
    {
        const QString projImg1 = m_projects->absoluteImagePath(projRow);
        if (!projImg1.isEmpty() && QFileInfo::exists(projImg1))
        {
            slot1 = projImg1;
        }
    }

    // Slot 2 (when the generation/project has a second source image):
    // prefer regenerated secondary image over secondary source image.
    QString slot2 = findFileInDirs(genDirs, GENERATED_IMAGE_NAME_2);
    if (slot2.isEmpty())
    {
        slot2 = findPatternInDirs(genDirs, {QStringLiteral("repeat_source_2.*")});
    }
    if (slot2.isEmpty() && recipe.imagePaths.size() >= 2
        && QFileInfo::exists(recipe.imagePaths.at(1)))
    {
        slot2 = recipe.imagePaths.at(1);
    }
    if (slot2.isEmpty())
    {
        slot2 = findLatestGeneratedImageInProject(
            m_projects, m_videos, projRow, record.projectId, GENERATED_IMAGE_NAME_2);
    }
    if (slot2.isEmpty())
    {
        slot2 = findPatternInDirs(genDirs, {QStringLiteral("source2.*")});
    }
    if (slot2.isEmpty())
    {
        const QString projImg2 = m_projects->absoluteImagePath2(projRow);
        if (!projImg2.isEmpty() && QFileInfo::exists(projImg2))
        {
            slot2 = projImg2;
        }
    }

    // Multi-video workflow fallback / enrichment: steps store their own
    // ImageInput list with processedPath (regenerated) and originalPath.
    if (m_projects->isMultiVideoProject(projRow))
    {
        ProductShowcaseMultiVideoWorkflow workflow;
        bool loaded = false;
        for (const QDir &dir : genDirs)
        {
            if (dir.exists(WORKFLOW_MANIFEST_NAME)
                && workflow.loadFromFile(dir.absoluteFilePath(WORKFLOW_MANIFEST_NAME)))
            {
                loaded = true;
                break;
            }
        }
        if (!loaded)
        {
            const QString genManifest = m_projects->projectGenerationsDir(projRow)
                .absoluteFilePath(WORKFLOW_MANIFEST_NAME);
            if (QFileInfo::exists(genManifest))
            {
                loaded = workflow.loadFromFile(genManifest);
            }
        }
        if (!loaded)
        {
            const QString projManifest = m_projects->projectDir(projRow)
                .absoluteFilePath(WORKFLOW_MANIFEST_NAME);
            if (QFileInfo::exists(projManifest))
            {
                loaded = workflow.loadFromFile(projManifest);
            }
        }

        if (loaded)
        {
            QStringList workflowImages;
            for (const auto &step : workflow.steps())
            {
                for (const auto &img : step.images)
                {
                    QString chosen;
                    if (!img.processedPath.isEmpty() && QFileInfo::exists(img.processedPath))
                    {
                        chosen = img.processedPath;
                    }
                    else if (!img.originalPath.isEmpty() && QFileInfo::exists(img.originalPath))
                    {
                        chosen = img.originalPath;
                    }
                    if (!chosen.isEmpty() && !workflowImages.contains(chosen))
                    {
                        workflowImages << chosen;
                    }
                }
            }
            if (!workflowImages.isEmpty())
            {
                slot1 = workflowImages.at(0);
            }
            if (slot2.isEmpty() && workflowImages.size() >= 2)
            {
                slot2 = workflowImages.at(1);
            }
        }
    }

    QStringList result;
    if (!slot1.isEmpty())
    {
        result << slot1;
    }
    if (!slot2.isEmpty() && slot2 != slot1)
    {
        result << slot2;
    }
    return result;
}

const TableVideoStatistics::CachedPreview &TableVideoStatistics::_cachedPreviewFor(
    const TableVideos::VideoRecord &record) const
{
    auto it = m_previewCache.find(record.id);
    if (it == m_previewCache.end())
    {
        CachedPreview preview;
        preview.imagePaths = _resolvePreviewImagePaths(record);
        preview.pixmap = buildPreviewPixmap(preview.imagePaths);
        it = m_previewCache.insert(record.id, preview);
    }
    return it.value();
}
