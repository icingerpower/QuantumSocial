#ifndef FAVORITEVIDEOPROMPTS_H
#define FAVORITEVIDEOPROMPTS_H

#include <QAbstractTableModel>
#include <QList>
#include <QString>
#include <QStringList>

// User-curated example VIDEO prompts — harvested from generations that
// actually rendered like the fast, punchy, "viral-style" short-form videos
// the user wants, as opposed to slow, artsy/cinematic filler — fed to the
// CLI as style reference when it suggests new video prompts (see
// PaneGeneration::_suggestPlan). Same rationale and shape as
// PreferredHashtags/FavoriteHooks: real examples beat guesswork. Persisted
// in favorite_video_prompts.md (working-directory-wide), editable any time
// from the Inspiration pane, or grown one prompt at a time from the Generation
// page's "Favorite this video prompt" action while browsing a past video
// generation.
//
// Displayed in a QTableView with columns:
//   0 — Video Prompt
//   1 — Human / AI source
class FavoriteVideoPrompts : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_PROMPT{0};
    static const int IND_SOURCE{1};
    static const QString SOURCE_HUMAN;
    static const QString SOURCE_AI;

    explicit FavoriteVideoPrompts(const QString &workingDirectory, QObject *parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

    QStringList prompts(bool onlyHuman = false) const;
    // Replaces the whole list (trimmed, empties and exact-duplicate lines
    // dropped) and persists immediately.
    void setPrompts(const QStringList &prompts);
    // Appends one prompt (trimmed) unless already present — persists
    // immediately. No-op for an empty string.
    void addPrompt(const QString &prompt, const QString &source = QStringLiteral("Human"));
    void addRow(const QString &source = QStringLiteral("Human"));
    void removePrompt(int row);

    // All prompts as a ready-to-paste prompt section; empty string when
    // there are none yet.
    QString asPromptSection(bool onlyHuman = false) const;

private:
    struct Item
    {
        QString prompt;
        QString source;
    };

    static const QStringList HEADER;
    QString m_filePath;
    QList<Item> m_items;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // FAVORITEVIDEOPROMPTS_H
