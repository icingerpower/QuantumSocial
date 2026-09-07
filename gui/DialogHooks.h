#ifndef DIALOGHOOKS_H
#define DIALOGHOOKS_H

#include <QDialog>
#include <QList>
#include <QPair>

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
class DialogHooks : public QDialog
{
    Q_OBJECT

public:
    // codeTag: e.g. "#qs0001"; empty when no video record exists (appended
    // to every description so the recipe stays identifiable).
    explicit DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const QString &codeTag,
                         QWidget *parent = nullptr);
    ~DialogHooks();

    // Re-applies a previously saved pick (matches cell texts).
    void preselect(const QString &hook, const QString &description);

    QString selectedHook() const;
    QString selectedDescription() const;

private slots:
    void _onCellClicked(int row, int column);
    void _copyHook();
    void _copyDescription();

private:
    Ui::DialogHooks *ui;
    int m_hookRow = 0;
    int m_descriptionRow = 0;

    QString _cellText(int row, int column) const;
    void _applyHighlights();
};

#endif // DIALOGHOOKS_H
