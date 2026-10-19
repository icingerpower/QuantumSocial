#include "DialogValidateIdea.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

DialogValidateIdea::DialogValidateIdea(const QString &contentType,
                                       const QString &idea,
                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Validate AI Suggested %1").arg(contentType));

    auto *layout = new QVBoxLayout(this);

    auto *labelHeader = new QLabel(
        tr("The AI generated the following %1 idea. You can review or edit it before accepting it into the table:")
            .arg(contentType.toLower()), this);
    labelHeader->setWordWrap(true);
    layout->addWidget(labelHeader);

    auto *labelSource = new QLabel(tr("<b>Source:</b> AI"), this);
    layout->addWidget(labelSource);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setPlainText(idea);
    m_textEdit->setMinimumHeight(120);
    layout->addWidget(m_textEdit, 1);

    auto *buttonBox = new QDialogButtonBox(this);
    auto *buttonAccept = buttonBox->addButton(tr("Add to table"), QDialogButtonBox::AcceptRole);
    buttonAccept->setDefault(true);
    buttonBox->addButton(tr("Discard"), QDialogButtonBox::RejectRole);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    layout->addWidget(buttonBox);

    resize(520, 320);
}

DialogValidateIdea::~DialogValidateIdea() = default;

QString DialogValidateIdea::ideaText() const
{
    return m_textEdit->toPlainText();
}
