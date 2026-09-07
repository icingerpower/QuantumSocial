#ifndef DIALOGGENERATIONPLAN_H
#define DIALOGGENERATIONPLAN_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPair>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QUuid>

QT_BEGIN_NAMESPACE
namespace Ui { class DialogGenerationPlan; }
QT_END_NAMESPACE

class AbstractCli;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QStackedWidget;
class SavedPrompts;

// Lets the user compose the generation plan — THE USER WRITES THE PROMPT
// THEMSELVES here; CLI-authored prompts kept drifting off-brief (wrong
// outfit, changed background/product) with no reliable way to pin them down
// through instructions alone. The CLI's remaining role (see
// PaneGeneration::_suggestPlan) is reduced to preselecting/filtering the A/B
// properties and suggesting hooks — never writing prompt text.
//
// One CHECKABLE group per generation option — "One image", "Several images
// (slideshow)", and one per registered AbstractVideoGenerator backend (new
// backends appear automatically). Each group offers 3 FIXED STRATEGY SLOTS,
// radio-selected (one active at a time) — purely organizational labels for
// the user's own 3 independent prompt slots, e.g.:
//   0 - Animate/keep the source: a minimal-change take.
//   1 - Reimagine: same product, a new model/background/outfit.
//   2 - Creative: whatever else you want to try.
// Each strategy is a LEFT (editable prompt text, a saved-prompt load/save
// row, and the property picker) / RIGHT (live "Final prompt" preview)
// split. Every property shows ALL its sibling values in a combo box — not
// just the A/B sampler's weighted pick, which is only the pre-selected
// default now; real manual override is possible. Checking/unchecking a
// property, changing its selected value, or editing the prompt text all
// update the preview immediately.
//
// Saved prompts (SavedPrompts, one shared library across every strategy
// slot/content kind — most scene descriptions work for any of them):
// "Load" pulls a named prompt's text into the editor, "Save..." names (or
// renames-over, to edit an existing one) the current editor text into the
// library — available from every strategy's row.
//
// A property is IDENTIFIED BY THE SAME id across every section at the same
// strategy index (image/slideshow/video strategy #1 all reference the same
// sampled recipe for that slot) — toggling or re-picking it in one place
// stays in sync everywhere it appears, so the recorded recipe is coherent
// across every option generated from the same strategy.
//
// The "One image" and "Several images" groups additionally show a CLI
// combo (image generation is CLI-driven — see AbstractImageGenerator).
// Video groups don't need one: AbstractVideoGenerator backends are
// self-contained.
// Several options can be ticked; ALL of them run independently when
// generation starts. The last ticked set (and chosen CLIs/strategies) is
// remembered and preselected next time.
//
// Hooks/descriptions are deliberately NOT here: they are picked in
// DialogHooks after the content is generated, when it is worth copy-pasting.
class DialogGenerationPlan : public QDialog
{
    Q_OBJECT

public:
    // One ticked video backend's pick: its own prompt AND its own recipe
    // (a different backend may have a different strategy active, so each
    // carries its own property ids rather than sharing one).
    struct VideoPick
    {
        QString generatorId;
        QString prompt;
        QList<QUuid> propertyValueIds;
    };

    struct Plan
    {
        bool oneImage = false;
        QString imagePrompt;
        QList<QUuid> imagePropertyValueIds;
        bool slideshow = false;
        QString slideshowPrompt;
        QList<QUuid> slideshowPropertyValueIds;
        QList<VideoPick> videos;
    };

    // One value a property could take.
    struct PlanPropertyOption
    {
        QUuid id;
        QString name;      // e.g. "Street"
        QString fragment;  // the prompt fragment merged into the final prompt
    };

    // One A/B-test property as shown under one strategy: every sibling
    // value pickable via combo box, which one starts pre-selected (the A/B
    // sampler's weighted pick), and whether it is included at all (the
    // CLI's relevance filter, the user has the final say).
    struct PlanProperty
    {
        QUuid propertyId;
        QString propertyName;   // e.g. "Background"
        QList<PlanPropertyOption> options;
        QUuid selectedOptionId;
        bool checked = true;
    };

    // propertiesPerStrategy[i] is the recipe for strategy i — the same
    // strategy's list is reused identically across every section's page at
    // that index, but each index has its OWN independently-sampled values.
    // Every strategy page's prompt editor starts EMPTY (the user writes it)
    // — savedPrompts offers a starting point via its "Load" row.
    explicit DialogGenerationPlan(const QList<QList<PlanProperty>> &propertiesPerStrategy,
                                  const QList<AbstractCli *> &availableClis,
                                  SavedPrompts *savedPrompts,
                                  const QString &videoFormatLabel,
                                  QWidget *parent = nullptr);
    ~DialogGenerationPlan();

    // Every ticked option's prompt AND the A/B-test values confirmed for IT
    // specifically (the VideoRecord of that option's generation references
    // only its own recipe, not a plan-wide shared one).
    Plan plan() const;
    // The CLI chosen for the "One image" / "Several images" options;
    // nullptr when that option is not ticked or no image-capable CLI exists.
    AbstractCli *imageCli() const;
    AbstractCli *slideshowCli() const;

    void accept() override;

private:
    static constexpr int kStrategyCount = 3;

    struct StrategyPage;  // defined below — PropertyRow only needs a pointer to it

    // One property row's live widgets under one strategy page. `page` is a
    // back-reference to the StrategyPage this row's widgets actually live
    // in, so a sync update (a DIFFERENT row of the same synced property,
    // possibly in another section) knows whose preview to recompute.
    struct PropertyRow
    {
        QUuid propertyId;
        QCheckBox *checkBox = nullptr;
        QComboBox *valueCombo = nullptr;  // option id at Qt::UserRole, fragment at Qt::UserRole + 1
        QSharedPointer<StrategyPage> page;
    };

    // propertyId alone is NOT a unique sync key: PropertySampler keeps the
    // SAME property id across all 3 strategies (only the SAMPLED VALUE
    // differs per strategy), but strategy 0's pick for "Background" must
    // stay independent from strategy 1's — so the sync key is scoped to
    // (strategyIndex, propertyId), shared only across SECTIONS at that same
    // index (image/slideshow/video strategy #1 all show the same recipe).
    static QString _syncKey(int strategyIndex, const QUuid &propertyId);

    // One of the 3 fixed strategies within one option section: the
    // editable base prompt (left), its property rows (left, below the
    // prompt), and the live Final Prompt preview (right).
    struct StrategyPage
    {
        QPlainTextEdit *promptEdit = nullptr;
        QLabel *previewLabel = nullptr;
        QList<PropertyRow> propertyRows;
    };

    // Heap-allocated (via QSharedPointer) so pointers into a StrategyPage
    // handed to lambdas stay valid regardless of how the enclosing
    // OptionSection is copied/moved (it is built by value in _makeSection
    // and then copied into its final member, e.g. m_imageSection).
    struct OptionSection
    {
        QGroupBox *group = nullptr;
        QButtonGroup *strategyButtons = nullptr;
        QStackedWidget *strategyStack = nullptr;
        QList<QSharedPointer<StrategyPage>> strategies;  // size kStrategyCount
        QComboBox *cliCombo = nullptr;   // only set for image-generating sections
    };

    Ui::DialogGenerationPlan *ui;
    SavedPrompts *m_savedPrompts;
    OptionSection m_imageSection;
    OptionSection m_slideshowSection;
    QList<QPair<QString, OptionSection>> m_videoSections;  // (generator id, section)
    // Every PropertyRow sharing a given _syncKey() — i.e. every section's
    // page at the SAME strategy index showing the SAME property — kept in
    // sync when one of them is toggled or re-picked.
    QHash<QString, QList<PropertyRow>> m_propertyRows;
    QHash<QString, bool> m_propertyChecked;
    QHash<QString, QUuid> m_selectedOption;  // sync key -> chosen option id
    // Every strategy page's saved-prompt combo, across every section — kept
    // in sync (repopulated) whenever a Save adds/overwrites an entry in the
    // shared SavedPrompts library, so a name saved from one tab is
    // immediately loadable from every other tab too.
    QList<QComboBox *> m_savedPromptCombos;

    OptionSection _makeSection(const QString &title,
                               const QList<QList<PlanProperty>> &propertiesPerStrategy,
                               bool checked);
    // Repopulates every tracked saved-prompt combo from m_savedPrompts,
    // preserving each combo's current text selection where it still exists.
    void _refreshSavedPromptCombos();
    // Inserts a "CLI:" combo (image-capable CLIs only) at the top of the
    // section's group, restoring the last choice saved under settingsKey.
    void _addCliCombo(OptionSection &section, const QList<AbstractCli *> &availableClis,
                      const QString &settingsKey);
    static AbstractCli *_comboCli(const QComboBox *combo);
    static int _activeStrategyIndex(const OptionSection &section);
    // The active strategy's prompt text (possibly edited by the user).
    static QString _sectionPrompt(const OptionSection &section);
    // The active strategy's checked properties, using each one's currently
    // SELECTED option (not necessarily the sampler's default pick).
    static QList<QUuid> _sectionCheckedPropertyValues(const OptionSection &section);
    static QString _computeFinal(const StrategyPage &strategy);
    void _recomputeFinal(StrategyPage &strategy);
    void _onPropertyCheckToggled(const QString &syncKey, bool checked);
    void _onPropertyValueChanged(const QString &syncKey, const QUuid &optionId);
    void _updateOkButton();
};

#endif // DIALOGGENERATIONPLAN_H
