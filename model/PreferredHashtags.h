#ifndef PREFERREDHASHTAGS_H
#define PREFERREDHASHTAGS_H

#include <QString>
#include <QStringList>

// The user's own curated hashtag list, persisted in the working directory
// (preferred_hashtags.json, editable any time from the Settings pane).
// Exists because CLI-suggested hashtags are pure guesswork with no
// engagement data behind them — once the user has hashtags they trust (or
// specifically want to test), the hook-generation step in
// PaneGeneration::_suggestPlan is told to pick from this list instead of
// inventing new ones.
class PreferredHashtags
{
public:
    explicit PreferredHashtags(const QString &workingDirectory);

    QStringList hashtags() const;
    // Normalizes (ensures a leading '#', trims, case-insensitive dedupe,
    // drops empties) and persists immediately.
    void setHashtags(const QStringList &hashtags);

    // Splits free-form text (newlines, spaces or commas as separators) into
    // a plain list — used by both the Settings editor and the first-run
    // setup prompt so typed input is parsed identically everywhere.
    static QStringList parse(const QString &freeformText);

private:
    QString m_filePath;
    QStringList m_hashtags;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // PREFERREDHASHTAGS_H
