#ifndef DIALOGHOOKS_H
#define DIALOGHOOKS_H

#include <QDialog>
#include <QList>
#include <QPair>
#include <QStringList>

class AbstractCli;
class QTableWidgetItem;

QT_BEGIN_NAMESPACE
namespace Ui { class DialogHooks; }
QT_END_NAMESPACE

// Shown after the content was generated (the moment the copy-paste to the
// platforms happens), and reopenable at any time from the Generation pane:
// the suggested hooks with their descriptions+hashtags in an editable table.
//
// The hook and the description are picked INDEPENDENTLY (click a cell in
// each column — they do not need to be on the same row); the chosen cells
// are highlighted. Every description carries the video's short code tag
// (e.g. "#qs0001"), which maps back to the exact property values the video
// was generated with (TableVideos::recordFromShortCode).
//
// Users can also regenerate more hooks using the selected CLI, or add,
// duplicate, and edit hooks manually.
class DialogHooks : public QDialog
{
    Q_OBJECT

public:
    struct Context
    {
        QString keyword;
        QString hookIdea;
        QString videoFormatLabel;
        QString videoPrompt;
        QStringList preferredHashtags;
        QString favoriteHooksPrompt;
        QString codeTag;
        QString workingDir;
        QList<AbstractCli *> availableClis;
        AbstractCli *selectedCli = nullptr;
    };

    // codeTag: e.g. "#qs0001"; empty when no video record exists.
    explicit DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const QString &codeTag,
                         QWidget *parent = nullptr);

    explicit DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const Context &context,
                         QWidget *parent = nullptr);
    ~DialogHooks();

    // Re-applies a previously saved pick (matches cell texts).
    void preselect(const QString &hook, const QString &description);

    QString selectedHook() const;
    QString selectedDescription() const;
    QList<QPair<QString, QString>> allHooks() const;
    AbstractCli *selectedCli() const;

private slots:
    void _onCellClicked(int row, int column);
    void _onItemChanged(QTableWidgetItem *item);
    void _copyHook();
    void _copyDescription();
    void _addHook();
    void _duplicateHook();
    void _editHook();
    void _deleteHook();
    void _regenerateHooks();
    void _showContextMenu(const QPoint &pos);

private:
    Ui::DialogHooks *ui;
    Context m_context;
    int m_hookRow = -1;
    int m_descriptionRow = -1;
    bool m_updatingHighlights = false;

    void _initUi(const QList<QPair<QString, QString>> &hooks);
    QString _cellText(int row, int column) const;
    void _applyHighlights();
    void _insertHookRow(int row, const QString &hook, const QString &description);
    int _targetRow() const;
    void _updateButtonStates();
    QString _buildRegenerationPrompt() const;
};

#endif // DIALOGHOOKS_H
