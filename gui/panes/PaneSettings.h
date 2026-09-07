#ifndef PANESETTINGS_H
#define PANESETTINGS_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneSettings; }
QT_END_NAMESPACE

class AvailableCliTable;
class TableGenerationSettings;
class PreferredHashtags;
class FavoriteHooks;
class FavoriteVideoPrompts;

// Settings tab: the AI CLIs found on this machine (common's
// AvailableCliTable, with async availability + membership checks), the
// video generation settings table (one section per registered backend, rows
// come from each backend's availableSettings), the user's own preferred
// hashtag list, favorite example hooks harvested from top-ranked videos,
// and favorite example video prompts harvested from generations that
// actually rendered like the wanted viral-style videos (all four also
// settable once at working-directory setup — see main.cpp — but editable
// here any time after).
class PaneSettings : public QWidget
{
    Q_OBJECT

public:
    explicit PaneSettings(TableGenerationSettings *generationSettings,
                          PreferredHashtags *hashtags,
                          FavoriteHooks *favoriteHooks,
                          FavoriteVideoPrompts *favoriteVideoPrompts,
                          QWidget *parent = nullptr);
    ~PaneSettings();

private slots:
    // Auto-saves on every edit — a plain-text local write is cheap, and it
    // matches how every other per-workingDir setting in this app persists
    // (no separate "Save" button to forget to click).
    void _saveHashtags();
    void _saveFavoriteHooks();
    void _saveFavoriteVideoPrompts();

private:
    Ui::PaneSettings *ui;
    TableGenerationSettings *m_generationSettings;
    PreferredHashtags *m_hashtags;
    FavoriteHooks *m_favoriteHooks;
    FavoriteVideoPrompts *m_favoriteVideoPrompts;
    AvailableCliTable *m_availableClis;
};

#endif // PANESETTINGS_H
