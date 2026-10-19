#ifndef FAVORITEHOOKS_H
#define FAVORITEHOOKS_H

#include <QAbstractTableModel>
#include <QList>
#include <QString>
#include <QStringList>

// User-curated example hooks — harvested from top-ranked videos (the
// user's own, or ones seen elsewhere) — fed to the CLI as style/tone
// inspiration when it suggests new hooks (see PaneGeneration::_suggestPlan),
// the same way PreferredHashtags seeds hashtag choices with real data
// instead of guesswork. Persisted in favorite_hooks.md (working-directory-
// wide), editable any time from the Inspiration pane, or grown one hook at a
// time from the Generation page's "Favorite this hook" action while
// browsing past generations.
//
// Displayed in a QTableView with columns:
//   0 — Hook
//   1 — Human / AI source
class FavoriteHooks : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_HOOK{0};
    static const int IND_SOURCE{1};
    static const QString SOURCE_HUMAN;
    static const QString SOURCE_AI;

    explicit FavoriteHooks(const QString &workingDirectory, QObject *parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

    QStringList hooks(bool onlyHuman = false) const;
    // Replaces the whole list (trimmed, empties and exact-duplicate lines
    // dropped) and persists immediately.
    void setHooks(const QStringList &hooks);
    // Appends one hook (trimmed) unless already present — persists
    // immediately. No-op for an empty string.
    void addHook(const QString &hook, const QString &source = QStringLiteral("Human"));
    void addRow(const QString &source = QStringLiteral("Human"));
    void removeHook(int row);

    // All hooks as a ready-to-paste prompt section; empty string when there
    // are none yet.
    QString asPromptSection(bool onlyHuman = false) const;

private:
    struct Item
    {
        QString hook;
        QString source;
    };

    static const QStringList HEADER;
    QString m_filePath;
    QList<Item> m_items;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // FAVORITEHOOKS_H
