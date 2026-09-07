#ifndef PROMPTLESSONS_H
#define PROMPTLESSONS_H

#include <QString>
#include <QStringList>

// A growing, working-directory-wide knowledge base of concrete pitfalls
// discovered from real generation failures (a video clip refused by the
// content check, an image that failed its brief check...), persisted in
// prompt_lessons.md so it survives restarts and stays human-readable/
// editable. Injected into every future prompt-writing/generation call (see
// PaneGeneration::_suggestPlan, ImageGeneratorCli::generate(),
// VideoGenerationWorkflow) so the system gets better at avoiding known
// failure modes over time instead of only reacting to them in the moment —
// the same mistake (e.g. "a full-body shot shrinks the product to nothing")
// doesn't have to be rediscovered generation after generation.
class PromptLessons
{
public:
    explicit PromptLessons(const QString &workingDirectory);

    QStringList lessons() const;
    // Appends a new lesson (trimmed, length-capped) unless an existing one
    // is already near-identical (case-insensitive exact match) — persists
    // immediately. No-op for an empty string. Oldest lessons are dropped
    // once the list grows past a sane bound, on the assumption that recent
    // failures are the most relevant ones to keep front of mind.
    void addLesson(const QString &lesson);
    // All lessons as a ready-to-paste bullet-list prompt section; empty
    // string when there are none yet.
    QString asPromptSection() const;

private:
    QString m_filePath;
    QStringList m_lessons;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // PROMPTLESSONS_H
