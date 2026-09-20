#include "DialogHooks.h"
#include "ui_DialogHooks.h"

#include "AbstractCli.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace {

const QString HOOKS_REPLY_FILE_NAME = QStringLiteral("hooks_reply.json");

class DialogEditHook : public QDialog
{
public:
    explicit DialogEditHook(const QString &hook, const QString &description, QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(hook.isEmpty() && description.isEmpty()
            ? tr("Add Hook & Description")
            : tr("Edit Hook & Description"));
        resize(560, 320);

        auto *layout = new QVBoxLayout(this);

        auto *labelHook = new QLabel(tr("Hook (short, catchy opening text):"), this);
        m_editHook = new QLineEdit(hook, this);
        m_editHook->setPlaceholderText(tr("e.g. Stop scrolling! Watch this..."));

        auto *labelDesc = new QLabel(tr("Description (post caption & hashtags):"), this);
        m_editDescription = new QPlainTextEdit(description, this);
        m_editDescription->setPlaceholderText(tr("Write the post caption with hashtags and #qs shortcode..."));

        auto *buttonBox = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

        layout->addWidget(labelHook);
        layout->addWidget(m_editHook);
        layout->addWidget(labelDesc);
        layout->addWidget(m_editDescription, 1);
        layout->addWidget(buttonBox);
    }

    QString hook() const
    {
        return m_editHook ? m_editHook->text().trimmed() : QString{};
    }

    QString description() const
    {
        return m_editDescription ? m_editDescription->toPlainText().trimmed() : QString{};
    }

private:
    QLineEdit *m_editHook = nullptr;
    QPlainTextEdit *m_editDescription = nullptr;
};

QJsonObject extractHooksJson(const QString &raw)
{
    // 1. Try directly
    QJsonDocument doc = QJsonDocument::fromJson(raw.trimmed().toUtf8());
    if (doc.isObject() && doc.object().contains(QStringLiteral("hooks")))
    {
        return doc.object();
    }

    // 2. Strip markdown code fences if present (```json ... ``` or ``` ...)
    QString text = raw;
    qsizetype fenceStart = text.indexOf(QStringLiteral("```"));
    while (fenceStart >= 0)
    {
        qsizetype nextNewline = text.indexOf(QLatin1Char('\n'), fenceStart);
        if (nextNewline >= 0)
        {
            qsizetype fenceEnd = text.indexOf(QStringLiteral("```"), nextNewline);
            if (fenceEnd > nextNewline)
            {
                QString fencedContent = text.mid(nextNewline + 1, fenceEnd - nextNewline - 1).trimmed();
                doc = QJsonDocument::fromJson(fencedContent.toUtf8());
                if (doc.isObject() && doc.object().contains(QStringLiteral("hooks")))
                {
                    return doc.object();
                }
            }
        }
        fenceStart = text.indexOf(QStringLiteral("```"), fenceStart + 3);
    }

    // 3. Substring between first '{' and last '}'
    const qsizetype first = raw.indexOf(QLatin1Char('{'));
    const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
    if (first >= 0 && last > first)
    {
        doc = QJsonDocument::fromJson(raw.mid(first, last - first + 1).toUtf8());
        if (doc.isObject() && doc.object().contains(QStringLiteral("hooks")))
        {
            return doc.object();
        }
    }

    return QJsonObject{};
}

} // namespace

DialogHooks::DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const QString &codeTag, QWidget *parent)
    : DialogHooks(hooks, [codeTag]() {
        Context ctx;
        ctx.codeTag = codeTag;
        return ctx;
    }(), parent)
{
}

DialogHooks::DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const Context &context, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DialogHooks)
    , m_context(context)
{
    ui->setupUi(this);
    _initUi(hooks);
}

DialogHooks::~DialogHooks()
{
    delete ui;
}

void DialogHooks::_initUi(const QList<QPair<QString, QString>> &hooks)
{
    const QString codeTag = m_context.codeTag;
    ui->labelHint->setText(tr("Pick the hook and the description to publish "
        "with — click a cell in each column (they do not need to be on the "
        "same line). Cells are editable; the %1 tag identifies the video's "
        "properties.").arg(codeTag.isEmpty() ? tr("short-code") : codeTag));

    for (AbstractCli *cli : m_context.availableClis)
    {
        ui->comboCli->addItem(cli->getName(), QVariant::fromValue(cli));
    }
    if (m_context.selectedCli)
    {
        const int idx = ui->comboCli->findText(m_context.selectedCli->getName());
        if (idx >= 0)
        {
            ui->comboCli->setCurrentIndex(idx);
        }
    }
    const bool hasCli = ui->comboCli->count() > 0;
    ui->buttonRegenerate->setEnabled(hasCli);
    if (!hasCli)
    {
        ui->labelStatus->setText(tr("No AI CLI available for regeneration."));
    }
    ui->progressBar->setVisible(false);

    ui->tableHooks->setColumnCount(2);
    ui->tableHooks->setHorizontalHeaderLabels({tr("Hook"), tr("Description")});
    ui->tableHooks->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableHooks->horizontalHeader()->setStretchLastSection(true);
    ui->tableHooks->setSelectionMode(QAbstractItemView::NoSelection);
    ui->tableHooks->setEditTriggers(
        QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    ui->tableHooks->setContextMenuPolicy(Qt::CustomContextMenu);

    for (int row = 0; row < hooks.size(); ++row)
    {
        QString description = hooks[row].second.trimmed();
        if (!codeTag.isEmpty() && !description.contains(codeTag))
        {
            description += QLatin1Char(' ') + codeTag;
        }
        _insertHookRow(row, hooks[row].first, description);
    }
    if (ui->tableHooks->rowCount() > 0)
    {
        m_hookRow = 0;
        m_descriptionRow = 0;
    }
    _applyHighlights();
    _updateButtonStates();

    connect(ui->tableHooks, &QTableWidget::cellClicked,
            this, &DialogHooks::_onCellClicked);
    connect(ui->tableHooks, &QTableWidget::itemChanged,
            this, &DialogHooks::_onItemChanged);
    connect(ui->tableHooks, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (auto *item = ui->tableHooks->item(row, col))
        {
            ui->tableHooks->editItem(item);
        }
    });
    connect(ui->tableHooks, &QTableWidget::customContextMenuRequested,
            this, &DialogHooks::_showContextMenu);

    connect(ui->buttonCopyHook, &QPushButton::clicked, this, &DialogHooks::_copyHook);
    connect(ui->buttonCopyDescription, &QPushButton::clicked, this, &DialogHooks::_copyDescription);
    connect(ui->buttonAdd, &QPushButton::clicked, this, &DialogHooks::_addHook);
    connect(ui->buttonDuplicate, &QPushButton::clicked, this, &DialogHooks::_duplicateHook);
    connect(ui->buttonEdit, &QPushButton::clicked, this, &DialogHooks::_editHook);
    connect(ui->buttonDelete, &QPushButton::clicked, this, &DialogHooks::_deleteHook);
    connect(ui->buttonRegenerate, &QPushButton::clicked, this, &DialogHooks::_regenerateHooks);
}

void DialogHooks::preselect(const QString &hook, const QString &description)
{
    for (int row = 0; row < ui->tableHooks->rowCount(); ++row)
    {
        if (!hook.isEmpty() && _cellText(row, 0) == hook.trimmed())
        {
            m_hookRow = row;
        }
        if (!description.isEmpty() && _cellText(row, 1) == description.trimmed())
        {
            m_descriptionRow = row;
        }
    }
    _applyHighlights();
    _updateButtonStates();
}

QString DialogHooks::selectedHook() const
{
    return (m_hookRow >= 0 && m_hookRow < ui->tableHooks->rowCount())
        ? _cellText(m_hookRow, 0) : QString{};
}

QString DialogHooks::selectedDescription() const
{
    return (m_descriptionRow >= 0 && m_descriptionRow < ui->tableHooks->rowCount())
        ? _cellText(m_descriptionRow, 1) : QString{};
}

QList<QPair<QString, QString>> DialogHooks::allHooks() const
{
    QList<QPair<QString, QString>> result;
    for (int row = 0; row < ui->tableHooks->rowCount(); ++row)
    {
        const QString hook = _cellText(row, 0);
        const QString desc = _cellText(row, 1);
        if (!hook.isEmpty() || !desc.isEmpty())
        {
            result << qMakePair(hook, desc);
        }
    }
    return result;
}

AbstractCli *DialogHooks::selectedCli() const
{
    return ui->comboCli->currentData().value<AbstractCli *>();
}

void DialogHooks::_onCellClicked(int row, int column)
{
    if (column == 0)
    {
        m_hookRow = row;
    }
    else
    {
        m_descriptionRow = row;
    }
    _applyHighlights();
    _updateButtonStates();
}

void DialogHooks::_onItemChanged(QTableWidgetItem *)
{
    if (m_updatingHighlights)
    {
        return;
    }
    _applyHighlights();
    _updateButtonStates();
}

void DialogHooks::_copyHook()
{
    QGuiApplication::clipboard()->setText(selectedHook());
}

void DialogHooks::_copyDescription()
{
    QGuiApplication::clipboard()->setText(selectedDescription());
}

void DialogHooks::_addHook()
{
    QString initialDesc;
    if (!m_context.codeTag.isEmpty())
    {
        initialDesc = m_context.codeTag;
    }
    DialogEditHook dialog(QString{}, initialDesc, this);
    if (dialog.exec() == QDialog::Accepted)
    {
        if (dialog.hook().isEmpty() && dialog.description().isEmpty())
        {
            return;
        }
        QString desc = dialog.description();
        if (!m_context.codeTag.isEmpty() && !desc.contains(m_context.codeTag))
        {
            desc += QLatin1Char(' ') + m_context.codeTag;
        }
        const int newRow = ui->tableHooks->rowCount();
        _insertHookRow(newRow, dialog.hook(), desc);
        m_hookRow = newRow;
        m_descriptionRow = newRow;
        ui->tableHooks->setCurrentCell(newRow, 0);
        _applyHighlights();
        _updateButtonStates();
    }
}

void DialogHooks::_duplicateHook()
{
    const int row = _targetRow();
    if (row < 0 || row >= ui->tableHooks->rowCount())
    {
        return;
    }
    const QString hook = _cellText(row, 0);
    const QString desc = _cellText(row, 1);
    const int newRow = row + 1;
    _insertHookRow(newRow, hook, desc);
    m_hookRow = newRow;
    m_descriptionRow = newRow;
    ui->tableHooks->setCurrentCell(newRow, 0);
    _applyHighlights();
    _updateButtonStates();
}

void DialogHooks::_editHook()
{
    const int row = _targetRow();
    if (row < 0 || row >= ui->tableHooks->rowCount())
    {
        return;
    }
    const QString currentHook = _cellText(row, 0);
    const QString currentDesc = _cellText(row, 1);
    DialogEditHook dialog(currentHook, currentDesc, this);
    if (dialog.exec() == QDialog::Accepted)
    {
        if (auto *item0 = ui->tableHooks->item(row, 0))
        {
            item0->setText(dialog.hook());
        }
        if (auto *item1 = ui->tableHooks->item(row, 1))
        {
            item1->setText(dialog.description());
        }
        _applyHighlights();
        _updateButtonStates();
    }
}

void DialogHooks::_deleteHook()
{
    const int row = _targetRow();
    if (row < 0 || row >= ui->tableHooks->rowCount())
    {
        return;
    }
    ui->tableHooks->removeRow(row);
    if (ui->tableHooks->rowCount() == 0)
    {
        m_hookRow = -1;
        m_descriptionRow = -1;
    }
    else
    {
        m_hookRow = qBound(0, m_hookRow, ui->tableHooks->rowCount() - 1);
        m_descriptionRow = qBound(0, m_descriptionRow, ui->tableHooks->rowCount() - 1);
        ui->tableHooks->setCurrentCell(m_hookRow, 0);
    }
    _applyHighlights();
    _updateButtonStates();
}

void DialogHooks::_regenerateHooks()
{
    AbstractCli *cli = selectedCli();
    if (!cli)
    {
        QMessageBox::warning(this, tr("No CLI selected"),
            tr("Please select an AI CLI to generate hooks."));
        return;
    }

    const QString workingDir = !m_context.workingDir.isEmpty() && QDir(m_context.workingDir).exists()
        ? m_context.workingDir
        : QDir::tempPath();

    const QString replyFilePath = QDir(workingDir).filePath(HOOKS_REPLY_FILE_NAME);
    QFile::remove(replyFilePath);

    ui->buttonRegenerate->setEnabled(false);
    ui->comboCli->setEnabled(false);
    ui->buttonBox->setEnabled(false);
    ui->progressBar->setRange(0, 0);
    ui->progressBar->setVisible(true);
    ui->labelStatus->setText(tr("Asking %1 for 10 more hooks...").arg(cli->getName()));

    const QString prompt = _buildRegenerationPrompt();

    cli->runPromptAsync(prompt, workingDir, this, [this, cli, workingDir, replyFilePath](CliRunResult result) {
        ui->buttonRegenerate->setEnabled(true);
        ui->comboCli->setEnabled(true);
        ui->buttonBox->setEnabled(true);
        ui->progressBar->setVisible(false);

        if (!result.processStarted || result.exitCode != 0 || result.output.trimmed().isEmpty())
        {
            QString errorMsg;
            switch (cli->classifyError(result.errorOutput))
            {
            case CliErrorKind::AuthRequired:
                errorMsg = tr("%1's login has expired — run `%2 login` in a terminal.")
                    .arg(cli->getName(), cli->getExecutable());
                break;
            case CliErrorKind::QuotaExceeded:
                errorMsg = tr("%1 hit its usage quota — wait for reset or switch CLI.")
                    .arg(cli->getName());
                break;
            default:
                errorMsg = !result.errorOutput.isEmpty()
                    ? result.errorOutput.left(300) : result.output.left(300);
                break;
            }
            ui->labelStatus->setText(tr("Generation failed: %1").arg(errorMsg));
            return;
        }

        QJsonObject object;
        QFile replyFile{replyFilePath};
        if (replyFile.open(QFile::ReadOnly))
        {
            object = extractHooksJson(QString::fromUtf8(replyFile.readAll()));
            replyFile.close();
            QFile::remove(replyFilePath);
        }

        if (object.isEmpty())
        {
            object = extractHooksJson(result.output);
        }

        QSet<QString> seenHooks;
        for (int r = 0; r < ui->tableHooks->rowCount(); ++r)
        {
            const QString existing = _cellText(r, 0).toLower();
            if (!existing.isEmpty())
            {
                seenHooks.insert(existing);
            }
        }

        QList<QPair<QString, QString>> newHooks;
        for (const QJsonValue &val : object.value(QStringLiteral("hooks")).toArray())
        {
            const QJsonObject hookObj = val.toObject();
            const QString hook = hookObj.value(QStringLiteral("hook")).toString().trimmed();
            QString desc = hookObj.value(QStringLiteral("description")).toString().trimmed();
            if (hook.isEmpty() || seenHooks.contains(hook.toLower()))
            {
                continue;
            }
            seenHooks.insert(hook.toLower());
            if (!m_context.codeTag.isEmpty() && !desc.contains(m_context.codeTag))
            {
                desc += QLatin1Char(' ') + m_context.codeTag;
            }
            newHooks << qMakePair(hook, desc);
        }

        if (newHooks.isEmpty())
        {
            if (object.contains(QStringLiteral("hooks"))
                && !object.value(QStringLiteral("hooks")).toArray().isEmpty())
            {
                ui->labelStatus->setText(
                    tr("The CLI suggested hooks, but all of them were duplicates of existing hooks."));
            }
            else
            {
                ui->labelStatus->setText(tr("CLI replied, but no valid hooks were found in JSON."));
            }
            return;
        }

        const int startRow = ui->tableHooks->rowCount();
        for (const auto &pair : newHooks)
        {
            _insertHookRow(ui->tableHooks->rowCount(), pair.first, pair.second);
        }

        if (m_hookRow < 0)
        {
            m_hookRow = startRow;
            m_descriptionRow = startRow;
        }
        ui->tableHooks->setCurrentCell(startRow, 0);
        if (auto *firstNewItem = ui->tableHooks->item(startRow, 0))
        {
            ui->tableHooks->scrollToItem(firstNewItem);
        }
        _applyHighlights();
        _updateButtonStates();

        ui->labelStatus->setText(
            tr("Added %1 new hooks from %2.").arg(newHooks.size()).arg(cli->getName()));
    });
}

void DialogHooks::_showContextMenu(const QPoint &pos)
{
    const int row = ui->tableHooks->rowAt(pos.y());
    if (row >= 0)
    {
        ui->tableHooks->setCurrentCell(row, ui->tableHooks->columnAt(pos.x()));
    }
    QMenu menu(this);
    menu.addAction(tr("Edit..."), this, &DialogHooks::_editHook);
    menu.addAction(tr("Duplicate"), this, &DialogHooks::_duplicateHook);
    menu.addAction(tr("Add..."), this, &DialogHooks::_addHook);
    menu.addAction(tr("Delete"), this, &DialogHooks::_deleteHook);
    menu.addSeparator();
    menu.addAction(tr("Copy Hook"), this, &DialogHooks::_copyHook);
    menu.addAction(tr("Copy Description"), this, &DialogHooks::_copyDescription);
    menu.exec(ui->tableHooks->viewport()->mapToGlobal(pos));
}

QString DialogHooks::_cellText(int row, int column) const
{
    if (row < 0 || row >= ui->tableHooks->rowCount())
    {
        return QString{};
    }
    const QTableWidgetItem *item = ui->tableHooks->item(row, column);
    return item ? item->text().trimmed() : QString{};
}

void DialogHooks::_applyHighlights()
{
    m_updatingHighlights = true;
    const QBrush chosen = palette().highlight();
    const QBrush chosenText = palette().highlightedText();
    for (int row = 0; row < ui->tableHooks->rowCount(); ++row)
    {
        for (int column = 0; column < 2; ++column)
        {
            QTableWidgetItem *item = ui->tableHooks->item(row, column);
            if (!item)
            {
                continue;
            }
            const bool isChosen = (column == 0 && row == m_hookRow)
                || (column == 1 && row == m_descriptionRow);
            item->setBackground(isChosen ? chosen : QBrush{});
            item->setForeground(isChosen ? chosenText : QBrush{});
        }
    }
    m_updatingHighlights = false;
}

void DialogHooks::_insertHookRow(int row, const QString &hook, const QString &description)
{
    ui->tableHooks->insertRow(row);
    auto *hookItem = new QTableWidgetItem(hook);
    auto *descItem = new QTableWidgetItem(description);
    hookItem->setFlags(hookItem->flags() | Qt::ItemIsEditable);
    descItem->setFlags(descItem->flags() | Qt::ItemIsEditable);
    ui->tableHooks->setItem(row, 0, hookItem);
    ui->tableHooks->setItem(row, 1, descItem);
}

int DialogHooks::_targetRow() const
{
    const int current = ui->tableHooks->currentRow();
    if (current >= 0 && current < ui->tableHooks->rowCount())
    {
        return current;
    }
    if (m_hookRow >= 0 && m_hookRow < ui->tableHooks->rowCount())
    {
        return m_hookRow;
    }
    if (m_descriptionRow >= 0 && m_descriptionRow < ui->tableHooks->rowCount())
    {
        return m_descriptionRow;
    }
    return ui->tableHooks->rowCount() > 0 ? 0 : -1;
}

void DialogHooks::_updateButtonStates()
{
    const bool hasRows = ui->tableHooks->rowCount() > 0;
    ui->buttonDuplicate->setEnabled(hasRows);
    ui->buttonEdit->setEnabled(hasRows);
    ui->buttonDelete->setEnabled(hasRows);
    ui->buttonCopyHook->setEnabled(hasRows && !selectedHook().isEmpty());
    ui->buttonCopyDescription->setEnabled(hasRows && !selectedDescription().isEmpty());
}

QString DialogHooks::_buildRegenerationPrompt() const
{
    QString prompt = QStringLiteral(
        "Suggest 10 short, catchy hooks and matching post descriptions for a %1 social-media video.\nContext:\n")
        .arg(m_context.videoFormatLabel.isEmpty() ? QStringLiteral("short") : m_context.videoFormatLabel);
    if (!m_context.keyword.isEmpty())
    {
        prompt += QStringLiteral("- Keyword / Product: %1\n").arg(m_context.keyword);
    }
    if (!m_context.hookIdea.isEmpty())
    {
        prompt += QStringLiteral("- Hook idea: %1\n").arg(m_context.hookIdea);
    }
    if (!m_context.videoPrompt.isEmpty())
    {
        prompt += QStringLiteral("- Video content / concept: %1\n").arg(m_context.videoPrompt);
    }
    prompt += QStringLiteral(
        "Style guidance: keep everything tasteful, brand-safe fashion-editorial — "
        "NOT overtly sexual or lingerie-styled. Confidence and glamour are fine; explicit sexiness is not.\n");
    if (!m_context.keyword.isEmpty())
    {
        prompt += QStringLiteral(
            "Product focus: this content is about \"%1\". Keep the product a clear focal point.\n")
            .arg(m_context.keyword);
        const QString keywordHashtag = QStringLiteral("#")
            + QString{m_context.keyword}.remove(QLatin1Char(' ')).toLower();
        prompt += QStringLiteral(
            "SEO — non-negotiable: the target keyword is \"%1\". EVERY one of "
            "the 10 descriptions must include \"%1\" as plain readable text somewhere in the caption "
            "AND include %2 as one of its hashtags.\n").arg(m_context.keyword, keywordHashtag);
    }
    if (!m_context.preferredHashtags.isEmpty())
    {
        prompt += QStringLiteral(
            "Preferred hashtags (chosen by the user — use ONLY these unless truly none fit, in which case add at most one relevant new one): %1\n")
            .arg(m_context.preferredHashtags.join(QStringLiteral(" ")));
    }
    if (!m_context.favoriteHooksPrompt.isEmpty())
    {
        prompt += m_context.favoriteHooksPrompt;
    }

    const auto existing = allHooks();
    if (!existing.isEmpty())
    {
        prompt += QStringLiteral("\nIMPORTANT — The following hooks ALREADY exist in the table. Do NOT duplicate or repeat them. "
                                 "Generate 10 FRESH, DIFFERENT catchy hooks with new creative angles:\n");
        for (const auto &pair : existing)
        {
            prompt += QStringLiteral("- %1\n").arg(pair.first);
        }
    }

    prompt += QStringLiteral(
        "\nOutput format: reply ONLY with a single valid JSON object as plain text (no markdown fences, no commentary). Exactly this shape:\n"
        "{\"hooks\": [{\"hook\": \"...\", \"description\": \"...\"}]}\n"
        "\"hooks\": 10 NEW short catchy hooks; each \"description\" is the matching post description, ending with relevant hashtags.\n");

    return prompt;
}
