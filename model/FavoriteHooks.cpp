#include "FavoriteHooks.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

FavoriteHooks::FavoriteHooks(const QString &workingDirectory)
    : m_filePath(QDir(workingDirectory).absoluteFilePath(
          QStringLiteral("favorite_hooks.md")))
{
    _loadFromFile();
}

QStringList FavoriteHooks::hooks() const
{
    return m_hooks;
}

void FavoriteHooks::setHooks(const QStringList &hooks)
{
    QStringList normalized;
    for (const QString &hook : hooks)
    {
        const QString clean = hook.trimmed();
        if (clean.isEmpty() || normalized.contains(clean, Qt::CaseInsensitive))
        {
            continue;
        }
        normalized << clean;
    }
    m_hooks = normalized;
    _saveInFile();
}

void FavoriteHooks::addHook(const QString &hook)
{
    const QString clean = hook.trimmed();
    if (clean.isEmpty())
    {
        return;
    }
    for (const QString &existing : m_hooks)
    {
        if (existing.compare(clean, Qt::CaseInsensitive) == 0)
        {
            return; // already saved
        }
    }
    m_hooks << clean;
    _saveInFile();
}

QString FavoriteHooks::asPromptSection() const
{
    if (m_hooks.isEmpty())
    {
        return QString{};
    }
    QStringList bullets;
    for (const QString &hook : m_hooks)
    {
        bullets << QStringLiteral("- %1").arg(hook);
    }
    return QStringLiteral(
        "Hooks that performed well before (style/tone/structure inspiration "
        "— write NEW hooks in a similar spirit, do not just repeat these "
        "verbatim):\n%1\n").arg(bullets.join(QStringLiteral("\n")));
}

void FavoriteHooks::_loadFromFile()
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
        // Only lines actually written as bullets are hooks — everything
        // else (the "# Favorite hooks" heading, the explanatory paragraph,
        // blank lines) must never be mistaken for one (see PromptLessons,
        // which originally got this wrong).
        if (!trimmed.startsWith(QLatin1String("- ")))
        {
            continue;
        }
        const QString hook = trimmed.mid(2).trimmed();
        if (!hook.isEmpty())
        {
            m_hooks << hook;
        }
    }
}

void FavoriteHooks::_saveInFile() const
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    QTextStream stream{&file};
    stream << "# Favorite hooks\n\n"
              "Example hooks proven to perform well — harvested from your "
              "top-ranked videos (Statistics tab), or from anywhere else "
              "you've seen a hook that worked — fed to the CLI as style/"
              "tone inspiration when it suggests new hooks. Edit or delete "
              "lines freely.\n\n";
    for (const QString &hook : m_hooks)
    {
        stream << "- " << hook << "\n";
    }
}
