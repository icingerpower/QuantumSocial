#ifndef FAVORITEVIDEOPROMPTS_H
#define FAVORITEVIDEOPROMPTS_H

#include <QString>
#include <QStringList>

// User-curated example VIDEO prompts — harvested from generations that
// actually rendered like the fast, punchy, "viral-style" short-form videos
// the user wants, as opposed to slow, artsy/cinematic filler — fed to the
// CLI as style reference when it suggests new video prompts (see
// PaneGeneration::_suggestPlan). Same rationale and shape as
// PreferredHashtags/FavoriteHooks: real examples beat guesswork. Persisted
// in favorite_video_prompts.md (working-directory-wide), editable any time
// from the Settings pane, or grown one prompt at a time from the Generation
// page's "Favorite this video prompt" action while browsing a past video
// generation.
class FavoriteVideoPrompts
{
public:
    explicit FavoriteVideoPrompts(const QString &workingDirectory);

    QStringList prompts() const;
    // Replaces the whole list (trimmed, empties and exact-duplicate lines
    // dropped) and persists immediately — used by the Settings editor.
    void setPrompts(const QStringList &prompts);
    // Appends one prompt (trimmed) unless already present — persists
    // immediately. No-op for an empty string.
    void addPrompt(const QString &prompt);
    // All prompts as a ready-to-paste prompt section; empty string when
    // there are none yet.
    QString asPromptSection() const;

private:
    QString m_filePath;
    QStringList m_prompts;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // FAVORITEVIDEOPROMPTS_H
