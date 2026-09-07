#include "DialogReviewNewProperties.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

DialogReviewNewProperties::DialogReviewNewProperties(const QList<Proposal> &proposals,
                                                     QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Review new A/B properties"));

    auto *layout = new QVBoxLayout{this};
    auto *hint = new QLabel{
        tr("The CLI suggests these new A/B properties. Nothing here is "
           "added unless you tick it — leave one unticked (or Cancel) to "
           "discard it entirely, as if it had never been suggested."), this};
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *scrollArea = new QScrollArea{this};
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto *scrollContent = new QWidget{scrollArea};
    auto *scrollLayout = new QVBoxLayout{scrollContent};
    for (const Proposal &proposal : proposals)
    {
        auto *checkBox = new QCheckBox{proposal.propertyName, scrollContent};
        checkBox->setChecked(false);
        scrollLayout->addWidget(checkBox);
        auto *valuesLabel = new QLabel{proposal.valuesSummary, scrollContent};
        valuesLabel->setWordWrap(true);
        valuesLabel->setContentsMargins(24, 0, 0, 8);
        scrollLayout->addWidget(valuesLabel);
        m_checkBoxes << qMakePair(proposal.propertyIndex, checkBox);
    }
    scrollLayout->addStretch(1);
    scrollArea->setWidget(scrollContent);
    layout->addWidget(scrollArea, 1);

    auto *buttonBox = new QDialogButtonBox{
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);

    resize(560, 420);
}

DialogReviewNewProperties::~DialogReviewNewProperties() = default;

QList<int> DialogReviewNewProperties::approvedIndices() const
{
    QList<int> approved;
    for (const auto &entry : m_checkBoxes)
    {
        if (entry.second->isChecked())
        {
            approved << entry.first;
        }
    }
    return approved;
}
