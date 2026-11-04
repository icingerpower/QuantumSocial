// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneInspiration.h"
#include "ui_PaneInspiration.h"

#include <algorithm>

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSignalBlocker>

#include "AbstractCli.h"
#include "AvailableCliList.h"
#include "AvailableCliTable.h"
#include "../ComboBoxColumnDelegate.h"
#include "../DialogValidateIdea.h"

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"

PaneInspiration::PaneInspiration(PreferredHashtags *hashtags,
                                 FavoriteHooks *favoriteHooks,
                                 FavoriteVideoPrompts *favoriteVideoPrompts,
                                 AvailableCliTable *availableClis,
                                 QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneInspiration)
    , m_hashtags(hashtags)
    , m_favoriteHooks(favoriteHooks)
    , m_favoriteVideoPrompts(favoriteVideoPrompts)
    , m_availableCliTable(availableClis ? availableClis : new AvailableCliTable(this))
    , m_cliList(new AvailableCliList(m_availableCliTable, this))
{
    ui->setupUi(this);

    _setupViews();
    _setupDefaultPrompts();
    _loadSettings();

    // Connect Add / Remove buttons
    connect(ui->buttonAddHashtag, &QPushButton::clicked, this, &PaneInspiration::_addHashtagRow);
    connect(ui->buttonRemoveHashtag, &QPushButton::clicked, this, &PaneInspiration::_removeSelectedHashtags);
    connect(ui->buttonAddHook, &QPushButton::clicked, this, &PaneInspiration::_addHookRow);
    connect(ui->buttonRemoveHook, &QPushButton::clicked, this, &PaneInspiration::_removeSelectedHooks);
    connect(ui->buttonAddPrompt, &QPushButton::clicked, this, &PaneInspiration::_addPromptRow);
    connect(ui->buttonRemovePrompt, &QPushButton::clicked, this, &PaneInspiration::_removeSelectedPrompts);

    // Connect Generate buttons
    connect(ui->buttonGenerateHashtag, &QPushButton::clicked, this, &PaneInspiration::_generateHashtagIdea);
    connect(ui->buttonGenerateHook, &QPushButton::clicked, this, &PaneInspiration::_generateHookIdea);
    connect(ui->buttonGeneratePrompt, &QPushButton::clicked, this, &PaneInspiration::_generatePromptIdea);
}

PaneInspiration::~PaneInspiration()
{
    delete ui;
}

void PaneInspiration::_setupViews()
{
    const QStringList originOptions{PreferredHashtags::SOURCE_HUMAN, PreferredHashtags::SOURCE_AI};

    // Preferred Hashtags Table
    ui->tableViewHashtags->setModel(m_hashtags);
    ui->tableViewHashtags->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableViewHashtags->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tableViewHashtags->setItemDelegate(new ComboBoxColumnDelegate(
        {{PreferredHashtags::IND_SOURCE, originOptions}}, ui->tableViewHashtags));
    ui->tableViewHashtags->horizontalHeader()->setSectionResizeMode(
        PreferredHashtags::IND_TAG, QHeaderView::Stretch);
    ui->tableViewHashtags->horizontalHeader()->setSectionResizeMode(
        PreferredHashtags::IND_SOURCE, QHeaderView::Fixed);
    ui->tableViewHashtags->horizontalHeader()->resizeSection(PreferredHashtags::IND_SOURCE, 130);

    // Favorite Hooks Table
    ui->tableViewHooks->setModel(m_favoriteHooks);
    ui->tableViewHooks->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableViewHooks->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tableViewHooks->setItemDelegate(new ComboBoxColumnDelegate(
        {{FavoriteHooks::IND_SOURCE, originOptions}}, ui->tableViewHooks));
    ui->tableViewHooks->horizontalHeader()->setSectionResizeMode(
        FavoriteHooks::IND_HOOK, QHeaderView::Stretch);
    ui->tableViewHooks->horizontalHeader()->setSectionResizeMode(
        FavoriteHooks::IND_SOURCE, QHeaderView::Fixed);
    ui->tableViewHooks->horizontalHeader()->resizeSection(FavoriteHooks::IND_SOURCE, 130);

    // Favorite Video Prompts Table
    ui->tableViewPrompts->setModel(m_favoriteVideoPrompts);
    ui->tableViewPrompts->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableViewPrompts->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->tableViewPrompts->setItemDelegate(new ComboBoxColumnDelegate(
        {{FavoriteVideoPrompts::IND_SOURCE, originOptions}}, ui->tableViewPrompts));
    ui->tableViewPrompts->horizontalHeader()->setSectionResizeMode(
        FavoriteVideoPrompts::IND_PROMPT, QHeaderView::Stretch);
    ui->tableViewPrompts->horizontalHeader()->setSectionResizeMode(
        FavoriteVideoPrompts::IND_SOURCE, QHeaderView::Fixed);
    ui->tableViewPrompts->horizontalHeader()->resizeSection(FavoriteVideoPrompts::IND_SOURCE, 130);

    // Set CLI list model for all combo boxes
    ui->comboCliHashtag->setModel(m_cliList);
    ui->comboCliHook->setModel(m_cliList);
    ui->comboCliPrompt->setModel(m_cliList);

    // Make tables expand to absorb extra vertical space while bottom AI groups stay compact
    ui->verticalLayoutHashtags->setStretch(0, 0);
    ui->verticalLayoutHashtags->setStretch(1, 0);
    ui->verticalLayoutHashtags->setStretch(2, 1);
    ui->verticalLayoutHashtags->setStretch(3, 0);

    ui->verticalLayoutHooks->setStretch(0, 0);
    ui->verticalLayoutHooks->setStretch(1, 0);
    ui->verticalLayoutHooks->setStretch(2, 1);
    ui->verticalLayoutHooks->setStretch(3, 0);

    ui->verticalLayoutPrompts->setStretch(0, 0);
    ui->verticalLayoutPrompts->setStretch(1, 0);
    ui->verticalLayoutPrompts->setStretch(2, 1);
    ui->verticalLayoutPrompts->setStretch(3, 0);
}

void PaneInspiration::_setupDefaultPrompts()
{
    ui->editPromptHashtag->setPlainText(
        QStringLiteral("Suggest high-performing hashtags for short-form social media videos, "
                       "similar in style, niche, and audience to the existing preferred hashtags. "
                       "Return each hashtag on its own line (e.g. #hashtag), without numbering or extra commentary."));

    ui->editPromptHook->setPlainText(
        QStringLiteral("Suggest compelling, high-converting opening hook sentences (first 3 seconds) "
                       "for a short-form video, matching the tone, energy, and style of our favorite hooks. "
                       "Return each hook on its own line, without quotes, numbering, or commentary."));

    ui->editPromptPrompt->setPlainText(
        QStringLiteral("Suggest punchy, fast-paced video prompts for generating short-form videos "
                       "(visual scene, camera movement, and pacing), matching the style and dynamics "
                       "of our favorite video prompts. Each prompt must start with 'Generate a video:' or 'Generate a video of'. "
                       "Return each prompt on its own line, without numbering or commentary."));
}

void PaneInspiration::_loadSettings()
{
    auto settings = WorkingDirectoryManager::instance()->settings();

    // Restore spin box values (default 10)
    int countHashtag = settings->value(QStringLiteral("Inspiration/HashtagsCount"), 10).toInt();
    if (countHashtag <= 0) countHashtag = 10;
    ui->spinCountHashtag->setValue(countHashtag);

    int countHook = settings->value(QStringLiteral("Inspiration/HooksCount"), 10).toInt();
    if (countHook <= 0) countHook = 10;
    ui->spinCountHook->setValue(countHook);

    int countPrompt = settings->value(QStringLiteral("Inspiration/PromptsCount"), 10).toInt();
    if (countPrompt <= 0) countPrompt = 10;
    ui->spinCountPrompt->setValue(countPrompt);

    // Restore only-human checkbox states (default false)
    ui->checkOnlyHumanHashtag->setChecked(
        settings->value(QStringLiteral("Inspiration/HashtagsOnlyHuman"), false).toBool());
    ui->checkOnlyHumanHook->setChecked(
        settings->value(QStringLiteral("Inspiration/HooksOnlyHuman"), false).toBool());
    ui->checkOnlyHumanPrompt->setChecked(
        settings->value(QStringLiteral("Inspiration/PromptsOnlyHuman"), false).toBool());

    // Restore prompts (or keep default prompts if unset)
    const QString defaultHashtagPrompt = QStringLiteral(
        "Suggest high-performing hashtags for short-form social media videos, "
        "similar in style, niche, and audience to the existing preferred hashtags. "
        "Return each hashtag on its own line (e.g. #hashtag), without numbering or extra commentary.");
    const QString defaultHookPrompt = QStringLiteral(
        "Suggest compelling, high-converting opening hook sentences (first 3 seconds) "
        "for a short-form video, matching the tone, energy, and style of our favorite hooks. "
        "Return each hook on its own line, without quotes, numbering, or commentary.");
    const QString defaultPromptPrompt = QStringLiteral(
        "Suggest punchy, fast-paced video prompts for generating short-form videos "
        "(visual scene, camera movement, and pacing), matching the style and dynamics "
        "of our favorite video prompts. Each prompt must start with 'Generate a video:' or 'Generate a video of'. "
        "Return each prompt on its own line, without numbering or commentary.");

    const QString savedHashtagPrompt = settings->value(QStringLiteral("Inspiration/HashtagsPrompt")).toString();
    ui->editPromptHashtag->setPlainText(savedHashtagPrompt.isEmpty() ? defaultHashtagPrompt : savedHashtagPrompt);

    const QString savedHookPrompt = settings->value(QStringLiteral("Inspiration/HooksPrompt")).toString();
    ui->editPromptHook->setPlainText(savedHookPrompt.isEmpty() ? defaultHookPrompt : savedHookPrompt);

    const QString savedPromptPrompt = settings->value(QStringLiteral("Inspiration/PromptsPrompt")).toString();
    ui->editPromptPrompt->setPlainText(savedPromptPrompt.isEmpty() ? defaultPromptPrompt : savedPromptPrompt);

    // Read saved preferred CLI names
    m_savedCliHashtag = settings->value(QStringLiteral("Inspiration/HashtagsCli")).toString().trimmed();
    m_savedCliHook = settings->value(QStringLiteral("Inspiration/HooksCli")).toString().trimmed();
    m_savedCliPrompt = settings->value(QStringLiteral("Inspiration/PromptsCli")).toString().trimmed();

    // Persist spin box changes to working directory settings
    connect(ui->spinCountHashtag, qOverload<int>(&QSpinBox::valueChanged), this, [](int val) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HashtagsCount"), val);
        s->sync();
    });
    connect(ui->spinCountHook, qOverload<int>(&QSpinBox::valueChanged), this, [](int val) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HooksCount"), val);
        s->sync();
    });
    connect(ui->spinCountPrompt, qOverload<int>(&QSpinBox::valueChanged), this, [](int val) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/PromptsCount"), val);
        s->sync();
    });

    // Persist only-human checkbox changes to working directory settings
    connect(ui->checkOnlyHumanHashtag, &QCheckBox::toggled, this, [](bool checked) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HashtagsOnlyHuman"), checked);
        s->sync();
    });
    connect(ui->checkOnlyHumanHook, &QCheckBox::toggled, this, [](bool checked) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HooksOnlyHuman"), checked);
        s->sync();
    });
    connect(ui->checkOnlyHumanPrompt, &QCheckBox::toggled, this, [](bool checked) {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/PromptsOnlyHuman"), checked);
        s->sync();
    });

    // Persist prompt edits to working directory settings
    connect(ui->editPromptHashtag, &QPlainTextEdit::textChanged, this, [this]() {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HashtagsPrompt"), ui->editPromptHashtag->toPlainText());
        s->sync();
    });
    connect(ui->editPromptHook, &QPlainTextEdit::textChanged, this, [this]() {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/HooksPrompt"), ui->editPromptHook->toPlainText());
        s->sync();
    });
    connect(ui->editPromptPrompt, &QPlainTextEdit::textChanged, this, [this]() {
        auto s = WorkingDirectoryManager::instance()->settings();
        s->setValue(QStringLiteral("Inspiration/PromptsPrompt"), ui->editPromptPrompt->toPlainText());
        s->sync();
    });

    // Persist CLI selections on user interaction (activated) and when current text changes
    connect(ui->comboCliHashtag, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        const QString name = ui->comboCliHashtag->itemText(idx).trimmed();
        if (!name.isEmpty())
        {
            m_savedCliHashtag = name;
            auto s = WorkingDirectoryManager::instance()->settings();
            s->setValue(QStringLiteral("Inspiration/HashtagsCli"), name);
            s->sync();
        }
    });
    connect(ui->comboCliHashtag, &QComboBox::currentTextChanged, this, [this](const QString &) {
        _saveCliIfUserChanged(ui->comboCliHashtag, QStringLiteral("Inspiration/HashtagsCli"), m_savedCliHashtag);
    });

    connect(ui->comboCliHook, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        const QString name = ui->comboCliHook->itemText(idx).trimmed();
        if (!name.isEmpty())
        {
            m_savedCliHook = name;
            auto s = WorkingDirectoryManager::instance()->settings();
            s->setValue(QStringLiteral("Inspiration/HooksCli"), name);
            s->sync();
        }
    });
    connect(ui->comboCliHook, &QComboBox::currentTextChanged, this, [this](const QString &) {
        _saveCliIfUserChanged(ui->comboCliHook, QStringLiteral("Inspiration/HooksCli"), m_savedCliHook);
    });

    connect(ui->comboCliPrompt, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        const QString name = ui->comboCliPrompt->itemText(idx).trimmed();
        if (!name.isEmpty())
        {
            m_savedCliPrompt = name;
            auto s = WorkingDirectoryManager::instance()->settings();
            s->setValue(QStringLiteral("Inspiration/PromptsCli"), name);
            s->sync();
        }
    });
    connect(ui->comboCliPrompt, &QComboBox::currentTextChanged, this, [this](const QString &) {
        _saveCliIfUserChanged(ui->comboCliPrompt, QStringLiteral("Inspiration/PromptsCli"), m_savedCliPrompt);
    });

    // Try restoring saved CLI right now and whenever new CLIs are discovered asynchronously
    _restoreSelectedClis();
    connect(m_cliList, &QAbstractItemModel::rowsInserted, this, &PaneInspiration::_restoreSelectedClis);
    connect(m_cliList, &QAbstractItemModel::modelReset, this, &PaneInspiration::_restoreSelectedClis);
}

void PaneInspiration::_saveCliIfUserChanged(QComboBox *combo, const QString &key, QString &savedCli)
{
    const QString current = combo->currentText().trimmed();
    if (current.isEmpty())
    {
        return;
    }
    // If we have a saved CLI setting that hasn't appeared yet in the combo model
    // (because available CLIs are still being discovered asynchronously),
    // do not overwrite the user's preferred CLI with Qt's automatic row-0 selection.
    if (!savedCli.isEmpty() && combo->findText(savedCli) < 0)
    {
        return;
    }
    savedCli = current;
    auto s = WorkingDirectoryManager::instance()->settings();
    s->setValue(key, current);
    s->sync();
}

void PaneInspiration::_restoreSelectedClis()
{
    const auto restoreCombo = [&](QComboBox *combo, const QString &savedCli) {
        if (!savedCli.isEmpty())
        {
            const int idx = combo->findText(savedCli);
            if (idx >= 0 && combo->currentIndex() != idx)
            {
                const QSignalBlocker blocker{combo};
                combo->setCurrentIndex(idx);
            }
        }
    };

    restoreCombo(ui->comboCliHashtag, m_savedCliHashtag);
    restoreCombo(ui->comboCliHook, m_savedCliHook);
    restoreCombo(ui->comboCliPrompt, m_savedCliPrompt);
}

void PaneInspiration::_addHashtagRow()
{
    m_hashtags->addRow(PreferredHashtags::SOURCE_HUMAN);
    const QModelIndex newIdx = m_hashtags->index(m_hashtags->rowCount() - 1, PreferredHashtags::IND_TAG);
    ui->tableViewHashtags->setCurrentIndex(newIdx);
    ui->tableViewHashtags->scrollTo(newIdx);
    ui->tableViewHashtags->edit(newIdx);
}

void PaneInspiration::_removeSelectedHashtags()
{
    _removeSelectedRows(ui->tableViewHashtags);
}

void PaneInspiration::_addHookRow()
{
    m_favoriteHooks->addRow(FavoriteHooks::SOURCE_HUMAN);
    const QModelIndex newIdx = m_favoriteHooks->index(m_favoriteHooks->rowCount() - 1, FavoriteHooks::IND_HOOK);
    ui->tableViewHooks->setCurrentIndex(newIdx);
    ui->tableViewHooks->scrollTo(newIdx);
    ui->tableViewHooks->edit(newIdx);
}

void PaneInspiration::_removeSelectedHooks()
{
    _removeSelectedRows(ui->tableViewHooks);
}

void PaneInspiration::_addPromptRow()
{
    m_favoriteVideoPrompts->addRow(FavoriteVideoPrompts::SOURCE_HUMAN);
    const QModelIndex newIdx = m_favoriteVideoPrompts->index(
        m_favoriteVideoPrompts->rowCount() - 1, FavoriteVideoPrompts::IND_PROMPT);
    ui->tableViewPrompts->setCurrentIndex(newIdx);
    ui->tableViewPrompts->scrollTo(newIdx);
    ui->tableViewPrompts->edit(newIdx);
}

void PaneInspiration::_removeSelectedPrompts()
{
    _removeSelectedRows(ui->tableViewPrompts);
}

void PaneInspiration::_removeSelectedRows(QTableView *tableView)
{
    auto *selectionModel = tableView->selectionModel();
    if (!selectionModel)
    {
        return;
    }

    const auto &selRows = selectionModel->selectedRows();
    if (selRows.isEmpty())
    {
        QMessageBox::information(this, tr("No selection"),
            tr("Select the row(s) to remove in the table first."));
        return;
    }

    QList<int> rows;
    for (const auto &index : selRows)
    {
        rows << index.row();
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    auto *model = tableView->model();
    for (int row : rows)
    {
        model->removeRow(row);
    }
}

void PaneInspiration::_generateHashtagIdea()
{
    _generateIdea(ContentType::Hashtags);
}

void PaneInspiration::_generateHookIdea()
{
    _generateIdea(ContentType::Hooks);
}

void PaneInspiration::_generatePromptIdea()
{
    _generateIdea(ContentType::VideoPrompts);
}

void PaneInspiration::_generateIdea(ContentType type)
{
    QComboBox *comboCli = nullptr;
    QPlainTextEdit *editPrompt = nullptr;
    QPushButton *buttonGenerate = nullptr;
    QLabel *labelStatus = nullptr;
    QTableView *tableView = nullptr;
    int count = 10;
    QString fullPrompt;
    QString typeName;

    switch (type)
    {
    case ContentType::Hashtags: {
        comboCli = ui->comboCliHashtag;
        editPrompt = ui->editPromptHashtag;
        buttonGenerate = ui->buttonGenerateHashtag;
        labelStatus = ui->labelStatusHashtag;
        tableView = ui->tableViewHashtags;
        count = ui->spinCountHashtag->value();
        typeName = tr("Hashtag");

        fullPrompt = QStringLiteral("Please generate exactly %1 new suggestions (one per line):\n\n%2")
                         .arg(count)
                         .arg(editPrompt->toPlainText().trimmed());
        const bool onlyHuman = ui->checkOnlyHumanHashtag->isChecked();
        const QString section = m_hashtags->asPromptSection(onlyHuman);
        if (!section.isEmpty())
        {
            fullPrompt += QStringLiteral("\n\n") + section;
        }
        break;
    }
    case ContentType::Hooks: {
        comboCli = ui->comboCliHook;
        editPrompt = ui->editPromptHook;
        buttonGenerate = ui->buttonGenerateHook;
        labelStatus = ui->labelStatusHook;
        tableView = ui->tableViewHooks;
        count = ui->spinCountHook->value();
        typeName = tr("Hook");

        fullPrompt = QStringLiteral("Please generate exactly %1 new suggestions (one per line):\n\n%2")
                         .arg(count)
                         .arg(editPrompt->toPlainText().trimmed());
        const bool onlyHuman = ui->checkOnlyHumanHook->isChecked();
        const QString section = m_favoriteHooks->asPromptSection(onlyHuman);
        if (!section.isEmpty())
        {
            fullPrompt += QStringLiteral("\n\n") + section;
        }
        break;
    }
    case ContentType::VideoPrompts: {
        comboCli = ui->comboCliPrompt;
        editPrompt = ui->editPromptPrompt;
        buttonGenerate = ui->buttonGeneratePrompt;
        labelStatus = ui->labelStatusPrompt;
        tableView = ui->tableViewPrompts;
        count = ui->spinCountPrompt->value();
        typeName = tr("Video Prompt");

        fullPrompt = QStringLiteral("Please generate exactly %1 new suggestions (one per line):\n\n%2")
                         .arg(count)
                         .arg(editPrompt->toPlainText().trimmed());
        const bool onlyHuman = ui->checkOnlyHumanPrompt->isChecked();
        const QString section = m_favoriteVideoPrompts->asPromptSection(onlyHuman);
        if (!section.isEmpty())
        {
            fullPrompt += QStringLiteral("\n\n") + section;
        }
        break;
    }
    }

    // Save the current CLI and prompt to working directory settings
    {
        auto s = WorkingDirectoryManager::instance()->settings();
        if (type == ContentType::Hashtags)
        {
            m_savedCliHashtag = comboCli->currentText().trimmed();
            s->setValue(QStringLiteral("Inspiration/HashtagsCli"), m_savedCliHashtag);
            s->setValue(QStringLiteral("Inspiration/HashtagsPrompt"), editPrompt->toPlainText());
        }
        else if (type == ContentType::Hooks)
        {
            m_savedCliHook = comboCli->currentText().trimmed();
            s->setValue(QStringLiteral("Inspiration/HooksCli"), m_savedCliHook);
            s->setValue(QStringLiteral("Inspiration/HooksPrompt"), editPrompt->toPlainText());
        }
        else if (type == ContentType::VideoPrompts)
        {
            m_savedCliPrompt = comboCli->currentText().trimmed();
            s->setValue(QStringLiteral("Inspiration/PromptsCli"), m_savedCliPrompt);
            s->setValue(QStringLiteral("Inspiration/PromptsPrompt"), editPrompt->toPlainText());
        }
        s->sync();
    }

    AbstractCli *cli = m_cliList->cliAt(comboCli->currentIndex());
    if (!cli)
    {
        QMessageBox::information(this, tr("No CLI Available"),
            tr("Please select an available AI CLI from the dropdown."));
        return;
    }

    if (editPrompt->toPlainText().trimmed().isEmpty())
    {
        QMessageBox::information(this, tr("Empty Prompt"),
            tr("Please enter a prompt asking for a similar idea."));
        return;
    }

    buttonGenerate->setEnabled(false);
    buttonGenerate->setText(tr("Generating..."));
    labelStatus->setText(tr("Running %1...").arg(cli->getName()));

    const QString workingDir = WorkingDirectoryManager::instance()->workingDir().absolutePath();
    cli->runPromptAsync(fullPrompt, workingDir, this,
        [this, type, cli, typeName, count, buttonGenerate, labelStatus, tableView](CliRunResult result) {
            buttonGenerate->setEnabled(true);
            buttonGenerate->setText(tr("Suggest next idea"));
            labelStatus->clear();

            if (!result.processStarted)
            {
                QMessageBox::warning(this, tr("Execution Error"),
                    tr("Failed to start %1. Please ensure it is installed and executable.")
                        .arg(cli->getName()));
                return;
            }

            if (result.exitCode != 0)
            {
                QMessageBox::warning(this, tr("CLI Error"),
                    tr("%1 returned error code %2:\n%3")
                        .arg(cli->getName())
                        .arg(result.exitCode)
                        .arg(result.errorOutput.isEmpty() ? result.output : result.errorOutput));
                return;
            }

            const QString rawOutput = cli->extractTextFromOutput(result.output.toUtf8()).trimmed();
            const QString cleanIdea = _cleanOutput(rawOutput);
            if (cleanIdea.isEmpty())
            {
                QMessageBox::information(this, tr("No Output"),
                    tr("The CLI returned empty output."));
                return;
            }

            const QString dialogTitle = tr("%1 (%2 requested)").arg(typeName).arg(count);
            DialogValidateIdea dialog(dialogTitle, cleanIdea, this);
            if (dialog.exec() != QDialog::Accepted)
            {
                return; // Discarded by user
            }

            const QString validatedText = dialog.ideaText().trimmed();
            if (validatedText.isEmpty())
            {
                return;
            }

            int firstNewRow = -1;
            switch (type)
            {
            case ContentType::Hashtags: {
                firstNewRow = m_hashtags->rowCount();
                const QStringList tags = PreferredHashtags::parse(validatedText);
                if (tags.isEmpty())
                {
                    m_hashtags->addHashtag(validatedText, PreferredHashtags::SOURCE_AI);
                }
                else
                {
                    for (const QString &tag : tags)
                    {
                        m_hashtags->addHashtag(tag, PreferredHashtags::SOURCE_AI);
                    }
                }
                break;
            }
            case ContentType::Hooks: {
                firstNewRow = m_favoriteHooks->rowCount();
                const QStringList lines = validatedText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString &line : lines)
                {
                    const QString h = line.trimmed();
                    if (!h.isEmpty())
                    {
                        m_favoriteHooks->addHook(h, FavoriteHooks::SOURCE_AI);
                    }
                }
                break;
            }
            case ContentType::VideoPrompts: {
                firstNewRow = m_favoriteVideoPrompts->rowCount();
                const QStringList lines = validatedText.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString &line : lines)
                {
                    const QString p = line.trimmed();
                    if (!p.isEmpty())
                    {
                        m_favoriteVideoPrompts->addPrompt(p, FavoriteVideoPrompts::SOURCE_AI);
                    }
                }
                break;
            }
            }

            if (firstNewRow >= 0 && tableView && tableView->model() && tableView->model()->rowCount() > firstNewRow)
            {
                const QModelIndex idx = tableView->model()->index(firstNewRow, 0);
                tableView->setCurrentIndex(idx);
                tableView->scrollTo(idx);
            }
        });
}

QString PaneInspiration::_cleanOutput(const QString &raw)
{
    QString s = raw.trimmed();

    // Strip markdown code fences if wrapped in ```...```
    if (s.startsWith(QLatin1String("```")))
    {
        const int firstNewline = s.indexOf(QLatin1Char('\n'));
        if (firstNewline != -1)
        {
            s = s.mid(firstNewline + 1);
        }
        if (s.endsWith(QLatin1String("```")))
        {
            s.chop(3);
        }
        s = s.trimmed();
    }

    // Process line by line: strip leading numbering like "1. ", "2) ", "- ", "* ", "• "
    static const QRegularExpression leadPrefixRegex{QStringLiteral("^(?:\\d+[.)]\\s*|[-*•]\\s*)")};
    QStringList lines = s.split(QLatin1Char('\n'));
    QStringList cleanedLines;
    for (const QString &line : lines)
    {
        QString trimmedLine = line.trimmed();
        trimmedLine.remove(leadPrefixRegex);
        trimmedLine = trimmedLine.trimmed();
        // Strip surrounding quotes
        if (trimmedLine.length() >= 2)
        {
            if ((trimmedLine.startsWith(QLatin1Char('"')) && trimmedLine.endsWith(QLatin1Char('"'))) ||
                (trimmedLine.startsWith(QLatin1Char('\'')) && trimmedLine.endsWith(QLatin1Char('\''))))
            {
                trimmedLine = trimmedLine.mid(1, trimmedLine.length() - 2).trimmed();
            }
        }
        if (!trimmedLine.isEmpty())
        {
            cleanedLines.append(trimmedLine);
        }
    }

    return cleanedLines.join(QStringLiteral("\n"));
}
