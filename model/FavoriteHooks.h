#ifndef FAVORITEHOOKS_H
#define FAVORITEHOOKS_H

#include <QString>
#include <QStringList>

// User-curated example hooks — harvested from top-ranked videos (the
// user's own, or ones seen elsewhere) — fed to the CLI as style/tone
// inspiration when it suggests new hooks (see PaneGeneration::_suggestPlan),
// the same way PreferredHashtags seeds hashtag choices with real data
// instead of guesswork. Persisted in favorite_hooks.md (working-directory-
// wide), editable any time from the Settings pane, or grown one hook at a
// time from the Generation page's "Favorite this hook" action while
// browsing past generations.
class FavoriteHooks
{
public:
    explicit FavoriteHooks(const QString &workingDirectory);

    QStringList hooks() const;
    // Replaces the whole list (trimmed, empties and exact-duplicate lines
    // dropped) and persists immediately — used by the Settings editor.
    void setHooks(const QStringList &hooks);
    // Appends one hook (trimmed) unless already present — persists
    // immediately. No-op for an empty string.
    void addHook(const QString &hook);
    // All hooks as a ready-to-paste prompt section; empty string when there
    // are none yet.
    QString asPromptSection() const;

private:
    QString m_filePath;
    QStringList m_hooks;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // FAVORITEHOOKS_H
