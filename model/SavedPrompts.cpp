#include "SavedPrompts.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

SavedPrompts::SavedPrompts(const QString &workingDirectory)
    : m_filePath(QDir(workingDirectory).absoluteFilePath(QStringLiteral("saved_prompts.md")))
{
    _loadFromFile();
}

QList<SavedPrompts::Entry> SavedPrompts::entries() const
{
    return m_entries;
}

void SavedPrompts::savePrompt(const QString &name, const QString &prompt)
{
    const QString cleanName = name.trimmed();
    const QString cleanPrompt = prompt.trimmed();
    if (cleanName.isEmpty() || cleanPrompt.isEmpty())
    {
        return;
    }
    for (Entry &entry : m_entries)
    {
        if (entry.name.compare(cleanName, Qt::CaseInsensitive) == 0)
        {
            entry.prompt = cleanPrompt;
            _saveInFile();
            return;
        }
    }
    m_entries << Entry{cleanName, cleanPrompt};
    _saveInFile();
}

void SavedPrompts::_loadFromFile()
{
    QFile file{m_filePath};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    QTextStream stream{&file};
    QString currentName;
    QStringList currentBody;
    const auto flush = [this, &currentName, &currentBody]() {
        const QString prompt = currentBody.join(QLatin1Char('\n')).trimmed();
        if (!currentName.isEmpty() && !prompt.isEmpty())
        {
            m_entries << Entry{currentName, prompt};
        }
        currentBody.clear();
    };
    for (const QString &rawLine : stream.readAll().split(QLatin1Char('\n')))
    {
        if (rawLine.startsWith(QLatin1String("## ")))
        {
            flush();
            currentName = rawLine.mid(3).trimmed();
            continue;
        }
        if (currentName.isEmpty() || rawLine.startsWith(QLatin1String("# ")))
        {
            // Top-level heading / explanatory paragraph before the first
            // entry — never part of a prompt's body (see PromptLessons,
            // which originally mistook exactly this for real data).
            continue;
        }
        currentBody << rawLine;
    }
    flush();
}

void SavedPrompts::_saveInFile() const
{
    QFile file{m_filePath};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    QTextStream stream{&file};
    stream << "# Saved prompts\n\n"
              "Reusable prompts you wrote and saved from the generation plan "
              "dialog (any strategy tab, any content kind — one shared "
              "library). Load one back from there to reuse or edit; you can "
              "also edit the text below by hand, it will be picked up on "
              "next launch too. Each entry is a '## name' heading followed "
              "by its prompt text.\n\n";
    for (const Entry &entry : m_entries)
    {
        stream << "## " << entry.name << "\n" << entry.prompt << "\n\n";
    }
}
