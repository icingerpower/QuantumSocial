#ifndef PREFERREDHASHTAGS_H
#define PREFERREDHASHTAGS_H

#include <QAbstractTableModel>
#include <QList>
#include <QString>
#include <QStringList>

// The user's own curated hashtag list, persisted in the working directory
// (preferred_hashtags.json, editable any time from the Inspiration pane).
// Exists because CLI-suggested hashtags are pure guesswork with no
// engagement data behind them — once the user has hashtags they trust (or
// specifically want to test), the hook-generation step in
// PaneGeneration::_suggestPlan is told to pick from this list instead of
// inventing new ones.
//
// Displayed in a QTableView with columns:
//   0 — Hashtag
//   1 — Human / AI source
class PreferredHashtags : public QAbstractTableModel
{
    Q_OBJECT

public:
    static const int IND_TAG{0};
    static const int IND_SOURCE{1};
    static const QString SOURCE_HUMAN;
    static const QString SOURCE_AI;

    explicit PreferredHashtags(const QString &workingDirectory, QObject *parent = nullptr);

    // QAbstractTableModel interface
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

    QStringList hashtags(bool onlyHuman = false) const;
    QString asPromptSection(bool onlyHuman = false) const;

    // Prompt section explaining the viral 4+1 hashtag laddering strategy
    // (pick 3-4 existing validated tags + at most 1 high-volume discovery tag,
    // capped at 5 hashtags total, with target keyword SEO anchor).
    QString viralHashtagPromptSection(const QString &keyword = QString{}) const;
    static QString viralHashtagPromptSection(const QStringList &preferredTags, const QString &keyword = QString{});

    // Optimizes a post description's trailing hashtags to adhere to the
    // viral 4+1 strategy (up to 4 validated preferred/keyword tags + at most 1
    // high-volume discovery tag, capped at 5 hashtags total).
    // If a new discovery tag is accepted, it is automatically registered into
    // this PreferredHashtags model with source "AI" and saved to disk.
    QString optimizeDescriptionHashtags(const QString &rawDescription, const QString &keyword = QString{});

    // Static helper that does the optimization without requiring a model instance.
    // Newly accepted discovery tags are appended to outNewTags if provided.
    static QString optimizeDescriptionHashtags(const QString &rawDescription,
                                              const QString &keyword,
                                              const QStringList &preferredTags,
                                              QStringList *outNewTags = nullptr);

    // Normalizes (ensures a leading '#', trims, case-insensitive dedupe,
    // drops empties) and persists immediately.
    void setHashtags(const QStringList &hashtags);

    void addHashtag(const QString &tag, const QString &source = QStringLiteral("Human"));
    void addRow(const QString &source = QStringLiteral("Human"));
    void removeHashtag(int row);

    // Splits free-form text (newlines, spaces or commas as separators) into
    // a plain list — used by both the Inspiration editor and the first-run
    // setup prompt so typed input is parsed identically everywhere.
    static QStringList parse(const QString &freeformText);

private:
    struct Item
    {
        QString tag;
        QString source;
    };

    static const QStringList HEADER;
    QString m_filePath;
    QList<Item> m_items;

    void _loadFromFile();
    void _saveInFile() const;
};

#endif // PREFERREDHASHTAGS_H
