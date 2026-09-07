#ifndef DIALOGREVIEWNEWPROPERTIES_H
#define DIALOGREVIEWNEWPROPERTIES_H

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QCheckBox;

// Gate shown whenever the suggestion CLI proposes brand-new A/B properties
// (or new values under an existing one, see PaneGeneration::_suggestPlan) —
// nothing in "newProperties" reaches the catalog, or the plan, without
// being explicitly approved here first.
//
// Exists because the CLI kept inventing properties that are already fixed/
// visible in the reference product photo itself (heel height, shoe
// silhouette, finish...) — pure prompt bloat with no real A/B value, to the
// point long prompts started getting partially ignored by the video
// backend. Every proposal starts UNCHECKED: approving one is a deliberate
// opt-in, not a "confirm everything" formality — Cancel (or leaving
// everything unchecked) discards ALL of them, exactly like never having
// been suggested.
class DialogReviewNewProperties : public QDialog
{
    Q_OBJECT

public:
    // One property block the CLI proposed in this call — propertyIndex is
    // this proposal's position in the ORIGINAL "newProperties" JSON array,
    // handed back unchanged in approvedIndices() so the caller can map
    // straight back to it.
    struct Proposal
    {
        int propertyIndex = -1;
        QString propertyName;
        // Ready-to-display "name — fragment" lines, one per proposed value,
        // already formatted by the caller (this dialog does no JSON parsing
        // of its own).
        QString valuesSummary;
    };

    explicit DialogReviewNewProperties(const QList<Proposal> &proposals,
                                       QWidget *parent = nullptr);
    ~DialogReviewNewProperties();

    // propertyIndex of every proposal the user checked, in the same order
    // as the constructor's list.
    QList<int> approvedIndices() const;

private:
    QList<QPair<int, QCheckBox *>> m_checkBoxes;
};

#endif // DIALOGREVIEWNEWPROPERTIES_H
