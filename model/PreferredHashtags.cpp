#include "PreferredHashtags.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

PreferredHashtags::PreferredHashtags(const QString &workingDirectory)
    : m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("preferred_hashtags.json")))
{
    _loadFromFile();
}

QStringList PreferredHashtags::hashtags() const
{
    return m_hashtags;
}

void PreferredHashtags::setHashtags(const QStringList &hashtags)
{
    QStringList normalized;
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
        normalized << clean;
    }
    m_hashtags = normalized;
    _saveInFile();
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
    const QJsonArray array = QJsonDocument::fromJson(file.readAll()).array();
    for (const QJsonValue &value : array)
    {
        const QString tag = value.toString();
        if (!tag.isEmpty())
        {
            m_hashtags << tag;
        }
    }
}

void PreferredHashtags::_saveInFile() const
{
    QJsonArray array;
    for (const QString &tag : m_hashtags)
    {
        array << tag;
    }
    QFile file{m_filePath};
    if (file.open(QFile::WriteOnly))
    {
        file.write(QJsonDocument(array).toJson());
    }
}
