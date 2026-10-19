#ifndef PANEINSPIRATION_H
#define PANEINSPIRATION_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneInspiration; }
QT_END_NAMESPACE

class PreferredHashtags;
class FavoriteHooks;
class FavoriteVideoPrompts;
class AvailableCliTable;
class AvailableCliList;
class QTableView;
class QComboBox;

// Inspiration tab: Preferred hashtags, favorite hooks, and favorite video
// prompts in editable QTableViews with human / AI origin differentiation.
// Each content type provides an AI CLI selector (backed by common's
// AvailableCliList) and an editable prompt asking for similar ideas. Generated
// ideas require human validation before being added to the table.
class PaneInspiration : public QWidget
{
    Q_OBJECT

public:
    enum class ContentType
    {
        Hashtags,
        Hooks,
        VideoPrompts,
    };

    explicit PaneInspiration(PreferredHashtags *hashtags,
                             FavoriteHooks *favoriteHooks,
                             FavoriteVideoPrompts *favoriteVideoPrompts,
                             AvailableCliTable *availableClis,
                             QWidget *parent = nullptr);
    ~PaneInspiration() override;

private slots:
    void _addHashtagRow();
    void _removeSelectedHashtags();
    void _addHookRow();
    void _removeSelectedHooks();
    void _addPromptRow();
    void _removeSelectedPrompts();

    void _generateHashtagIdea();
    void _generateHookIdea();
    void _generatePromptIdea();

private:
    void _setupViews();
    void _setupDefaultPrompts();
    void _loadSettings();
    void _restoreSelectedClis();
    void _saveCliIfUserChanged(QComboBox *combo, const QString &key, QString &savedCli);
    void _generateIdea(ContentType type);
    void _removeSelectedRows(QTableView *tableView);
    static QString _cleanOutput(const QString &raw);

    Ui::PaneInspiration *ui;
    PreferredHashtags *m_hashtags;
    FavoriteHooks *m_favoriteHooks;
    FavoriteVideoPrompts *m_favoriteVideoPrompts;
    AvailableCliTable *m_availableCliTable;
    AvailableCliList *m_cliList;
    QString m_savedCliHashtag;
    QString m_savedCliHook;
    QString m_savedCliPrompt;
};

#endif // PANEINSPIRATION_H
