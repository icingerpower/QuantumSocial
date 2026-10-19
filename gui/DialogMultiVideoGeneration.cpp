#include "DialogMultiVideoGeneration.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

#include "AbstractCli.h"
#include "MultiVideoStepWidget.h"
#include "model/SavedPrompts.h"
#include "model/properties/PropertySampler.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/AbstractVideoGenerator.h"
#include "model/videos/TableVideos.h"

namespace {
QString withTrailingPeriod(QString text)
{
    text = text.trimmed();
    if (!text.isEmpty() && !text.endsWith(QLatin1Char('.')))
    {
        text += QLatin1Char('.');
    }
    return text;
}
}

DialogMultiVideoGeneration::DialogMultiVideoGeneration(
    const QList<AbstractCli *> &availableClis, QWidget *parent)
    : DialogMultiVideoGeneration(availableClis, nullptr, nullptr, nullptr, parent)
{
}

DialogMultiVideoGeneration::DialogMultiVideoGeneration(
    const QList<AbstractCli *> &availableClis,
    SavedPrompts *savedPrompts,
    QWidget *parent)
    : DialogMultiVideoGeneration(availableClis, savedPrompts, nullptr, nullptr, parent)
{
}

DialogMultiVideoGeneration::DialogMultiVideoGeneration(
    const QList<AbstractCli *> &availableClis,
    SavedPrompts *savedPrompts,
    TreeProperties *properties,
    TableVideos *videos,
    QWidget *parent)
    : QDialog(parent)
    , m_availableClis(availableClis)
    , m_savedPrompts(savedPrompts)
    , m_properties(properties)
    , m_videos(videos)
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
    auto *layoutPrompts = new QVBoxLayout(groupPrompts);
    layoutPrompts->setSpacing(8);

    // Intro header toolbar & editor
    auto *layoutIntroHeader = new QHBoxLayout();
    layoutIntroHeader->setSpacing(6);
    layoutIntroHeader->addWidget(new QLabel(tr("Intro step prompt:"), groupPrompts));

    m_comboSavedIntroPrompt = new QComboBox(groupPrompts);
    m_comboSavedIntroPrompt->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_comboSavedIntroPrompt->setPlaceholderText(tr("Saved prompts..."));
    layoutIntroHeader->addWidget(m_comboSavedIntroPrompt);

    m_buttonLoadIntroPrompt = new QPushButton(tr("Load"), groupPrompts);
    m_buttonLoadIntroPrompt->setToolTip(tr("Load selected saved prompt into intro prompt editor"));
    layoutIntroHeader->addWidget(m_buttonLoadIntroPrompt);

    m_buttonEditIntroPrompt = new QPushButton(tr("Edit..."), groupPrompts);
    m_buttonEditIntroPrompt->setToolTip(tr("Edit selected saved prompt"));
    layoutIntroHeader->addWidget(m_buttonEditIntroPrompt);

    m_buttonSaveIntroPrompt = new QPushButton(tr("Save..."), groupPrompts);
    m_buttonSaveIntroPrompt->setToolTip(tr("Save current intro prompt text to saved prompts library"));
    layoutIntroHeader->addWidget(m_buttonSaveIntroPrompt);

    layoutIntroHeader->addStretch(1);

    layoutIntroHeader->addWidget(new QLabel(tr("Intro takes:"), groupPrompts));
    m_spinIntroTakes = new QSpinBox(groupPrompts);
    m_spinIntroTakes->setRange(1, 10);
    m_spinIntroTakes->setValue(2);
    layoutIntroHeader->addWidget(m_spinIntroTakes);

    layoutPrompts->addLayout(layoutIntroHeader);

    struct PlanProp
    {
        QUuid id;
        QString name;
        QList<PropertySampler::SiblingOption> options;
        QUuid selectedId;
    };
    QList<PlanProp> propList;
    if (m_properties)
    {
        if (m_videos)
        {
            const auto variants = PropertySampler::sampleVariants(*m_properties, *m_videos, 1);
            if (!variants.isEmpty())
            {
                for (const auto &sampled : variants.first())
                {
                    PlanProp prop;
                    prop.id = sampled.propertyId;
                    prop.name = sampled.propertyName;
                    prop.options = sampled.siblings;
                    prop.selectedId = sampled.id;
                    propList << prop;
                }
            }
        }
        if (propList.isEmpty())
        {
            for (TreeProperties::PropertyNode *node : m_properties->topLevelNodes())
            {
                if (node->children.isEmpty())
                {
                    continue;
                }
                PlanProp prop;
                prop.id = node->id;
                prop.name = node->name;
                for (TreeProperties::PropertyNode *child : node->children)
                {
                    prop.options << PropertySampler::SiblingOption{
                        child->id, child->name, child->promptFragment};
                }
                prop.selectedId = node->children.first()->id;
                propList << prop;
            }
        }
    }

    if (!propList.isEmpty())
    {
        auto *introSplitter = new QSplitter(Qt::Horizontal, groupPrompts);
        introSplitter->setChildrenCollapsible(false);

        auto *panelLeft = new QWidget(introSplitter);
        auto *layoutLeft = new QVBoxLayout(panelLeft);
        layoutLeft->setContentsMargins(0, 0, 0, 0);
        layoutLeft->setSpacing(4);

        m_editIntroPrompt = new QPlainTextEdit(panelLeft);
        m_editIntroPrompt->setPlaceholderText(tr(
            "Hook & opener video prompt: fast dynamic camera zoom on product collection, bold catchy atmosphere, smooth cinematic lighting..."));
        m_editIntroPrompt->setMaximumHeight(70);
        layoutLeft->addWidget(m_editIntroPrompt);

        auto *labelProps = new QLabel(tr("A/B properties (applied to Intro step only):"), panelLeft);
        labelProps->setStyleSheet(QStringLiteral("font-weight: bold; color: #555;"));
        layoutLeft->addWidget(labelProps);

        auto *scrollProps = new QScrollArea(panelLeft);
        scrollProps->setWidgetResizable(true);
        scrollProps->setFrameShape(QFrame::NoFrame);
        scrollProps->setMaximumHeight(140);

        auto *contentProps = new QWidget(scrollProps);
        auto *layoutProps = new QVBoxLayout(contentProps);
        layoutProps->setContentsMargins(0, 0, 0, 0);
        layoutProps->setSpacing(4);

        for (const auto &prop : propList)
        {
            auto *rowWidget = new QWidget(contentProps);
            auto *rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(6);

            PropertyRow row;
            row.propertyId = prop.id;

            row.checkBox = new QCheckBox(prop.name, rowWidget);
            row.checkBox->setChecked(true);
            row.checkBox->setMinimumWidth(110);
            rowLayout->addWidget(row.checkBox);

            row.valueCombo = new QComboBox(rowWidget);
            for (const auto &opt : prop.options)
            {
                row.valueCombo->addItem(opt.name);
                const int idx = row.valueCombo->count() - 1;
                row.valueCombo->setItemData(idx, opt.id, Qt::UserRole);
                row.valueCombo->setItemData(idx, opt.promptFragment, Qt::UserRole + 1);
            }
            for (int idx = 0; idx < row.valueCombo->count(); ++idx)
            {
                if (row.valueCombo->itemData(idx, Qt::UserRole).toUuid() == prop.selectedId)
                {
                    row.valueCombo->setCurrentIndex(idx);
                    break;
                }
            }
            rowLayout->addWidget(row.valueCombo, 1);
            layoutProps->addWidget(rowWidget);

            connect(row.checkBox, &QCheckBox::toggled, this, [this, row](bool checked) {
                row.valueCombo->setEnabled(checked);
                _recomputeIntroFinal();
            });
            connect(row.valueCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                    this, [this]() {
                _recomputeIntroFinal();
            });

            m_propertyRows << row;
        }
        layoutProps->addStretch(1);
        scrollProps->setWidget(contentProps);
        layoutLeft->addWidget(scrollProps);

        introSplitter->addWidget(panelLeft);

        auto *panelRight = new QWidget(introSplitter);
        auto *layoutRight = new QVBoxLayout(panelRight);
        layoutRight->setContentsMargins(0, 0, 0, 0);
        layoutRight->setSpacing(4);

        auto *labelFinalTitle = new QLabel(tr("Final Intro Prompt Preview:"), panelRight);
        labelFinalTitle->setStyleSheet(QStringLiteral("font-weight: bold; color: #555;"));
        layoutRight->addWidget(labelFinalTitle);

        auto *scrollPreview = new QScrollArea(panelRight);
        scrollPreview->setWidgetResizable(true);
        scrollPreview->setFrameShape(QFrame::StyledPanel);

        m_labelIntroFinalPreview = new QLabel(scrollPreview);
        m_labelIntroFinalPreview->setWordWrap(true);
        m_labelIntroFinalPreview->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_labelIntroFinalPreview->setAlignment(Qt::AlignLeading | Qt::AlignLeft | Qt::AlignTop);
        m_labelIntroFinalPreview->setMargin(6);
        m_labelIntroFinalPreview->setStyleSheet(QStringLiteral("background-color: #FAFAFA; color: #222; font-size: 11px;"));

        scrollPreview->setWidget(m_labelIntroFinalPreview);
        layoutRight->addWidget(scrollPreview);

        introSplitter->addWidget(panelRight);
        introSplitter->setStretchFactor(0, 3);
        introSplitter->setStretchFactor(1, 2);
        introSplitter->setSizes({550, 350});

        layoutPrompts->addWidget(introSplitter);
    }
    else
    {
        m_editIntroPrompt = new QPlainTextEdit(groupPrompts);
        m_editIntroPrompt->setPlaceholderText(tr(
            "Hook & opener video prompt: fast dynamic camera zoom on product collection, bold catchy atmosphere, smooth cinematic lighting..."));
        m_editIntroPrompt->setMaximumHeight(70);
        layoutPrompts->addWidget(m_editIntroPrompt);
    }

    // After-intro header toolbar & editor
    auto *layoutAfterIntroHeader = new QHBoxLayout();
    layoutAfterIntroHeader->setSpacing(6);
    layoutAfterIntroHeader->addWidget(new QLabel(tr("After-intro steps prompt:"), groupPrompts));

    m_comboSavedAfterIntroPrompt = new QComboBox(groupPrompts);
    m_comboSavedAfterIntroPrompt->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_comboSavedAfterIntroPrompt->setPlaceholderText(tr("Saved prompts..."));
    layoutAfterIntroHeader->addWidget(m_comboSavedAfterIntroPrompt);

    m_buttonLoadAfterIntroPrompt = new QPushButton(tr("Load"), groupPrompts);
    m_buttonLoadAfterIntroPrompt->setToolTip(tr("Load selected saved prompt into after-intro prompt editor"));
    layoutAfterIntroHeader->addWidget(m_buttonLoadAfterIntroPrompt);

    m_buttonEditAfterIntroPrompt = new QPushButton(tr("Edit..."), groupPrompts);
    m_buttonEditAfterIntroPrompt->setToolTip(tr("Edit selected saved prompt"));
    layoutAfterIntroHeader->addWidget(m_buttonEditAfterIntroPrompt);

    m_buttonSaveAfterIntroPrompt = new QPushButton(tr("Save..."), groupPrompts);
    m_buttonSaveAfterIntroPrompt->setToolTip(tr("Save current after-intro prompt text to saved prompts library"));
    layoutAfterIntroHeader->addWidget(m_buttonSaveAfterIntroPrompt);

    layoutAfterIntroHeader->addStretch(1);

    layoutAfterIntroHeader->addWidget(new QLabel(tr("Takes per step:"), groupPrompts));
    m_spinAfterIntroTakes = new QSpinBox(groupPrompts);
    m_spinAfterIntroTakes->setRange(1, 10);
    m_spinAfterIntroTakes->setValue(2);
    layoutAfterIntroHeader->addWidget(m_spinAfterIntroTakes);

    layoutPrompts->addLayout(layoutAfterIntroHeader);

    m_editAfterIntroPrompt = new QPlainTextEdit(groupPrompts);
    m_editAfterIntroPrompt->setPlaceholderText(tr(
        "Product showcase prompt: clean 360 rotation highlighting material quality and fine craftsmanship, elegant backdrop..."));
    m_editAfterIntroPrompt->setMaximumHeight(70);
    layoutPrompts->addWidget(m_editAfterIntroPrompt);

    m_labelTotalSummary = new QLabel(groupPrompts);
    m_labelTotalSummary->setStyleSheet(QStringLiteral("font-weight: bold; color: #4CAF50;"));
    layoutPrompts->addWidget(m_labelTotalSummary);

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
    connect(m_editIntroPrompt, &QPlainTextEdit::textChanged, this, &DialogMultiVideoGeneration::_recomputeIntroFinal);
    connect(m_editAfterIntroPrompt, &QPlainTextEdit::textChanged, this, &DialogMultiVideoGeneration::_updateOkButton);
    connect(m_buttonApplyAllRegen, &QPushButton::clicked, this, &DialogMultiVideoGeneration::_applyDefaultRegenToAllSlots);

    connect(m_comboSavedIntroPrompt, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        m_buttonLoadIntroPrompt->setEnabled(index >= 0);
        m_buttonEditIntroPrompt->setEnabled(index >= 0);
    });
    connect(m_buttonLoadIntroPrompt, &QPushButton::clicked, this, [this]() {
        _loadPrompt(m_comboSavedIntroPrompt, m_editIntroPrompt);
    });
    connect(m_buttonEditIntroPrompt, &QPushButton::clicked, this, [this]() {
        _editPrompt(m_comboSavedIntroPrompt, m_editIntroPrompt);
    });
    connect(m_buttonSaveIntroPrompt, &QPushButton::clicked, this, [this]() {
        _savePrompt(m_comboSavedIntroPrompt, m_editIntroPrompt);
    });

    connect(m_comboSavedAfterIntroPrompt, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        m_buttonLoadAfterIntroPrompt->setEnabled(index >= 0);
        m_buttonEditAfterIntroPrompt->setEnabled(index >= 0);
    });
    connect(m_buttonLoadAfterIntroPrompt, &QPushButton::clicked, this, [this]() {
        _loadPrompt(m_comboSavedAfterIntroPrompt, m_editAfterIntroPrompt);
    });
    connect(m_buttonEditAfterIntroPrompt, &QPushButton::clicked, this, [this]() {
        _editPrompt(m_comboSavedAfterIntroPrompt, m_editAfterIntroPrompt);
    });
    connect(m_buttonSaveAfterIntroPrompt, &QPushButton::clicked, this, [this]() {
        _savePrompt(m_comboSavedAfterIntroPrompt, m_editAfterIntroPrompt);
    });

    const bool hasSaved = (m_savedPrompts != nullptr);
    m_comboSavedIntroPrompt->setVisible(hasSaved);
    m_buttonLoadIntroPrompt->setVisible(hasSaved);
    m_buttonEditIntroPrompt->setVisible(hasSaved);
    m_buttonSaveIntroPrompt->setVisible(hasSaved);
    m_comboSavedAfterIntroPrompt->setVisible(hasSaved);
    m_buttonLoadAfterIntroPrompt->setVisible(hasSaved);
    m_buttonEditAfterIntroPrompt->setVisible(hasSaved);
    m_buttonSaveAfterIntroPrompt->setVisible(hasSaved);

    _refreshSavedPromptCombos();

    _rebuildStepWidgets(m_spinTotalSteps->value());
    _updateTotalCountLabel();
    _updateOkButton();
    _recomputeIntroFinal();
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
    return _computeIntroFinal();
}

QString DialogMultiVideoGeneration::rawIntroPrompt() const
{
    return m_editIntroPrompt ? m_editIntroPrompt->toPlainText().trimmed() : QString();
}

QList<QUuid> DialogMultiVideoGeneration::introPropertyValueIds() const
{
    QList<QUuid> ids;
    for (const auto &row : m_propertyRows)
    {
        if (row.checkBox && row.checkBox->isChecked() && row.valueCombo)
        {
            const QUuid id = row.valueCombo->currentData(Qt::UserRole).toUuid();
            if (!id.isNull())
            {
                ids << id;
            }
        }
    }
    return ids;
}

QString DialogMultiVideoGeneration::_computeIntroFinal() const
{
    if (!m_editIntroPrompt)
    {
        return QString();
    }
    const QString basePrompt = m_editIntroPrompt->toPlainText().trimmed();
    QStringList fragments;
    for (const PropertyRow &row : m_propertyRows)
    {
        if (row.checkBox && row.checkBox->isChecked() && row.valueCombo)
        {
            const QString fragment
                = row.valueCombo->currentData(Qt::UserRole + 1).toString().trimmed();
            if (!fragment.isEmpty())
            {
                fragments << withTrailingPeriod(fragment);
            }
        }
    }

    if (fragments.isEmpty())
    {
        return basePrompt;
    }

    QStringList parts;
    if (!basePrompt.isEmpty())
    {
        parts << withTrailingPeriod(basePrompt);
    }
    parts.append(fragments);
    return parts.join(QStringLiteral(" "));
}

void DialogMultiVideoGeneration::_recomputeIntroFinal()
{
    const QString computed = _computeIntroFinal();
    if (m_labelIntroFinalPreview)
    {
        m_labelIntroFinalPreview->setText(computed);
    }
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

void DialogMultiVideoGeneration::setKeyword(const QString &keyword)
{
    if (m_editKeyword)
    {
        m_editKeyword->setText(keyword);
    }
}

void DialogMultiVideoGeneration::setIntroPrompt(const QString &prompt)
{
    if (m_editIntroPrompt)
    {
        m_editIntroPrompt->setPlainText(prompt);
    }
}

void DialogMultiVideoGeneration::setAfterIntroPrompt(const QString &prompt)
{
    if (m_editAfterIntroPrompt)
    {
        m_editAfterIntroPrompt->setPlainText(prompt);
    }
}

void DialogMultiVideoGeneration::_refreshSavedPromptCombos()
{
    if (!m_savedPrompts)
    {
        return;
    }
    QStringList names;
    for (const auto &entry : m_savedPrompts->entries())
    {
        names << entry.name;
    }
    const auto updateCombo = [&names](QComboBox *combo, QPushButton *btnEdit, QPushButton *btnLoad) {
        if (!combo)
        {
            return;
        }
        const QString current = combo->currentText();
        const QSignalBlocker blocker{combo};
        combo->clear();
        combo->addItems(names);
        const int idx = combo->findText(current);
        combo->setCurrentIndex(idx);
        if (btnEdit)
        {
            btnEdit->setEnabled(idx >= 0);
        }
        if (btnLoad)
        {
            btnLoad->setEnabled(idx >= 0);
        }
    };
    updateCombo(m_comboSavedIntroPrompt, m_buttonEditIntroPrompt, m_buttonLoadIntroPrompt);
    updateCombo(m_comboSavedAfterIntroPrompt, m_buttonEditAfterIntroPrompt, m_buttonLoadAfterIntroPrompt);
}

void DialogMultiVideoGeneration::_loadPrompt(QComboBox *combo, QPlainTextEdit *edit)
{
    if (!m_savedPrompts || !combo || !edit || combo->currentIndex() < 0)
    {
        return;
    }
    const QString selectedName = combo->currentText();
    for (const auto &entry : m_savedPrompts->entries())
    {
        if (entry.name == selectedName)
        {
            edit->setPlainText(entry.prompt);
            break;
        }
    }
}

void DialogMultiVideoGeneration::_editPrompt(QComboBox *combo, QPlainTextEdit *edit)
{
    if (!m_savedPrompts || !combo || !edit || combo->currentIndex() < 0)
    {
        return;
    }
    const QString selectedName = combo->currentText();
    for (const auto &entry : m_savedPrompts->entries())
    {
        if (entry.name != selectedName)
        {
            continue;
        }
        bool accepted = false;
        const QString edited = QInputDialog::getMultiLineText(
            this, tr("Edit saved prompt — %1").arg(entry.name),
            tr("Prompt:"), entry.prompt, &accepted);
        if (accepted)
        {
            if (edited.trimmed().isEmpty())
            {
                QMessageBox::warning(this, tr("Edit saved prompt"),
                                     tr("A saved prompt cannot be empty."));
                return;
            }
            m_savedPrompts->savePrompt(entry.name, edited);
            if (m_editIntroPrompt && m_editIntroPrompt->toPlainText() == entry.prompt)
            {
                m_editIntroPrompt->setPlainText(edited);
            }
            if (m_editAfterIntroPrompt && m_editAfterIntroPrompt->toPlainText() == entry.prompt)
            {
                m_editAfterIntroPrompt->setPlainText(edited);
            }
            _refreshSavedPromptCombos();
            combo->setCurrentText(entry.name);
        }
        return;
    }
}

void DialogMultiVideoGeneration::_savePrompt(QComboBox *combo, QPlainTextEdit *edit)
{
    if (!m_savedPrompts || !combo || !edit)
    {
        return;
    }
    const QString promptText = edit->toPlainText().trimmed();
    if (promptText.isEmpty())
    {
        QMessageBox::warning(this, tr("Save prompt"),
                             tr("Write a prompt first before saving it."));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, tr("Save prompt"),
        tr("Name (an existing name overwrites/edits that saved prompt):"),
        QLineEdit::Normal, combo->currentText(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
    {
        return;
    }
    m_savedPrompts->savePrompt(name, promptText);
    _refreshSavedPromptCombos();
    combo->setCurrentText(name);
    const int idx = combo->findText(name);
    if (idx >= 0)
    {
        combo->setCurrentIndex(idx);
    }
}
