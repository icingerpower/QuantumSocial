#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "TableProjects.h"
#include "videos/TableVideos.h"

namespace {

const QString PROJECTS_SUBDIR = QStringLiteral("projects");
const QString FILE_NAME = QStringLiteral("projects.csv");

// Unlike the accounts file, these fields are free text (name, hook idea...)
// that will contain commas, so this file uses real CSV quoting instead of
// the naive split: fields containing a comma or a quote are wrapped in
// quotes with inner quotes doubled. Newlines are flattened to spaces on
// write to keep the format line-based.
QString escapeCsvField(const QString &field)
{
    QString escaped = field;
    escaped.replace(QLatin1Char('\n'), QLatin1Char(' '));
    escaped.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (escaped.contains(QLatin1Char(',')) || escaped.contains(QLatin1Char('"')))
    {
        escaped.replace(QLatin1String("\""), QLatin1String("\"\""));
        escaped = QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return escaped;
}

QString slugify(const QString &text)
{
    const QString trimmed = text.trimmed().toLower();
    QString result;
    result.reserve(trimmed.size());
    bool lastWasHyphen = false;
    for (const QChar &ch : trimmed)
    {
        if (ch.isLetterOrNumber())
        {
            result += ch;
            lastWasHyphen = false;
        }
        else if (ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch.isSpace())
        {
            if (!lastWasHyphen && !result.isEmpty())
            {
                result += QLatin1Char('-');
                lastWasHyphen = true;
            }
        }
    }
    while (result.endsWith(QLatin1Char('-')))
    {
        result.chop(1);
    }
    return result;
}

QStringList splitCsvLine(const QString &line)
{
    QStringList fields;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i)
    {
        const QChar character = line[i];
        if (inQuotes)
        {
            if (character == QLatin1Char('"'))
            {
                if (i + 1 < line.size() && line[i + 1] == QLatin1Char('"'))
                {
                    current += QLatin1Char('"');
                    ++i;
                }
                else
                {
                    inQuotes = false;
                }
            }
            else
            {
                current += character;
            }
        }
        else if (character == QLatin1Char('"'))
        {
            inQuotes = true;
        }
        else if (character == QLatin1Char(','))
        {
            fields << current;
            current.clear();
        }
        else
        {
            current += character;
        }
    }
    fields << current;
    return fields;
}

} // namespace

const QStringList TableProjects::HEADER{
    "Name", "Created", "Keyword", "Hook idea", "Image", "Gen prompt",
    "Gen hook", "Gen description", "Image 2", "Type", "Last update", "Id"};
const int TableProjects::IND_NAME{0};
const int TableProjects::IND_CREATED{1};
const int TableProjects::IND_KEYWORD{2};
const int TableProjects::IND_HOOK{3};
const int TableProjects::IND_IMAGE{4};
const int TableProjects::IND_GEN_PROMPT{5};
const int TableProjects::IND_GEN_HOOK{6};
const int TableProjects::IND_GEN_DESC{7};
const int TableProjects::IND_IMAGE2{8};
const int TableProjects::IND_TYPE{9};
const int TableProjects::IND_LAST_UPDATE{10};
const int TableProjects::IND_ID{11};

const QColor TableProjects::COLOR_RECENT_BG{34, 100, 48};
const QColor TableProjects::COLOR_RECENT_FG{230, 255, 230};

TableProjects::TableProjects(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
    , m_workingDir(workingDirectory)
{
    m_workingDir.mkpath(PROJECTS_SUBDIR);
    m_filePath = m_workingDir.absoluteFilePath(
        PROJECTS_SUBDIR + QLatin1Char('/') + FILE_NAME);
    _loadFromFile();
}

QVariant TableProjects::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal)
    {
        if (section >= 0 && section < HEADER.size())
        {
            if (section == IND_NAME)
            {
                return tr("Name");
            }
            if (section == IND_TYPE)
            {
                return tr("Type");
            }
            if (section == IND_CREATED)
            {
                return tr("Created");
            }
            if (section == IND_LAST_UPDATE)
            {
                return tr("Last update");
            }
            return HEADER[section];
        }
    }
    return QVariant{};
}

int TableProjects::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return m_listOfVariantList.size();
}

int TableProjects::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return HEADER.size();
}

QVariant TableProjects::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_listOfVariantList.size())
    {
        return QVariant{};
    }

    if (index.column() == IND_LAST_UPDATE)
    {
        const QDateTime dt = lastVideoDate(index.row());
        if (role == Qt::DisplayRole)
        {
            if (dt.isValid())
            {
                return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
            }
            return QString{};
        }
        if (role == Qt::EditRole)
        {
            return dt.isValid() ? dt.toUTC().toString(Qt::ISODate) : QString{};
        }
        if (role == Qt::ToolTipRole)
        {
            if (dt.isValid())
            {
                return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
            }
            return QString{};
        }
        if (role == Qt::BackgroundRole)
        {
            if (isRecentDate(dt))
            {
                return COLOR_RECENT_BG;
            }
            return QVariant{};
        }
        if (role == Qt::ForegroundRole)
        {
            if (isRecentDate(dt))
            {
                return COLOR_RECENT_FG;
            }
            return QVariant{};
        }
        return QVariant{};
    }

    if (role != Qt::DisplayRole && role != Qt::EditRole)
    {
        return QVariant{};
    }
    const QVariant &value = m_listOfVariantList[index.row()][index.column()];
    // Stored as ISO 8601 (sorts correctly as text); displayed local.
    if (index.column() == IND_CREATED && role == Qt::DisplayRole)
    {
        const QDateTime created = QDateTime::fromString(value.toString(), Qt::ISODate);
        if (created.isValid())
        {
            return created.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        }
    }
    if (index.column() == IND_TYPE)
    {
        if (role == Qt::DisplayRole)
        {
            return formatTypeLabel(projectType(index.row()));
        }
        return projectType(index.row());
    }
    return value;
}

bool TableProjects::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::EditRole)
    {
        const int row = index.row();
        if (index.column() == IND_LAST_UPDATE)
        {
            m_listOfVariantList[index.row()][index.column()] = value;
            _saveInFile();
            emit dataChanged(index, index, {role, Qt::DisplayRole, Qt::BackgroundRole, Qt::ForegroundRole});
            return true;
        }

        const QString oldFolderName = (index.column() == IND_NAME && row >= 0 && row < m_listOfVariantList.size())
            ? projectGenerationFolderName(row) : QString{};

        m_listOfVariantList[index.row()][index.column()] = value;
        _saveInFile();

        if (!oldFolderName.isEmpty())
        {
            const QString newFolderName = projectGenerationFolderName(row);
            if (!newFolderName.isEmpty() && oldFolderName != newFolderName)
            {
                QDir baseGenerationsDir{m_workingDir.absoluteFilePath(
                    PROJECTS_SUBDIR + QStringLiteral("/generations"))};
                if (baseGenerationsDir.exists(oldFolderName) && !baseGenerationsDir.exists(newFolderName))
                {
                    baseGenerationsDir.rename(oldFolderName, newFolderName);
                }
            }
        }

        emit dataChanged(index, index, {role, Qt::DisplayRole});
        return true;
    }
    return false;
}

Qt::ItemFlags TableProjects::flags(const QModelIndex &index) const
{
    Qt::ItemFlags itemFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    // Name in the table, keyword/hook through the Source form's mapper. The
    // creation date, image and id are set once by addProject().
    if (index.column() == IND_NAME || index.column() == IND_KEYWORD
        || index.column() == IND_HOOK || index.column() == IND_GEN_PROMPT
        || index.column() == IND_GEN_HOOK || index.column() == IND_GEN_DESC)
    {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

namespace {
// Copies imagePath into the project folder under the given file stem
// ("source"/"source2"); returns the stored (possibly relative) path, or the
// original path unchanged as a fallback if the copy fails (unreadable
// source, full disk...), or "" when imagePath itself doesn't exist.
QString storeProjectImage(const QDir &workingDir, const QString &idString,
                          const QString &stem, const QString &imagePath)
{
    const QFileInfo imageInfo{imagePath};
    if (!imageInfo.exists())
    {
        return QString{};
    }
    const QString relativeTarget = QStringLiteral("projects/") + idString
        + QLatin1Char('/') + stem + QLatin1Char('.') + imageInfo.suffix().toLower();
    if (QFile::copy(imagePath, workingDir.absoluteFilePath(relativeTarget)))
    {
        return relativeTarget;
    }
    return imagePath;
}
} // namespace

int TableProjects::addProject(const QString &imagePath, const QString &imagePath2,
                              const QString &keyword, const QString &hookIdea,
                              const QString &type)
{
    const QUuid id = QUuid::createUuid();
    const QString idString = id.toString(QUuid::WithoutBraces);
    m_workingDir.mkpath(PROJECTS_SUBDIR + QLatin1Char('/') + idString);

    const QString storedImagePath
        = storeProjectImage(m_workingDir, idString, QStringLiteral("source"), imagePath);
    const QString storedImagePath2
        = storeProjectImage(m_workingDir, idString, QStringLiteral("source2"), imagePath2);

    const QString name = keyword.trimmed().isEmpty()
        ? tr("Project %1").arg(m_listOfVariantList.size() + 1)
        : keyword.trimmed();

    const QString storedType = type.trimmed().isEmpty() ? QStringLiteral("single") : type.trimmed();

    const int row = m_listOfVariantList.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_listOfVariantList << QVariantList{
        name
        , QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        , keyword
        , hookIdea
        , storedImagePath
        , QString{} // generation prompt, filled once the CLI suggested one
        , QString{} // picked hook
        , QString{} // picked description (with hashtags)
        , storedImagePath2
        , storedType
        , QString{} // last update
        , idString
    };
    endInsertRows();
    _saveInFile();
    return row;
}

int TableProjects::addMultiVideoProject(const QString &keyword, const QString &firstImagePath)
{
    const QUuid id = QUuid::createUuid();
    const QString idString = id.toString(QUuid::WithoutBraces);
    m_workingDir.mkpath(PROJECTS_SUBDIR + QLatin1Char('/') + idString);

    const QString storedImagePath = !firstImagePath.isEmpty()
        ? storeProjectImage(m_workingDir, idString, QStringLiteral("source"), firstImagePath)
        : QString{};

    const QString name = keyword.trimmed().isEmpty()
        ? tr("Multi-Video %1").arg(m_listOfVariantList.size() + 1)
        : keyword.trimmed();

    const int row = m_listOfVariantList.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_listOfVariantList << QVariantList{
        name
        , QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        , keyword
        , QString{} // hook idea
        , storedImagePath
        , QString{} // gen prompt
        , QString{} // gen hook
        , QString{} // gen desc
        , QString{} // image 2
        , QStringLiteral("multi_video")
        , QString{} // last update
        , idString
    };
    endInsertRows();
    _saveInFile();
    return row;
}

bool TableProjects::isMultiVideoProject(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return false;
    }
    const QString type = m_listOfVariantList[row][IND_TYPE].toString();
    if (type == QLatin1String("multi_video"))
    {
        return true;
    }
    return projectDir(row).exists(QStringLiteral("multi_video_workflow.json"))
        || projectGenerationsDir(row).exists(QStringLiteral("multi_video_workflow.json"));
}

QString TableProjects::projectType(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QString{};
    }
    const QString type = m_listOfVariantList[row][IND_TYPE].toString();
    if (!type.isEmpty())
    {
        return type;
    }
    return isMultiVideoProject(row) ? QStringLiteral("multi_video") : QStringLiteral("single");
}

QString TableProjects::formatTypeLabel(const QString &type)
{
    const QString trimmed = type.trimmed().toLower();
    if (trimmed.isEmpty() || trimmed == QLatin1String("single"))
    {
        return tr("Single");
    }
    if (trimmed == QLatin1String("multi_video") || trimmed == QLatin1String("multi-video"))
    {
        return tr("Multi-video");
    }
    // Future kinds: capitalize words and convert underscores to hyphens
    QString formatted = trimmed;
    formatted.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (!formatted.isEmpty())
    {
        formatted[0] = formatted[0].toUpper();
    }
    return formatted;
}

void TableProjects::removeProject(int row)
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return;
    }
    QDir dir = projectDir(row);
    if (dir != m_workingDir)
    {
        dir.removeRecursively();
    }
    QDir genDir = projectGenerationsDir(row);
    if (genDir != m_workingDir && genDir.exists())
    {
        genDir.removeRecursively();
    }
    beginRemoveRows(QModelIndex{}, row, row);
    m_listOfVariantList.removeAt(row);
    endRemoveRows();
    _saveInFile();
}

QUuid TableProjects::projectId(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QUuid{};
    }
    return QUuid::fromString(m_listOfVariantList[row][IND_ID].toString());
}

int TableProjects::rowOfId(const QUuid &projectId) const
{
    const QString idString = projectId.toString(QUuid::WithoutBraces);
    for (int row = 0; row < m_listOfVariantList.size(); ++row)
    {
        if (m_listOfVariantList[row][IND_ID].toString() == idString)
        {
            return row;
        }
    }
    return -1;
}

QDir TableProjects::projectDir(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return m_workingDir;
    }
    return QDir{m_workingDir.absoluteFilePath(PROJECTS_SUBDIR + QLatin1Char('/')
        + m_listOfVariantList[row][IND_ID].toString())};
}

QString TableProjects::projectGenerationFolderName(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QString{};
    }
    const QString name = m_listOfVariantList[row][IND_NAME].toString();
    const QString slug = slugify(name);
    const QString prefix = QStringLiteral("%1").arg(row + 1, 3, 10, QLatin1Char('0'));
    return slug.isEmpty() ? prefix : QStringLiteral("%1-%2").arg(prefix, slug);
}

QDir TableProjects::projectGenerationsDir(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return m_workingDir;
    }
    const QString folderName = projectGenerationFolderName(row);
    if (folderName.isEmpty())
    {
        return m_workingDir;
    }
    QDir dir{m_workingDir.absoluteFilePath(
        PROJECTS_SUBDIR + QStringLiteral("/generations/") + folderName)};
    dir.mkpath(QStringLiteral("."));
    return dir;
}

QString TableProjects::absoluteImagePath(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QString{};
    }
    const QString stored = m_listOfVariantList[row][IND_IMAGE].toString();
    if (stored.isEmpty() || QFileInfo{stored}.isAbsolute())
    {
        return stored;
    }
    return m_workingDir.absoluteFilePath(stored);
}

QString TableProjects::absoluteImagePath2(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QString{};
    }
    const QString stored = m_listOfVariantList[row][IND_IMAGE2].toString();
    if (stored.isEmpty() || QFileInfo{stored}.isAbsolute())
    {
        return stored;
    }
    return m_workingDir.absoluteFilePath(stored);
}

QDir TableProjects::generationDir(int row, const QString &shortCode, bool create) const
{
    if (row < 0 || row >= m_listOfVariantList.size() || shortCode.isEmpty())
    {
        return m_workingDir;
    }
    const QDir newDir{projectGenerationsDir(row).absoluteFilePath(shortCode)};
    const QDir legacyDir{projectDir(row).absoluteFilePath(
        QStringLiteral("generations/") + shortCode)};

    if (legacyDir.exists() && !newDir.exists())
    {
        return legacyDir;
    }
    if (create)
    {
        newDir.mkpath(QStringLiteral("."));
    }
    return newDir;
}

QDir TableProjects::generationTempDir(int row, const QString &shortCode) const
{
    if (row < 0 || row >= m_listOfVariantList.size() || shortCode.isEmpty())
    {
        return m_workingDir;
    }
    const QDir baseGenDir = generationDir(row, shortCode);
    const QString idString = m_listOfVariantList[row][IND_ID].toString();
    const QString subfolderName = idString.isEmpty() ? QStringLiteral("temp") : idString;
    const QDir idDir{baseGenDir.absoluteFilePath(subfolderName)};

    if (idDir.exists())
    {
        return idDir;
    }
    const QDir legacyTempDir{baseGenDir.absoluteFilePath(QStringLiteral("temp"))};
    if (legacyTempDir.exists())
    {
        return legacyTempDir;
    }
    idDir.mkpath(QStringLiteral("."));
    return idDir;
}

QDir TableProjects::stagingDir(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return m_workingDir;
    }
    QDir dir{projectDir(row).absoluteFilePath(QStringLiteral("staging"))};
    dir.mkpath(QStringLiteral("."));
    return dir;
}

void TableProjects::resetStagingDir(int row) const
{
    QDir dir = stagingDir(row);
    dir.removeRecursively();
    dir.mkpath(QStringLiteral("."));
}

void TableProjects::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }

    QTextStream stream{&file};
    const auto &lines = stream.readAll().split(QStringLiteral("\n"));
    file.close();

    // First line is the header, skip it.
    for (int i = 1; i < lines.size(); ++i)
    {
        if (lines[i].trimmed().isEmpty())
        {
            continue;
        }
        QStringList elements = splitCsvLine(lines[i]);
        if (elements.size() < 2 || elements.last().isEmpty())
        {
            continue; // The id (always last) is mandatory.
        }
        // Rows written before a middle column existed are shorter: default
        // the missing fields to empty, keeping the id last.
        const QString id = elements.takeLast();
        while (elements.size() < HEADER.size() - 1)
        {
            elements << QString{};
        }
        QString type = elements[IND_TYPE];
        if (type.isEmpty())
        {
            const QDir projDir{m_workingDir.absoluteFilePath(PROJECTS_SUBDIR + QLatin1Char('/') + id)};
            const QString manifestName = QStringLiteral("multi_video_workflow.json");
            bool isMulti = projDir.exists(manifestName);
            if (!isMulti)
            {
                const QString slug = QStringLiteral("%1-%2")
                    .arg(m_listOfVariantList.size() + 1, 3, 10, QLatin1Char('0'))
                    .arg(slugify(elements[IND_NAME]));
                const QDir genDir{m_workingDir.absoluteFilePath(
                    QStringLiteral("projects/generations/") + slug)};
                if (genDir.exists(manifestName))
                {
                    isMulti = true;
                }
            }
            type = isMulti ? QStringLiteral("multi_video") : QStringLiteral("single");
        }
        m_listOfVariantList << QVariantList{
            elements[IND_NAME]
            , elements[IND_CREATED]
            , elements[IND_KEYWORD]
            , elements[IND_HOOK]
            , elements[IND_IMAGE]
            , elements[IND_GEN_PROMPT]
            , elements[IND_GEN_HOOK]
            , elements[IND_GEN_DESC]
            , elements[IND_IMAGE2]
            , type
            , elements[IND_LAST_UPDATE]
            , id
        };
    }
}

void TableProjects::_saveInFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }

    QTextStream stream{&file};
    stream << HEADER.join(QStringLiteral(","));
    for (int row = 0; row < m_listOfVariantList.size(); ++row)
    {
        const QDateTime dt = lastVideoDate(row);
        if (dt.isValid() && IND_LAST_UPDATE < m_listOfVariantList[row].size())
        {
            m_listOfVariantList[row][IND_LAST_UPDATE] = dt.toUTC().toString(Qt::ISODate);
        }
        QStringList elements;
        for (const auto &value : m_listOfVariantList[row])
        {
            elements << escapeCsvField(value.toString());
        }
        stream << "\n" + elements.join(QStringLiteral(","));
    }
    file.close();
}

void TableProjects::setVideosModel(TableVideos *videos)
{
    if (m_videos == videos)
    {
        return;
    }
    if (m_videos)
    {
        disconnect(m_videos, nullptr, this, nullptr);
    }
    m_videos = videos;
    if (m_videos)
    {
        const auto notifyAll = [this]() {
            refreshLastUpdate(-1);
        };
        connect(m_videos, &QAbstractItemModel::rowsInserted, this, notifyAll);
        connect(m_videos, &QAbstractItemModel::rowsRemoved, this, notifyAll);
        connect(m_videos, &QAbstractItemModel::modelReset, this, notifyAll);
        connect(m_videos, &QAbstractItemModel::dataChanged, this, notifyAll);
        notifyAll();
    }
}

void TableProjects::refreshLastUpdate(int row)
{
    if (rowCount() == 0)
    {
        return;
    }
    if (row >= 0 && row < rowCount())
    {
        emit dataChanged(index(row, IND_LAST_UPDATE), index(row, IND_LAST_UPDATE),
                         {Qt::DisplayRole, Qt::BackgroundRole, Qt::ForegroundRole, Qt::ToolTipRole});
    }
    else
    {
        emit dataChanged(index(0, IND_LAST_UPDATE), index(rowCount() - 1, IND_LAST_UPDATE),
                         {Qt::DisplayRole, Qt::BackgroundRole, Qt::ForegroundRole, Qt::ToolTipRole});
    }
}

bool TableProjects::isRecentDate(const QDateTime &dt)
{
    if (!dt.isValid())
    {
        return false;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime dtUtc = dt.toUTC();
    const qint64 secs = dtUtc.secsTo(now);
    return secs >= -60 && secs < 3 * 86400;
}

QDateTime TableProjects::lastVideoDate(int row) const
{
    if (row < 0 || row >= m_listOfVariantList.size())
    {
        return QDateTime{};
    }

    const QUuid projId = projectId(row);

    if (m_videos)
    {
        const auto records = m_videos->recordsForProject(projId);
        for (const auto *record : records)
        {
            if (!record)
            {
                continue;
            }
            const QDir genDir = generationDir(row, record->shortCode, false);
            if (genDir.exists())
            {
                const bool hasVideo = genDir.exists(QStringLiteral("final_video.mp4"))
                    || (genDir.exists(QStringLiteral("takes")) && genDir.exists(QStringLiteral("multi_video_workflow.json")))
                    || !genDir.entryList({QStringLiteral("*.mp4"), QStringLiteral("*.mov"),
                                          QStringLiteral("*.webm"), QStringLiteral("*.mkv")},
                                         QDir::Files).isEmpty();
                const bool hasOnlyImages = !hasVideo && !genDir.entryList({QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                                                                           QStringLiteral("*.jpeg"), QStringLiteral("*.webp")},
                                                                          QDir::Files).isEmpty();
                if (hasVideo || !hasOnlyImages)
                {
                    if (record->generatedDate.isValid())
                    {
                        return record->generatedDate;
                    }
                }
            }
            else
            {
                if (record->generatedDate.isValid())
                {
                    return record->generatedDate;
                }
            }
        }
    }

    const QDir genBaseDir = projectGenerationsDir(row);
    if (genBaseDir.exists())
    {
        QDateTime newest;
        const auto subDirs = genBaseDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &subDirInfo : subDirs)
        {
            const QDir subDir{subDirInfo.absoluteFilePath()};
            const auto videoFiles = subDir.entryInfoList({QStringLiteral("*.mp4"), QStringLiteral("*.mov"),
                                                          QStringLiteral("*.webm"), QStringLiteral("*.mkv")},
                                                         QDir::Files);
            for (const QFileInfo &vFile : videoFiles)
            {
                const QDateTime modTime = vFile.lastModified();
                if (!newest.isValid() || modTime > newest)
                {
                    newest = modTime;
                }
            }
        }
        if (newest.isValid())
        {
            return newest;
        }
    }

    const QDir projDir = projectDir(row);
    if (projDir.exists())
    {
        QDateTime newest;
        const auto videoFiles = projDir.entryInfoList({QStringLiteral("*.mp4"), QStringLiteral("*.mov"),
                                                       QStringLiteral("*.webm"), QStringLiteral("*.mkv")},
                                                      QDir::Files);
        for (const QFileInfo &vFile : videoFiles)
        {
            const QDateTime modTime = vFile.lastModified();
            if (!newest.isValid() || modTime > newest)
            {
                newest = modTime;
            }
        }
        if (newest.isValid())
        {
            return newest;
        }
    }

    if (IND_LAST_UPDATE < m_listOfVariantList[row].size())
    {
        const QString stored = m_listOfVariantList[row][IND_LAST_UPDATE].toString();
        if (!stored.isEmpty())
        {
            const QDateTime parsed = QDateTime::fromString(stored, Qt::ISODate);
            if (parsed.isValid())
            {
                return parsed;
            }
        }
    }

    return QDateTime{};
}
