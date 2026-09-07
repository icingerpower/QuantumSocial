#include "DialogHooks.h"
#include "ui_DialogHooks.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHeaderView>

DialogHooks::DialogHooks(const QList<QPair<QString, QString>> &hooks,
                         const QString &codeTag, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DialogHooks)
{
    ui->setupUi(this);
    ui->labelHint->setText(tr("Pick the hook and the description to publish "
        "with — click a cell in each column (they do not need to be on the "
        "same line). Cells are editable; the %1 tag identifies the video's "
        "properties.").arg(codeTag.isEmpty() ? tr("short-code") : codeTag));

    ui->tableHooks->setColumnCount(2);
    ui->tableHooks->setHorizontalHeaderLabels({tr("Hook"), tr("Description")});
    ui->tableHooks->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->tableHooks->horizontalHeader()->setStretchLastSection(true);
    // The hook/description picks are independent cells, tracked and
    // highlighted by hand — Qt's selection can't hold one cell per column.
    ui->tableHooks->setSelectionMode(QAbstractItemView::NoSelection);
    ui->tableHooks->setRowCount(hooks.size());
    for (int row = 0; row < hooks.size(); ++row)
    {
        QString description = hooks[row].second.trimmed();
        if (!codeTag.isEmpty() && !description.contains(codeTag))
        {
            description += QLatin1Char(' ') + codeTag;
        }
        ui->tableHooks->setItem(row, 0, new QTableWidgetItem{hooks[row].first});
        ui->tableHooks->setItem(row, 1, new QTableWidgetItem{description});
    }
    _applyHighlights();

    connect(ui->tableHooks, &QTableWidget::cellClicked,
            this, &DialogHooks::_onCellClicked);
    connect(ui->buttonCopyHook, &QPushButton::clicked, this, &DialogHooks::_copyHook);
    connect(ui->buttonCopyDescription, &QPushButton::clicked,
            this, &DialogHooks::_copyDescription);
}

DialogHooks::~DialogHooks()
{
    delete ui;
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
}

QString DialogHooks::selectedHook() const
{
    return _cellText(m_hookRow, 0);
}

QString DialogHooks::selectedDescription() const
{
    return _cellText(m_descriptionRow, 1);
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
}

void DialogHooks::_copyHook()
{
    QGuiApplication::clipboard()->setText(selectedHook());
}

void DialogHooks::_copyDescription()
{
    QGuiApplication::clipboard()->setText(selectedDescription());
}

QString DialogHooks::_cellText(int row, int column) const
{
    const QTableWidgetItem *item = ui->tableHooks->item(row, column);
    return item ? item->text().trimmed() : QString{};
}

void DialogHooks::_applyHighlights()
{
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
}
