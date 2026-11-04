#include "PreferredHashtags.h"

#include "accounts/AbstractSocialAccount.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

const int PreferredHashtags::IND_TAG;
const int PreferredHashtags::IND_SOURCE;
const QString PreferredHashtags::SOURCE_HUMAN{QStringLiteral("Human")};
const QString PreferredHashtags::SOURCE_AI{QStringLiteral("AI")};
const QStringList PreferredHashtags::HEADER{QStringLiteral("Hashtag"), QStringLiteral("Human / AI")};

PreferredHashtags::PreferredHashtags(const QString &workingDirectory, QObject *parent)
    : QAbstractTableModel(parent)
    , m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("preferred_hashtags.json")))
{
    _loadFromFile();
}

int PreferredHashtags::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return m_items.size();
}

int PreferredHashtags::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return HEADER.size();
}

QVariant PreferredHashtags::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
    {
        return QVariant{};
    }

    if (role == Qt::DisplayRole || role == Qt::EditRole)
    {
        if (index.column() == IND_TAG)
        {
            return m_items[index.row()].tag;
        }
        if (index.column() == IND_SOURCE)
        {
            return m_items[index.row()].source;
        }
    }
    return QVariant{};
}

bool PreferredHashtags::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size() || role != Qt::EditRole)
    {
        return false;
    }

    if (index.column() == IND_TAG)
    {
        QString clean = value.toString().trimmed();
        if (!clean.isEmpty() && !clean.startsWith(QLatin1Char('#')))
        {
            clean.prepend(QLatin1Char('#'));
        }
        m_items[index.row()].tag = clean;
    }
    else if (index.column() == IND_SOURCE)
    {
        m_items[index.row()].source = value.toString();
    }
    else
    {
        return false;
    }

    _saveInFile();
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
}

QVariant PreferredHashtags::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 && section < HEADER.size())
    {
        return HEADER[section];
    }
    return QVariant{};
}

Qt::ItemFlags PreferredHashtags::flags(const QModelIndex &index) const
{
    if (!index.isValid())
    {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

bool PreferredHashtags::removeRows(int row, int count, const QModelIndex &parent)
{
    if (row < 0 || row + count > m_items.size() || count <= 0 || parent.isValid())
    {
        return false;
    }

    beginRemoveRows(parent, row, row + count - 1);
    for (int i = 0; i < count; ++i)
    {
        m_items.removeAt(row);
    }
    endRemoveRows();
    _saveInFile();
    return true;
}

QStringList PreferredHashtags::hashtags(bool onlyHuman) const
{
    QStringList result;
    result.reserve(m_items.size());
    for (const auto &item : m_items)
    {
        if (!item.tag.isEmpty())
        {
            if (!onlyHuman || item.source.compare(SOURCE_HUMAN, Qt::CaseInsensitive) == 0)
            {
                result << item.tag;
            }
        }
    }
    return result;
}

QString PreferredHashtags::asPromptSection(bool onlyHuman) const
{
    const QStringList tags = hashtags(onlyHuman);
    if (tags.isEmpty())
    {
        return QString{};
    }
    const QString label = onlyHuman
        ? QStringLiteral("Curated human preferred hashtags for reference (style, niche, and topic reference — generate new, complementary hashtags in a similar spirit):\n%1\n")
        : QStringLiteral("Existing preferred hashtags for reference (style, niche, and topic reference — generate new, complementary hashtags in a similar spirit):\n%1\n");
    return label.arg(tags.join(QStringLiteral(" ")));
}

QString PreferredHashtags::viralHashtagPromptSection(const QString &keyword) const
{
    return viralHashtagPromptSection(hashtags(), keyword);
}

QString PreferredHashtags::viralHashtagPromptSection(const QStringList &preferredTags, const QString &keyword)
{
    QString section = QStringLiteral(
        "HASHTAG SELECTION STRATEGY (Viral 4+1 Formula — Max 5 hashtags per description):\n"
        "Social media algorithms (TikTok, Reels, Shorts) prioritize niche audience relevance and heavily penalize hashtag spam or fabricated tags with zero search volume.\n"
        "- For each description, pick 3 to 4 hashtags strictly from the Existing Preferred Hashtags list below.\n"
        "- You may pick AT MOST 1 new discovery hashtag per description, but it MUST be an established, high-volume / trending tag in this niche (e.g. #fashiontiktok, #ootd, #outfitinspo). NEVER invent composite or zero-volume tags.\n");

    if (!keyword.isEmpty())
    {
        const QString kwTag = QStringLiteral("#") + QString{keyword}.remove(QLatin1Char(' ')).toLower();
        section += QStringLiteral(
            "- Include %1 as one of the hashtags (target keyword SEO anchor).\n").arg(kwTag);
    }

    section += QStringLiteral(
        "- Total hashtags per description MUST NOT EXCEED 5.\n");

    if (!preferredTags.isEmpty())
    {
        section += QStringLiteral(
            "Existing Preferred Hashtags (pick 3-4 from this list):\n%1\n")
            .arg(preferredTags.join(QStringLiteral(" ")));
    }
    else
    {
        section += QStringLiteral(
            "Existing Preferred Hashtags: (none set yet — pick 4-5 high-volume, established hashtags in this niche; no invented tags).\n");
    }

    return section;
}

QString PreferredHashtags::optimizeDescriptionHashtags(const QString &rawDescription, const QString &keyword)
{
    QStringList newTags;
    const QString optimized = optimizeDescriptionHashtags(rawDescription, keyword, hashtags(), &newTags);
    for (const QString &tag : newTags)
    {
        addHashtag(tag, SOURCE_AI);
    }
    return optimized;
}

QString PreferredHashtags::optimizeDescriptionHashtags(
    const QString &rawDescription,
    const QString &keyword,
    const QStringList &preferredTags,
    QStringList *outNewTags)
{
    if (rawDescription.trimmed().isEmpty())
    {
        return QString{};
    }

    // 1. Detect and preserve tracking shortcode (e.g. "qs0001", "#qs0001", "QS-0042")
    static const QRegularExpression codePattern(
        QStringLiteral(R"((?:#\s*qs[-_]?|\bqs[-_]?)([0-9a-hjkmnp-tv-z]{4,})\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch codeMatch = codePattern.match(rawDescription);
    QString preservedCode;
    QString text = rawDescription;
    if (codeMatch.hasMatch())
    {
        preservedCode = codeMatch.captured(0).trimmed();
        text.remove(codeMatch.capturedStart(0), codeMatch.capturedLength(0));
        text = text.trimmed();
    }

    // 2. Separate trailing hashtags block from the caption text
    static const QRegularExpression trailingBlockRegex(
        QStringLiteral(R"((\s*#[A-Za-z0-9_\p{L}-]+[\s,]*)+$)"),
        QRegularExpression::UseUnicodePropertiesOption);
    const QRegularExpressionMatch trailingMatch = trailingBlockRegex.match(text);

    QString captionText = text;
    QString trailingTagsBlock;
    if (trailingMatch.hasMatch())
    {
        captionText = text.left(trailingMatch.capturedStart(0)).trimmed();
        trailingTagsBlock = trailingMatch.captured(0);
    }

    // 3. Extract candidate hashtags from the trailing block
    static const QRegularExpression tagRegex(
        QStringLiteral(R"(#[A-Za-z0-9_\p{L}-]+)"),
        QRegularExpression::UseUnicodePropertiesOption);

    // Track any inline hashtags that remain inside captionText to avoid duplicates
    QSet<QString> inlineTags;
    auto inlineIt = tagRegex.globalMatch(captionText);
    while (inlineIt.hasNext())
    {
        inlineTags.insert(inlineIt.next().captured(0).toLower());
    }

    QStringList candidateTags;
    QSet<QString> seenCandidates;
    auto tagIt = tagRegex.globalMatch(trailingTagsBlock);
    while (tagIt.hasNext())
    {
        QString tag = tagIt.next().captured(0).trimmed();
        while (tag.endsWith(QLatin1Char('.')) || tag.endsWith(QLatin1Char(','))
               || tag.endsWith(QLatin1Char(';')) || tag.endsWith(QLatin1Char('!')))
        {
            tag.chop(1);
        }
        if (!tag.startsWith(QLatin1Char('#')) || tag.length() <= 1)
        {
            continue;
        }
        if (!AbstractSocialAccount::extractShortCode(tag).isEmpty())
        {
            continue;
        }
        const QString lower = tag.toLower();
        if (!seenCandidates.contains(lower) && !inlineTags.contains(lower))
        {
            seenCandidates.insert(lower);
            candidateTags.append(tag);
        }
    }

    // 4. Determine SEO keyword tag
    QString kwTag;
    if (!keyword.trimmed().isEmpty())
    {
        kwTag = QStringLiteral("#") + QString{keyword.trimmed()}.remove(QLatin1Char(' ')).toLower();
    }

    // 5. Index preferred tags
    QSet<QString> preferredSet;
    QMap<QString, QString> preferredCased;
    for (const QString &pt : preferredTags)
    {
        QString clean = pt.trimmed();
        if (clean.isEmpty())
        {
            continue;
        }
        if (!clean.startsWith(QLatin1Char('#')))
        {
            clean.prepend(QLatin1Char('#'));
        }
        preferredSet.insert(clean.toLower());
        preferredCased.insert(clean.toLower(), clean);
    }

    // Classify candidates into existing (in preferred) vs new
    QStringList candidateExisting;
    QStringList candidateNew;
    for (const QString &tag : candidateTags)
    {
        const QString lower = tag.toLower();
        if (!kwTag.isEmpty() && lower == kwTag.toLower())
        {
            // Keyword tag handled specifically below
            continue;
        }
        if (preferredSet.contains(lower))
        {
            candidateExisting.append(preferredCased.value(lower, tag));
        }
        else
        {
            candidateNew.append(tag);
        }
    }

    // 6. Select tags according to the 4+1 viral formula (Max 5 total tags)
    QStringList selectedTags;
    QSet<QString> selectedKeys = inlineTags; // inline tags count toward the 5 limit

    // Rule A: SEO Keyword Tag (anchor)
    if (!kwTag.isEmpty() && !selectedKeys.contains(kwTag.toLower()) && selectedKeys.size() < 5)
    {
        selectedTags.append(kwTag);
        selectedKeys.insert(kwTag.toLower());
    }

    // Rule B: Pick existing validated tags from candidates (up to 4 existing)
    for (const QString &tag : candidateExisting)
    {
        if (selectedKeys.size() >= 4)
        {
            break;
        }
        const QString lower = tag.toLower();
        if (!selectedKeys.contains(lower))
        {
            selectedTags.append(tag);
            selectedKeys.insert(lower);
        }
    }

    // Rule C: At most ONE new high-volume discovery tag
    if (!candidateNew.isEmpty() && selectedKeys.size() < 5)
    {
        const QString firstNew = candidateNew.first();
        const QString lower = firstNew.toLower();
        if (!selectedKeys.contains(lower))
        {
            selectedTags.append(firstNew);
            selectedKeys.insert(lower);
            if (outNewTags)
            {
                outNewTags->append(firstNew);
            }
        }
    }

    // Rule D: If preferred tags exist and we have fewer than 3-4 validated tags,
    // supplement from preferred tags (prioritize tags matching words in caption)
    if (!preferredTags.isEmpty() && selectedKeys.size() < 4)
    {
        // First pass: match caption words
        for (const QString &pt : preferredTags)
        {
            if (selectedKeys.size() >= 4)
            {
                break;
            }
            QString clean = pt.trimmed();
            if (!clean.startsWith(QLatin1Char('#')))
            {
                clean.prepend(QLatin1Char('#'));
            }
            const QString tagWord = clean.mid(1);
            if (tagWord.length() >= 3 && captionText.contains(tagWord, Qt::CaseInsensitive))
            {
                const QString lower = clean.toLower();
                if (!selectedKeys.contains(lower))
                {
                    selectedTags.append(clean);
                    selectedKeys.insert(lower);
                }
            }
        }

        // Second pass: supplement from top preferred tags
        for (const QString &pt : preferredTags)
        {
            if (selectedKeys.size() >= 4)
            {
                break;
            }
            QString clean = pt.trimmed();
            if (!clean.startsWith(QLatin1Char('#')))
            {
                clean.prepend(QLatin1Char('#'));
            }
            const QString lower = clean.toLower();
            if (!selectedKeys.contains(lower))
            {
                selectedTags.append(clean);
                selectedKeys.insert(lower);
            }
        }
    }
    // If preferredTags was empty (fresh profile), accept up to 5 candidate tags total
    else if (preferredTags.isEmpty() && selectedKeys.size() < 5)
    {
        for (int i = 1; i < candidateNew.size() && selectedKeys.size() < 5; ++i)
        {
            const QString nt = candidateNew[i];
            const QString lower = nt.toLower();
            if (!selectedKeys.contains(lower))
            {
                selectedTags.append(nt);
                selectedKeys.insert(lower);
                if (outNewTags)
                {
                    outNewTags->append(nt);
                }
            }
        }
    }

    // 7. Assemble final description
    QString result = captionText;
    if (!selectedTags.isEmpty())
    {
        if (!result.isEmpty())
        {
            result += QLatin1Char(' ');
        }
        result += selectedTags.join(QLatin1Char(' '));
    }
    if (!preservedCode.isEmpty())
    {
        if (!result.isEmpty() && !result.endsWith(QLatin1Char(' ')))
        {
            result += QLatin1Char(' ');
        }
        result += preservedCode;
    }

    return result;
}

void PreferredHashtags::setHashtags(const QStringList &hashtags)
{
    beginResetModel();
    QList<Item> normalized;
    QSet<QString> seen;
    for (const QString &tag : hashtags)
    {
        QString clean = tag.trimmed();
        if (clean.isEmpty())
        {
            continue;
        }
        if (!clean.startsWith(QLatin1Char('#')))
        {
            clean.prepend(QLatin1Char('#'));
        }
        const QString key = clean.toLower();
        if (seen.contains(key))
        {
            continue;
        }
        seen.insert(key);

        QString src = SOURCE_HUMAN;
        for (const auto &it : m_items)
        {
            if (it.tag.compare(clean, Qt::CaseInsensitive) == 0)
            {
                src = it.source;
                break;
            }
        }
        normalized.append({clean, src});
    }
    m_items = normalized;
    endResetModel();
    _saveInFile();
}

void PreferredHashtags::addHashtag(const QString &tag, const QString &source)
{
    QString clean = tag.trimmed();
    if (clean.isEmpty())
    {
        return;
    }
    if (!clean.startsWith(QLatin1Char('#')))
    {
        clean.prepend(QLatin1Char('#'));
    }
    for (int i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].tag.compare(clean, Qt::CaseInsensitive) == 0)
        {
            if (m_items[i].source != source)
            {
                m_items[i].source = source;
                _saveInFile();
                emit dataChanged(index(i, IND_SOURCE), index(i, IND_SOURCE),
                                 {Qt::DisplayRole, Qt::EditRole});
            }
            return;
        }
    }

    const int row = m_items.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_items.append({clean, source});
    endInsertRows();
    _saveInFile();
}

void PreferredHashtags::addRow(const QString &source)
{
    const int row = m_items.size();
    beginInsertRows(QModelIndex{}, row, row);
    m_items.append({QStringLiteral("#"), source});
    endInsertRows();
    _saveInFile();
}

void PreferredHashtags::removeHashtag(int row)
{
    removeRow(row);
}

QStringList PreferredHashtags::parse(const QString &freeformText)
{
    static const QRegularExpression separators{QStringLiteral("[\\s,]+")};
    return freeformText.split(separators, Qt::SkipEmptyParts);
}

void PreferredHashtags::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    m_items.clear();
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : array)
    {
        if (value.isString())
        {
            const QString tag = value.toString().trimmed();
            if (!tag.isEmpty())
            {
                m_items.append({tag, SOURCE_HUMAN});
            }
        }
        else if (value.isObject())
        {
            const QJsonObject obj = value.toObject();
            const QString tag = obj[QStringLiteral("tag")].toString().trimmed();
            const QString source = obj[QStringLiteral("source")].toString(SOURCE_HUMAN);
            if (!tag.isEmpty())
            {
                m_items.append({tag, source});
            }
        }
    }
}

void PreferredHashtags::_saveInFile() const
{
    QJsonArray array;
    for (const auto &item : m_items)
    {
        QJsonObject obj;
        obj[QStringLiteral("tag")] = item.tag;
        obj[QStringLiteral("source")] = item.source;
        array.append(obj);
    }
    QFile file{m_filePath};
    if (file.open(QFile::WriteOnly))
    {
        file.write(QJsonDocument(array).toJson());
    }
}
