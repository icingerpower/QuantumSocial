#include "FavoriteVideoPrompts.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

FavoriteVideoPrompts::FavoriteVideoPrompts(const QString &workingDirectory)
    : m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("favorite_video_prompts.md")))
{
    _loadFromFile();
}

QStringList FavoriteVideoPrompts::prompts() const
{
    return m_prompts;
}

void FavoriteVideoPrompts::setPrompts(const QStringList &prompts)
{
    QStringList normalized;
    for (const QString &prompt : prompts)
    {
        const QString clean = prompt.trimmed();
        if (clean.isEmpty() || normalized.contains(clean, Qt::CaseInsensitive))
        {
            continue;
        }
        normalized << clean;
    }
    m_prompts = normalized;
    _saveInFile();
}

void FavoriteVideoPrompts::addPrompt(const QString &prompt)
{
    const QString clean = prompt.trimmed();
    if (clean.isEmpty())
    {
        return;
    }
    for (const QString &existing : m_prompts)
    {
        if (existing.compare(clean, Qt::CaseInsensitive) == 0)
        {
            return; // already saved
        }
    }
    m_prompts << clean;
    _saveInFile();
}

QString FavoriteVideoPrompts::asPromptSection() const
{
    if (m_prompts.isEmpty())
    {
        return QString{};
    }
    QStringList bullets;
    for (const QString &prompt : m_prompts)
    {
        bullets << QStringLiteral("- %1").arg(prompt);
    }
    return QStringLiteral(
        "Video prompts that actually rendered like the fast, punchy, "
        "viral-style short-form videos wanted here (style/pacing/structure "
        "reference — write NEW prompts in a similar spirit, do not just "
        "repeat these verbatim):\n%1\n").arg(bullets.join(QStringLiteral("\n")));
}

void FavoriteVideoPrompts::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    QTextStream stream{&file};
    for (const QString &rawLine : stream.readAll().split(QLatin1Char('\n')))
    {
        const QString trimmed = rawLine.trimmed();
        // Only lines actually written as bullets are prompts — everything
        // else (heading, explanatory paragraph, blank lines) must never be
        // mistaken for one (see PromptLessons, which originally got this
        // wrong).
        if (!trimmed.startsWith(QLatin1String("- ")))
        {
            continue;
        }
        const QString prompt = trimmed.mid(2).trimmed();
        if (!prompt.isEmpty())
        {
            m_prompts << prompt;
        }
    }
}

void FavoriteVideoPrompts::_saveInFile() const
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    QTextStream stream{&file};
    stream << "# Favorite video prompts\n\n"
              "Example video prompts that actually rendered the way you "
              "want — fast, punchy, viral-style short-form, not slow/artsy "
              "cinematic filler — harvested from a past generation "
              "(Generation page → select a video → \"Favorite this video "
              "prompt\") or written here directly. Fed to the CLI as style "
              "reference when it suggests new video prompts. Edit or delete "
              "lines freely.\n\n";
    for (const QString &prompt : m_prompts)
    {
        stream << "- " << prompt << "\n";
    }
}
