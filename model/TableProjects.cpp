#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include "TableProjects.h"

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
    "Gen hook", "Gen description", "Image 2", "Id"};
const int TableProjects::IND_NAME{0};
const int TableProjects::IND_CREATED{1};
const int TableProjects::IND_KEYWORD{2};
const int TableProjects::IND_HOOK{3};
const int TableProjects::IND_IMAGE{4};
const int TableProjects::IND_GEN_PROMPT{5};
const int TableProjects::IND_GEN_HOOK{6};
const int TableProjects::IND_GEN_DESC{7};
const int TableProjects::IND_IMAGE2{8};
const int TableProjects::IND_ID{9};

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
        return HEADER[section];
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
    return value;
}

bool TableProjects::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::EditRole)
    {
        const int row = index.row();
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
                              const QString &keyword, const QString &hookIdea)
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
        , idString
    };
    endInsertRows();
    _saveInFile();
    return row;
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

QDir TableProjects::generationDir(int row, const QString &shortCode) const
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
    newDir.mkpath(QStringLiteral("."));
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
    for (const auto &variantList : m_listOfVariantList)
    {
        QStringList elements;
        for (const auto &value : variantList)
        {
            elements << escapeCsvField(value.toString());
        }
        stream << "\n" + elements.join(QStringLiteral(","));
    }
    file.close();
}
