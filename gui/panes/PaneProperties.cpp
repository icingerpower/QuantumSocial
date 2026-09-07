#include "PaneProperties.h"
#include "ui_PaneProperties.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeView>
#include <QVBoxLayout>

#include "model/properties/TreeProperties.h"

PaneProperties::PaneProperties(TreeProperties *properties, TreeProperties *archive,
                               QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneProperties)
    , m_properties(properties)
    , m_archive(archive)
{
    ui->setupUi(this);

    auto *layout = new QVBoxLayout{this};
    auto *label = new QLabel{
        tr("The A/B property catalog — statistics accumulate as generated "
           "videos are published and their stats fetched. \"Origin\" shows "
           "whether a property/value was added manually, suggested by the "
           "CLI, or grounded in analyzed reference images (see \"Bootstrap "
           "from reference images...\" on the Generation page's properties "
           "list, or the working-directory setup step)."), this};
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *splitter = new QSplitter{Qt::Vertical, this};

    auto *activeGroup = new QGroupBox{tr("Active properties"), splitter};
    auto *activeLayout = new QVBoxLayout{activeGroup};
    auto *activeButtonRow = new QHBoxLayout{};
    auto *buttonAddProperty = new QPushButton{tr("Add property..."), activeGroup};
    auto *buttonAddValue = new QPushButton{tr("Add value..."), activeGroup};
    auto *buttonArchive = new QPushButton{tr("Archive"), activeGroup};
    connect(buttonAddProperty, &QPushButton::clicked, this, &PaneProperties::_addProperty);
    connect(buttonAddValue, &QPushButton::clicked, this, &PaneProperties::_addValue);
    connect(buttonArchive, &QPushButton::clicked, this, &PaneProperties::_archiveSelected);
    activeButtonRow->addWidget(buttonAddProperty);
    activeButtonRow->addWidget(buttonAddValue);
    activeButtonRow->addWidget(buttonArchive);
    activeButtonRow->addStretch(1);
    activeLayout->addLayout(activeButtonRow);
    m_treeActive = new QTreeView{activeGroup};
    activeLayout->addWidget(m_treeActive);
    splitter->addWidget(activeGroup);

    auto *archiveGroup = new QGroupBox{tr("Archived properties"), splitter};
    auto *archiveLayout = new QVBoxLayout{archiveGroup};
    auto *archiveButtonRow = new QHBoxLayout{};
    auto *buttonDelete = new QPushButton{tr("Delete permanently"), archiveGroup};
    connect(buttonDelete, &QPushButton::clicked, this, &PaneProperties::_deleteArchived);
    archiveButtonRow->addWidget(buttonDelete);
    archiveButtonRow->addStretch(1);
    archiveLayout->addLayout(archiveButtonRow);
    m_treeArchive = new QTreeView{archiveGroup};
    archiveLayout->addWidget(m_treeArchive);
    splitter->addWidget(archiveGroup);

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    _setupTree(m_treeActive, m_properties);
    _setupTree(m_treeArchive, m_archive);
}

PaneProperties::~PaneProperties()
{
    delete ui;
}

void PaneProperties::_setupTree(QTreeView *tree, TreeProperties *model)
{
    tree->setModel(model);
    tree->expandAll();
    // ResizeToContents/Stretch sections cannot be dragged by the user at
    // all — Interactive for every column (with sensible starting widths),
    // plus stretchLastSection so the last VISIBLE column still absorbs
    // extra space, keeps every column both sensibly sized AND resizable.
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->setColumnWidth(TreeProperties::IND_NAME, 200);
    tree->setColumnWidth(TreeProperties::IND_PROMPT, 320);
    tree->setColumnWidth(TreeProperties::IND_ORIGIN, 110);
    tree->header()->setStretchLastSection(true);
    tree->setColumnHidden(TreeProperties::IND_GENERATED, true);
    tree->setColumnHidden(TreeProperties::IND_STATS, true);
    QObject::connect(model, &TreeProperties::modelReset, tree, &QTreeView::expandAll);
    QObject::connect(model, &TreeProperties::rowsInserted, tree, &QTreeView::expandAll);
}

void PaneProperties::_addProperty()
{
    const QString name = QInputDialog::getText(this, tr("Add property"),
        tr("Property name (e.g. \"Background\"):"));
    if (name.trimmed().isEmpty())
    {
        return;
    }
    m_treeActive->setCurrentIndex(m_properties->addProperty(name.trimmed()));
}

void PaneProperties::_addValue()
{
    QModelIndex property = m_treeActive->currentIndex().siblingAtColumn(0);
    if (!property.isValid())
    {
        QMessageBox::information(this, tr("No property selected"),
            tr("Select a property (or one of its values) in the active "
               "list first."));
        return;
    }
    if (property.data(TreeProperties::RoleIsValue).toBool())
    {
        property = property.parent();
    }
    const QString name = QInputDialog::getText(this, tr("Add value"),
        tr("Value name (e.g. \"Street background\"):"));
    if (name.trimmed().isEmpty())
    {
        return;
    }
    const QString fragment = QInputDialog::getMultiLineText(this,
        tr("Add value"), tr("Prompt fragment injected into generation:"));
    const QModelIndex value
        = m_properties->addValue(property, name.trimmed(), fragment.trimmed());
    m_treeActive->expand(property);
    m_treeActive->setCurrentIndex(value);
}

void PaneProperties::_archiveSelected()
{
    const QModelIndex index = m_treeActive->currentIndex().siblingAtColumn(0);
    if (!index.isValid())
    {
        QMessageBox::information(this, tr("Nothing selected"),
            tr("Select a property or value in the active list first."));
        return;
    }
    m_properties->archiveTo(*m_archive, index);
}

void PaneProperties::_deleteArchived()
{
    const QModelIndex index = m_treeArchive->currentIndex().siblingAtColumn(0);
    if (!index.isValid())
    {
        QMessageBox::information(this, tr("Nothing selected"),
            tr("Select a property or value in the archived list first."));
        return;
    }
    if (QMessageBox::question(this, tr("Delete?"),
            tr("Delete \"%1\" permanently? This cannot be undone.")
                .arg(index.data().toString()))
        == QMessageBox::Yes)
    {
        m_archive->removeNode(index);
    }
}
