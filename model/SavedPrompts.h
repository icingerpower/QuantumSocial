#ifndef SAVEDPROMPTS_H
#define SAVEDPROMPTS_H

#include <QList>
#include <QString>

// User-authored, reusable content prompts — named templates the user writes
// once (in DialogGenerationPlan's prompt editor, any strategy tab, any
// content kind — image/slideshow/video prompts share ONE library since most
// scene descriptions work for any of them) and saves under a name, to load
// back and re-edit later instead of retyping.
//
// Exists because CLI-authored prompts kept drifting off-brief (wrong
// outfit, changed background, different product) — the user writes the
// prompt directly now; the CLI's role is reduced to preselecting/filtering
// A/B properties and suggesting hooks (see PaneGeneration::_suggestPlan).
//
// Persisted in saved_prompts.md (working-directory-wide, shared across
// projects) — editable by hand at any time; picked up again on next launch.
class SavedPrompts
{
public:
    struct Entry
    {
        QString name;
        QString prompt;
    };

    explicit SavedPrompts(const QString &workingDirectory);

    QList<Entry> entries() const;
    // Adds a new entry, or overwrites the existing one with the same name
    // (case-insensitive). The generation plan's Edit action uses this to
    // persist changes to the selected saved prompt.
    // No-op if either name or prompt is empty (after trimming).
    void savePrompt(const QString &name, const QString &prompt);

private:
    QString m_filePath;
    QList<Entry> m_entries;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // SAVEDPROMPTS_H
