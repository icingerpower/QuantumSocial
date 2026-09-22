#include "DialogMultiVideoGeneration.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "AbstractCli.h"
#include "MultiVideoStepWidget.h"
#include "model/videogen/AbstractVideoGenerator.h"

DialogMultiVideoGeneration::DialogMultiVideoGeneration(
    const QList<AbstractCli *> &availableClis, QWidget *parent)
    : QDialog(parent)
    , m_availableClis(availableClis)
{
    setWindowTitle(tr("New Multi-Video Generation"));
    resize(980, 850);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // --- Section 1: Global Setup (Project, Steps, Format, CLIs) ---
    auto *groupGlobal = new QGroupBox(tr("1. Multi-Video Configuration"), this);
    auto *layoutGlobal = new QGridLayout(groupGlobal);
    layoutGlobal->setSpacing(8);

    m_editKeyword = new QLineEdit(groupGlobal);
    m_editKeyword->setPlaceholderText(tr("e.g. Summer Handbags Collection"));

    m_spinTotalSteps = new QSpinBox(groupGlobal);
    m_spinTotalSteps->setRange(2, 20);
    m_spinTotalSteps->setValue(3); // 1 intro + 2 products by default

    auto *labelStepsDesc = new QLabel(tr("(1 intro step + %1 after-intro product steps)")
        .arg(m_spinTotalSteps->value() - 1), groupGlobal);
    labelStepsDesc->setStyleSheet(QStringLiteral("color: #888; font-style: italic;"));

    m_comboFormat = new QComboBox(groupGlobal);
    m_comboFormat->addItem(tr("Vertical (9:16) — Reels / TikTok / Shorts"),
                           static_cast<int>(DialogGenerationOptions::VideoFormat::Vertical916));
    m_comboFormat->addItem(tr("Horizontal (16:9) — Landscape"),
                           static_cast<int>(DialogGenerationOptions::VideoFormat::Horizontal169));
    m_comboFormat->addItem(tr("Square (1:1) — Feed post"),
                           static_cast<int>(DialogGenerationOptions::VideoFormat::Square));

    m_comboGenerator = new QComboBox(groupGlobal);
    const auto &generators = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    for (auto it = generators.cbegin(); it != generators.cend(); ++it)
    {
        m_comboGenerator->addItem(it.value()->getName(), it.key());
    }

    m_comboImageCli = new QComboBox(groupGlobal);
    m_comboDecisionCli = new QComboBox(groupGlobal);
    for (AbstractCli *cli : availableClis)
    {
        if (cli->canGenImages())
        {
            m_comboImageCli->addItem(cli->getName(), QVariant::fromValue(cli));
        }
        m_comboDecisionCli->addItem(cli->getName(), QVariant::fromValue(cli));
    }

    layoutGlobal->addWidget(new QLabel(tr("Project keyword / title:"), groupGlobal), 0, 0);
    layoutGlobal->addWidget(m_editKeyword, 0, 1);
    layoutGlobal->addWidget(new QLabel(tr("Total steps:"), groupGlobal), 0, 2);
    auto *stepsLayout = new QHBoxLayout();
    stepsLayout->addWidget(m_spinTotalSteps);
    stepsLayout->addWidget(labelStepsDesc);
    layoutGlobal->addLayout(stepsLayout, 0, 3);

    layoutGlobal->addWidget(new QLabel(tr("Video format:"), groupGlobal), 1, 0);
    layoutGlobal->addWidget(m_comboFormat, 1, 1);
    layoutGlobal->addWidget(new QLabel(tr("Video backend:"), groupGlobal), 1, 2);
    layoutGlobal->addWidget(m_comboGenerator, 1, 3);

    layoutGlobal->addWidget(new QLabel(tr("Image CLI (regeneration):"), groupGlobal), 2, 0);
    layoutGlobal->addWidget(m_comboImageCli, 2, 1);
    layoutGlobal->addWidget(new QLabel(tr("Validation CLI:"), groupGlobal), 2, 2);
    layoutGlobal->addWidget(m_comboDecisionCli, 2, 3);

    mainLayout->addWidget(groupGlobal);

    // --- Section 2: Prompts and Takes (1 for Intro, 1 for After-Intro) ---
    auto *groupPrompts = new QGroupBox(tr("2. Prompts & Takes"), this);
    auto *layoutPrompts = new QGridLayout(groupPrompts);
    layoutPrompts->setSpacing(8);

    m_editIntroPrompt = new QPlainTextEdit(groupPrompts);
    m_editIntroPrompt->setPlaceholderText(tr(
        "Hook & opener video prompt: fast dynamic camera zoom on product collection, bold catchy atmosphere, smooth cinematic lighting..."));
    m_editIntroPrompt->setMaximumHeight(70);

    m_spinIntroTakes = new QSpinBox(groupPrompts);
    m_spinIntroTakes->setRange(1, 10);
    m_spinIntroTakes->setValue(2);

    m_editAfterIntroPrompt = new QPlainTextEdit(groupPrompts);
    m_editAfterIntroPrompt->setPlaceholderText(tr(
        "Product showcase prompt: clean 360 rotation highlighting material quality and fine craftsmanship, elegant backdrop..."));
    m_editAfterIntroPrompt->setMaximumHeight(70);

    m_spinAfterIntroTakes = new QSpinBox(groupPrompts);
    m_spinAfterIntroTakes->setRange(1, 10);
    m_spinAfterIntroTakes->setValue(2);

    m_labelTotalSummary = new QLabel(groupPrompts);
    m_labelTotalSummary->setStyleSheet(QStringLiteral("font-weight: bold; color: #4CAF50;"));

    layoutPrompts->addWidget(new QLabel(tr("Intro step prompt:"), groupPrompts), 0, 0);
    layoutPrompts->addWidget(m_editIntroPrompt, 0, 1);
    layoutPrompts->addWidget(new QLabel(tr("Intro takes:"), groupPrompts), 0, 2);
    layoutPrompts->addWidget(m_spinIntroTakes, 0, 3);

    layoutPrompts->addWidget(new QLabel(tr("After-intro steps prompt:"), groupPrompts), 1, 0);
    layoutPrompts->addWidget(m_editAfterIntroPrompt, 1, 1);
    layoutPrompts->addWidget(new QLabel(tr("Takes per step:"), groupPrompts), 1, 2);
    layoutPrompts->addWidget(m_spinAfterIntroTakes, 1, 3);

    layoutPrompts->addWidget(m_labelTotalSummary, 2, 0, 1, 4);

    mainLayout->addWidget(groupPrompts);

    // --- Section 3: Dynamic Step Images (Scroll Area) ---
    auto *groupSteps = new QGroupBox(tr("3. Step Images (up to 4 images per step, with regeneration options)"), this);
    auto *groupStepsLayout = new QVBoxLayout(groupSteps);

    auto *toolbarLayout = new QHBoxLayout();
    toolbarLayout->addWidget(new QLabel(tr("Default image option:"), groupSteps));
    m_comboDefaultRegen = new QComboBox(groupSteps);
    m_comboDefaultRegen->addItem(tr("Isolate on white"),
                                 static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite));
    m_comboDefaultRegen->addItem(tr("Keep original"),
                                 static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal));
    m_comboDefaultRegen->addItem(tr("Regenerate with CLI"),
                                 static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate));

    const bool hasImageCli = (m_comboImageCli->count() > 0);
    const auto initialDefaultOpt = hasImageCli
        ? AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite
        : AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal;
    const int initialIdx = m_comboDefaultRegen->findData(static_cast<int>(initialDefaultOpt));
    if (initialIdx >= 0)
    {
        m_comboDefaultRegen->setCurrentIndex(initialIdx);
    }
    toolbarLayout->addWidget(m_comboDefaultRegen);

    m_buttonApplyAllRegen = new QPushButton(tr("Apply to all image slots"), groupSteps);
    m_buttonApplyAllRegen->setToolTip(tr("Apply this regeneration option to every image slot across all steps"));
    toolbarLayout->addWidget(m_buttonApplyAllRegen);
    toolbarLayout->addStretch();
    groupStepsLayout->addLayout(toolbarLayout);

    auto *scrollArea = new QScrollArea(groupSteps);
    scrollArea->setWidgetResizable(true);
    auto *scrollContainer = new QWidget(scrollArea);
    m_stepsLayout = new QVBoxLayout(scrollContainer);
    m_stepsLayout->setContentsMargins(4, 4, 4, 4);
    m_stepsLayout->setSpacing(8);
    scrollArea->setWidget(scrollContainer);
    groupStepsLayout->addWidget(scrollArea);

    mainLayout->addWidget(groupSteps, 1);

    // --- Section 4: Dialog Buttons ---
    auto *buttonBox = new QDialogButtonBox(this);
    m_buttonGenerate = buttonBox->addButton(tr("Start Generation"), QDialogButtonBox::AcceptRole);
    buttonBox->addButton(QDialogButtonBox::Cancel);
    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_spinTotalSteps, qOverload<int>(&QSpinBox::valueChanged),
            this, [this, labelStepsDesc](int total) {
        labelStepsDesc->setText(tr("(1 intro step + %1 after-intro product steps)").arg(total - 1));
        _onStepCountChanged(total);
    });

    connect(m_spinIntroTakes, qOverload<int>(&QSpinBox::valueChanged),
            this, &DialogMultiVideoGeneration::_updateTotalCountLabel);
    connect(m_spinAfterIntroTakes, qOverload<int>(&QSpinBox::valueChanged),
            this, &DialogMultiVideoGeneration::_updateTotalCountLabel);

    connect(m_editKeyword, &QLineEdit::textChanged, this, &DialogMultiVideoGeneration::_updateOkButton);
    connect(m_editIntroPrompt, &QPlainTextEdit::textChanged, this, &DialogMultiVideoGeneration::_updateOkButton);
    connect(m_editAfterIntroPrompt, &QPlainTextEdit::textChanged, this, &DialogMultiVideoGeneration::_updateOkButton);
    connect(m_buttonApplyAllRegen, &QPushButton::clicked, this, &DialogMultiVideoGeneration::_applyDefaultRegenToAllSlots);

    _rebuildStepWidgets(m_spinTotalSteps->value());
    _updateTotalCountLabel();
    _updateOkButton();
}

void DialogMultiVideoGeneration::_applyDefaultRegenToAllSlots()
{
    const auto opt = static_cast<AbstractMultiVideoWorkflow::ImageRegenOption>(
        m_comboDefaultRegen->currentData().toInt());
    for (auto *stepWidget : m_stepWidgets)
    {
        stepWidget->setAllSlotsRegenOption(opt);
    }
}

void DialogMultiVideoGeneration::_onStepCountChanged(int totalSteps)
{
    _rebuildStepWidgets(totalSteps);
    _updateTotalCountLabel();
}

void DialogMultiVideoGeneration::_rebuildStepWidgets(int totalSteps)
{
    const auto defaultOpt = m_comboDefaultRegen
        ? static_cast<AbstractMultiVideoWorkflow::ImageRegenOption>(m_comboDefaultRegen->currentData().toInt())
        : AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite;

    // Keep existing widgets if possible, add or remove as needed
    while (m_stepWidgets.size() < totalSteps)
    {
        const int idx = m_stepWidgets.size();
        const bool isIntro = (idx == 0);
        auto *widget = new MultiVideoStepWidget(idx, isIntro, this);
        widget->setDefaultRegenOption(defaultOpt);
        widget->setAllSlotsRegenOption(defaultOpt);
        connect(widget, &MultiVideoStepWidget::changed,
                this, &DialogMultiVideoGeneration::_updateOkButton);
        m_stepWidgets.append(widget);
        m_stepsLayout->addWidget(widget);
    }
    while (m_stepWidgets.size() > totalSteps)
    {
        auto *widget = m_stepWidgets.takeLast();
        m_stepsLayout->removeWidget(widget);
        delete widget;
    }
}

void DialogMultiVideoGeneration::_updateTotalCountLabel()
{
    const int totalSteps = m_spinTotalSteps->value();
    const int introTakes = m_spinIntroTakes->value();
    const int afterIntroSteps = totalSteps - 1;
    const int afterIntroTakes = m_spinAfterIntroTakes->value();
    const int totalVideos = introTakes + (afterIntroSteps * afterIntroTakes);

    m_labelTotalSummary->setText(
        tr("Total: %1 intro video(s) + (%2 steps × %3 takes = %4 videos) = %5 videos total will be generated.")
            .arg(introTakes).arg(afterIntroSteps).arg(afterIntroTakes)
            .arg(afterIntroSteps * afterIntroTakes).arg(totalVideos));
}

void DialogMultiVideoGeneration::_updateOkButton()
{
    const bool hasKeyword = !m_editKeyword->text().trimmed().isEmpty();
    const bool hasIntroPrompt = !m_editIntroPrompt->toPlainText().trimmed().isEmpty();
    const bool hasAfterIntroPrompt = !m_editAfterIntroPrompt->toPlainText().trimmed().isEmpty();

    // At least one image across all steps
    bool hasAnyImage = false;
    for (auto *widget : m_stepWidgets)
    {
        if (!widget->imageInputs().isEmpty())
        {
            hasAnyImage = true;
            break;
        }
    }

    m_buttonGenerate->setEnabled(hasKeyword && hasIntroPrompt && hasAfterIntroPrompt && hasAnyImage);
    if (!hasAnyImage)
    {
        m_buttonGenerate->setToolTip(tr("Please select at least one image in the steps below."));
    }
    else if (!hasKeyword)
    {
        m_buttonGenerate->setToolTip(tr("Please specify a project keyword or title."));
    }
    else if (!hasIntroPrompt || !hasAfterIntroPrompt)
    {
        m_buttonGenerate->setToolTip(tr("Please fill in both intro and after-intro prompts."));
    }
    else
    {
        m_buttonGenerate->setToolTip(QString{});
    }
}

QString DialogMultiVideoGeneration::keyword() const
{
    return m_editKeyword->text().trimmed();
}

int DialogMultiVideoGeneration::totalSteps() const
{
    return m_spinTotalSteps->value();
}

QString DialogMultiVideoGeneration::introPrompt() const
{
    return m_editIntroPrompt->toPlainText().trimmed();
}

int DialogMultiVideoGeneration::introTakes() const
{
    return m_spinIntroTakes->value();
}

QString DialogMultiVideoGeneration::afterIntroPrompt() const
{
    return m_editAfterIntroPrompt->toPlainText().trimmed();
}

int DialogMultiVideoGeneration::afterIntroTakes() const
{
    return m_spinAfterIntroTakes->value();
}

QList<QList<AbstractMultiVideoWorkflow::ImageInput>> DialogMultiVideoGeneration::stepImages() const
{
    QList<QList<AbstractMultiVideoWorkflow::ImageInput>> result;
    for (auto *widget : m_stepWidgets)
    {
        result.append(widget->imageInputs());
    }
    return result;
}

QString DialogMultiVideoGeneration::generatorId() const
{
    return m_comboGenerator->currentData().toString();
}

AbstractCli *DialogMultiVideoGeneration::imageCli() const
{
    return m_comboImageCli->currentData().value<AbstractCli *>();
}

AbstractCli *DialogMultiVideoGeneration::decisionCli() const
{
    return m_comboDecisionCli->currentData().value<AbstractCli *>();
}

DialogGenerationOptions::VideoFormat DialogMultiVideoGeneration::videoFormat() const
{
    return static_cast<DialogGenerationOptions::VideoFormat>(
        m_comboFormat->currentData().toInt());
}

QString DialogMultiVideoGeneration::videoFormatLabel() const
{
    return DialogGenerationOptions::formatLabel(videoFormat());
}
