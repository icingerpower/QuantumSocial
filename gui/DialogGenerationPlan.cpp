#include "DialogGenerationPlan.h"
#include "GenerationPlanSection.h"
#include "ui_DialogGenerationPlan.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "AbstractCli.h"

#include "model/SavedPrompts.h"
#include "model/videogen/AbstractVideoGenerator.h"

namespace {
const QString SETTING_ONE_IMAGE = QStringLiteral("generation/choiceOneImage");
const QString SETTING_SLIDESHOW = QStringLiteral("generation/choiceSlideshow");
const QString SETTING_VIDEOS = QStringLiteral("generation/choiceVideoGenerators");
const QString SETTING_IMAGE_CLI = QStringLiteral("generation/imageGenCli");
const QString SETTING_SLIDESHOW_CLI = QStringLiteral("generation/slideshowGenCli");
}

DialogGenerationPlan::DialogGenerationPlan(const QList<QList<PlanProperty>> &propertiesPerStrategy,
                                           const QList<AbstractCli *> &availableClis,
                                           SavedPrompts *savedPrompts,
                                           const QString &videoFormatLabel,
                                           QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DialogGenerationPlan)
    , m_savedPrompts(savedPrompts)
{
    ui->setupUi(this);
    if (!videoFormatLabel.isEmpty())
    {
        setWindowTitle(tr("Generation plan — %1").arg(videoFormatLabel));
    }
    // Pre-seed BEFORE any section is built, keyed by (strategy index,
    // property id) — see _syncKey() — so every section's first-built page
    // for a given key already agrees on the starting checked state/pick,
    // instead of depending on build order.
    for (int i = 0; i < propertiesPerStrategy.size() && i < kStrategyCount; ++i)
    {
        for (const PlanProperty &property : propertiesPerStrategy[i])
        {
            const QString key = _syncKey(i, property.propertyId);
            m_propertyChecked.insert(key, property.checked);
            m_selectedOption.insert(key, property.selectedOptionId);
        }
    }

    // Last ticked options are the defaults.
    QSettings settings;
    m_imageSection = _makeSection(tr("One image"), propertiesPerStrategy,
        settings.value(SETTING_ONE_IMAGE, true).toBool());
    _addCliCombo(m_imageSection, availableClis, SETTING_IMAGE_CLI);
    m_imageSection.group->setGenerationCount(settings.value("generation/count/image", 1).toInt());
    m_slideshowSection = _makeSection(tr("Several images (slideshow)"),
        propertiesPerStrategy, settings.value(SETTING_SLIDESHOW, false).toBool());
    _addCliCombo(m_slideshowSection, availableClis, SETTING_SLIDESHOW_CLI);
    m_slideshowSection.group->setGenerationCount(settings.value("generation/count/slideshow", 1).toInt());

    // One checkable group per registered video backend — a newly added
    // AbstractVideoGenerator subclass appears here on its own.
    const QStringList savedVideos = settings.value(SETTING_VIDEOS).toStringList();
    const auto &generators = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    for (auto it = generators.cbegin(); it != generators.cend(); ++it)
    {
        m_videoSections << qMakePair(it.key(),
            _makeSection(tr("Video — %1").arg(it.value()->getName()),
                         propertiesPerStrategy, savedVideos.contains(it.key())));
        m_videoSections.last().second.group->setGenerationCount(
            settings.value("generation/count/video/" + it.key(), 1).toInt());
    }

    _refreshSavedPromptCombos();
    _updateOkButton();
}

void DialogGenerationPlan::_refreshSavedPromptCombos()
{
    QStringList names;
    for (const SavedPrompts::Entry &entry : m_savedPrompts->entries())
    {
        names << entry.name;
    }
    for (int i = 0; i < m_savedPromptCombos.size(); ++i)
    {
        QComboBox *combo = m_savedPromptCombos[i];
        const QString current = combo->currentText();
        const QSignalBlocker blocker{combo};
        combo->clear();
        combo->addItems(names);
        combo->setCurrentIndex(combo->findText(current));
        m_editSavedPromptButtons[i]->setEnabled(combo->currentIndex() >= 0);
    }
}

DialogGenerationPlan::~DialogGenerationPlan()
{
    delete ui;
}

QString DialogGenerationPlan::_syncKey(int strategyIndex, const QUuid &propertyId)
{
    return QString::number(strategyIndex) + QLatin1Char('|')
        + propertyId.toString(QUuid::WithoutBraces);
}

DialogGenerationPlan::Plan DialogGenerationPlan::plan() const
{
    Plan plan;
    plan.oneImage = m_imageSection.group->isChecked();
    plan.imageCount = m_imageSection.group->generationCount();
    plan.imagePrompt = _sectionPrompt(m_imageSection);
    plan.imagePropertyValueIds = _sectionCheckedPropertyValues(m_imageSection);
    plan.slideshow = m_slideshowSection.group->isChecked();
    plan.slideshowCount = m_slideshowSection.group->generationCount();
    plan.slideshowPrompt = _sectionPrompt(m_slideshowSection);
    plan.slideshowPropertyValueIds = _sectionCheckedPropertyValues(m_slideshowSection);
    for (const auto &video : m_videoSections)
    {
        if (video.second.group->isChecked())
        {
            plan.videos << VideoPick{video.first, _sectionPrompt(video.second),
                                     _sectionCheckedPropertyValues(video.second),
                                     video.second.group->generationCount()};
        }
    }
    return plan;
}

AbstractCli *DialogGenerationPlan::imageCli() const
{
    return _comboCli(m_imageSection.cliCombo);
}

AbstractCli *DialogGenerationPlan::slideshowCli() const
{
    return _comboCli(m_slideshowSection.cliCombo);
}

AbstractCli *DialogGenerationPlan::_comboCli(const QComboBox *combo)
{
    return combo ? combo->currentData().value<AbstractCli *>() : nullptr;
}

void DialogGenerationPlan::accept()
{
    const Plan currentPlan = plan();
    QStringList videoIds;
    for (const auto &video : currentPlan.videos)
    {
        videoIds << video.generatorId;
    }
    QSettings settings;
    settings.setValue(SETTING_ONE_IMAGE, currentPlan.oneImage);
    settings.setValue(SETTING_SLIDESHOW, currentPlan.slideshow);
    settings.setValue(SETTING_VIDEOS, videoIds);
    settings.setValue("generation/count/image", currentPlan.imageCount);
    settings.setValue("generation/count/slideshow", currentPlan.slideshowCount);
    for (const auto &video : m_videoSections)
    {
        settings.setValue("generation/count/video/" + video.first,
                          video.second.group->generationCount());
    }
    if (AbstractCli *cli = imageCli())
    {
        settings.setValue(SETTING_IMAGE_CLI, cli->getName());
    }
    if (AbstractCli *cli = slideshowCli())
    {
        settings.setValue(SETTING_SLIDESHOW_CLI, cli->getName());
    }

    QDialog::accept();
}

int DialogGenerationPlan::_activeStrategyIndex(const OptionSection &section)
{
    const int id = section.strategyButtons ? section.strategyButtons->checkedId() : -1;
    return id >= 0 ? id : 0;
}

QString DialogGenerationPlan::_sectionPrompt(const OptionSection &section)
{
    const auto page = section.strategies.value(_activeStrategyIndex(section));
    return page ? _computeFinal(*page) : QString{};
}

QList<QUuid> DialogGenerationPlan::_sectionCheckedPropertyValues(const OptionSection &section)
{
    QList<QUuid> ids;
    const auto page = section.strategies.value(_activeStrategyIndex(section));
    if (!page)
    {
        return ids;
    }
    for (const PropertyRow &row : page->propertyRows)
    {
        if (row.checkBox->isChecked())
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

namespace {
// Ends the part with exactly one period — never doubled when the source
// text (a hand-written base prompt, or a property's own fragment text)
// already ends with one, e.g. "...background." must not become
// "...background..".
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

QString DialogGenerationPlan::_computeFinal(const StrategyPage &strategy)
{
    QStringList parts;
    const QString basePrompt = strategy.promptEdit->toPlainText().trimmed();
    if (!basePrompt.isEmpty())
    {
        parts << withTrailingPeriod(basePrompt);
    }
    for (const PropertyRow &row : strategy.propertyRows)
    {
        if (row.checkBox->isChecked())
        {
            const QString fragment
                = row.valueCombo->currentData(Qt::UserRole + 1).toString().trimmed();
            if (!fragment.isEmpty())
            {
                parts << withTrailingPeriod(fragment);
            }
        }
    }
    // Each part already ends with its own period, so a single space between
    // them reads as ". " separation without ever doubling the punctuation
    // (unlike joining with a literal ". " while parts may already have one).
    return parts.join(QStringLiteral(" "));
}

void DialogGenerationPlan::_recomputeFinal(StrategyPage &strategy)
{
    strategy.previewLabel->setText(_computeFinal(strategy));
}

void DialogGenerationPlan::_onPropertyCheckToggled(const QString &syncKey, bool checked)
{
    m_propertyChecked[syncKey] = checked;
    for (const PropertyRow &row : m_propertyRows.value(syncKey))
    {
        if (row.checkBox->isChecked() != checked)
        {
            const QSignalBlocker blocker{row.checkBox};
            row.checkBox->setChecked(checked);
        }
        row.valueCombo->setEnabled(checked);
        if (row.page)
        {
            _recomputeFinal(*row.page);
        }
    }
    _updateOkButton();
}

void DialogGenerationPlan::_onPropertyValueChanged(const QString &syncKey, const QUuid &optionId)
{
    if (optionId.isNull())
    {
        return;
    }
    m_selectedOption[syncKey] = optionId;
    for (const PropertyRow &row : m_propertyRows.value(syncKey))
    {
        if (row.valueCombo->currentData(Qt::UserRole).toUuid() != optionId)
        {
            const QSignalBlocker blocker{row.valueCombo};
            for (int idx = 0; idx < row.valueCombo->count(); ++idx)
            {
                if (row.valueCombo->itemData(idx, Qt::UserRole).toUuid() == optionId)
                {
                    row.valueCombo->setCurrentIndex(idx);
                    break;
                }
            }
        }
        if (row.page)
        {
            _recomputeFinal(*row.page);
        }
    }
}

DialogGenerationPlan::OptionSection DialogGenerationPlan::_makeSection(
    const QString &title,
    const QList<QList<PlanProperty>> &propertiesPerStrategy, bool checked)
{
    OptionSection section;
    section.group = new GenerationPlanSection{this};
    section.group->setTitle(title);
    section.group->setChecked(checked);
    section.strategyButtons = new QButtonGroup{section.group};
    const QList<QRadioButton *> radioButtons = section.group->strategyButtons();
    for (int i = 0; i < radioButtons.size(); ++i)
    {
        section.strategyButtons->addButton(radioButtons[i], i);
    }

    section.strategyStack = section.group->strategyStack();
    for (int i = 0; i < kStrategyCount; ++i)
    {
        auto page = QSharedPointer<StrategyPage>::create();

        // Saved-prompt row: one shared library (SavedPrompts) usable from
        // every strategy slot/content kind. Load copies a saved prompt into
        // the current editor; Edit changes the saved entry directly.
        auto *savedPromptCombo = section.group->savedPromptCombo(i);
        m_savedPromptCombos << savedPromptCombo;
        auto *buttonLoadPrompt = section.group->loadPromptButton(i);
        auto *buttonEditPrompt = section.group->editPromptButton(i);
        m_editSavedPromptButtons << buttonEditPrompt;
        auto *buttonSavePrompt = section.group->savePromptButton(i);
        page->promptEdit = section.group->promptEdit(i);

        connect(savedPromptCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                buttonEditPrompt, [buttonEditPrompt](int index) {
            buttonEditPrompt->setEnabled(index >= 0);
        });

        connect(buttonLoadPrompt, &QPushButton::clicked, this,
                [this, page, savedPromptCombo]() {
            if (savedPromptCombo->currentIndex() < 0)
            {
                return;
            }
            for (const SavedPrompts::Entry &entry : m_savedPrompts->entries())
            {
                if (entry.name == savedPromptCombo->currentText())
                {
                    page->promptEdit->setPlainText(entry.prompt);
                    break;
                }
            }
        });
        connect(buttonEditPrompt, &QPushButton::clicked, this,
                [this, page, savedPromptCombo]() {
            if (savedPromptCombo->currentIndex() < 0)
            {
                return;
            }
            for (const SavedPrompts::Entry &entry : m_savedPrompts->entries())
            {
                if (entry.name != savedPromptCombo->currentText())
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
                    if (page->promptEdit->toPlainText() == entry.prompt)
                    {
                        page->promptEdit->setPlainText(edited);
                    }
                }
                return;
            }
        });
        connect(buttonSavePrompt, &QPushButton::clicked, this,
                [this, page, savedPromptCombo]() {
            bool accepted = false;
            const QString name = QInputDialog::getText(this, tr("Save prompt"),
                tr("Name (an existing name overwrites/edits that saved prompt):"),
                QLineEdit::Normal, savedPromptCombo->currentText(), &accepted).trimmed();
            if (!accepted || name.isEmpty())
            {
                return;
            }
            m_savedPrompts->savePrompt(name, page->promptEdit->toPlainText());
            _refreshSavedPromptCombos();
            savedPromptCombo->setCurrentText(name);
        });

        const QList<PlanProperty> properties = propertiesPerStrategy.value(i);
        if (!properties.isEmpty())
        {
            auto *scrollLayout = section.group->propertyLayout(i);
            auto *scrollContent = section.group->propertyScroll(i)->widget();
            for (const PlanProperty &property : properties)
            {
                const QString syncKey = _syncKey(i, property.propertyId);

                auto *rowWidget = new QWidget{scrollContent};
                auto *rowLayout = new QHBoxLayout{rowWidget};
                rowLayout->setContentsMargins(0, 0, 0, 0);

                PropertyRow row;
                row.propertyId = property.propertyId;
                row.page = page;

                const bool rowChecked = m_propertyChecked.value(syncKey, property.checked);
                row.checkBox = new QCheckBox{property.propertyName, rowWidget};
                row.checkBox->setChecked(rowChecked);

                row.valueCombo = new QComboBox{rowWidget};
                row.valueCombo->setEnabled(rowChecked);
                for (const PlanPropertyOption &option : property.options)
                {
                    row.valueCombo->addItem(option.name);
                    const int idx = row.valueCombo->count() - 1;
                    row.valueCombo->setItemData(idx, option.id, Qt::UserRole);
                    row.valueCombo->setItemData(idx, option.fragment, Qt::UserRole + 1);
                }
                const QUuid selected = m_selectedOption.value(syncKey, property.selectedOptionId);
                for (int idx = 0; idx < row.valueCombo->count(); ++idx)
                {
                    if (row.valueCombo->itemData(idx, Qt::UserRole).toUuid() == selected)
                    {
                        row.valueCombo->setCurrentIndex(idx);
                        break;
                    }
                }

                rowLayout->addWidget(row.checkBox);
                rowLayout->addWidget(row.valueCombo, 1);
                scrollLayout->addWidget(rowWidget);

                connect(row.checkBox, &QCheckBox::toggled, this,
                        [this, syncKey](bool state) { _onPropertyCheckToggled(syncKey, state); });
                connect(row.valueCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this, syncKey](int index) {
                    auto *combo = qobject_cast<QComboBox *>(sender());
                    if (combo && index >= 0)
                    {
                        _onPropertyValueChanged(syncKey, combo->itemData(index, Qt::UserRole).toUuid());
                    }
                });

                page->propertyRows << row;
                m_propertyRows[syncKey] << row;
            }
            scrollLayout->addStretch(1);
        }
        else
        {
            section.group->propertyLabel(i)->hide();
            section.group->propertyScroll(i)->hide();
        }
        page->previewLabel = section.group->previewLabel(i);
        section.strategies << page;

        connect(page->promptEdit, &QPlainTextEdit::textChanged, this, [this, page]() {
            _recomputeFinal(*page);
            _updateOkButton();
        });

        _recomputeFinal(*page);
    }
    auto *stack = section.strategyStack;
    connect(section.strategyButtons, &QButtonGroup::idToggled, this,
            [this, stack](int id, bool buttonChecked) {
        if (buttonChecked)
        {
            stack->setCurrentIndex(id);
        }
        _updateOkButton();
    });

    // A splitter (not a plain layout) so the user can drag each section
    // taller to see one generation style's whole choice at once.
    ui->splitterOptions->addWidget(section.group);

    connect(section.group, &QGroupBox::toggled,
            this, &DialogGenerationPlan::_updateOkButton);

    return section;
}

void DialogGenerationPlan::_addCliCombo(OptionSection &section,
                                        const QList<AbstractCli *> &availableClis,
                                        const QString &settingsKey)
{
    section.group->cliRow()->show();
    section.cliCombo = section.group->cliCombo();
    for (AbstractCli *cli : availableClis)
    {
        if (cli->canGenImages())
        {
            section.cliCombo->addItem(cli->getName(), QVariant::fromValue(cli));
        }
    }
    if (section.cliCombo->count() == 0)
    {
        section.cliCombo->addItem(tr("(no image-capable CLI available)"));
        section.cliCombo->setEnabled(false);
        section.group->setChecked(false);
        section.group->setEnabled(false);
        section.group->setToolTip(
            tr("No image-capable CLI is available on this machine."));
    }
    else
    {
        const int savedIndex = section.cliCombo->findText(
            QSettings().value(settingsKey).toString());
        if (savedIndex >= 0)
        {
            section.cliCombo->setCurrentIndex(savedIndex);
        }
    }
}

void DialogGenerationPlan::_updateOkButton()
{
    // OK needs at least one ticked option, and every ticked option must
    // have a usable prompt.
    bool anyChecked = false;
    bool allValid = true;
    const auto checkSection = [&anyChecked, &allValid](const OptionSection &section) {
        if (!section.group || !section.group->isChecked())
        {
            return;
        }
        anyChecked = true;
        if (_sectionPrompt(section).isEmpty())
        {
            allValid = false;
        }
    };
    checkSection(m_imageSection);
    checkSection(m_slideshowSection);
    for (const auto &video : m_videoSections)
    {
        checkSection(video.second);
    }
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(anyChecked && allValid);
}
