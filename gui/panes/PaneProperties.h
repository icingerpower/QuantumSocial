#ifndef PANEPROPERTIES_H
#define PANEPROPERTIES_H

#include <QWidget>

#include <QCoro/QCoroTask>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneProperties; }
QT_END_NAMESPACE

class QPushButton;
class QTreeView;
class ResearchStore;
class TableAccounts;
class TableVideos;
class TreeProperties;

// Read/browse AND manage surface for the A/B property catalog — same
// TreeProperties model PaneGeneration's compact properties list already
// uses, just given a full page with every column readable (name, prompt
// fragment, ORIGIN — manual / an ordinary CLI suggestion / analyzed from
// reference images, see TreeProperties::PropertyNode::origin — and the
// accumulated statistics), plus the archived properties alongside the
// active ones.
// Add property.../Add value.../Archive act on the active tree's current
// selection (mirrors PaneGeneration's properties-list right-click menu,
// exposed here as buttons); Delete acts on the archive tree's selection —
// archiving (not deleting) is how something leaves the active catalog, so
// it keeps its accumulated statistics history; only an already-archived
// entry can be permanently erased.
class PaneProperties : public QWidget
{
    Q_OBJECT

public:
    explicit PaneProperties(TreeProperties *properties, TreeProperties *archive,
                            QWidget *parent = nullptr);
    explicit PaneProperties(TreeProperties *properties, TreeProperties *archive,
                            TableVideos *videos, TableAccounts *accounts,
                            QWidget *parent = nullptr,
                            ResearchStore *researchStore = nullptr);
    ~PaneProperties();

private slots:
    void _addProperty();
    // Adds a value under the active tree's current selection (the property
    // itself, or the parent of a currently-selected value).
    void _addValue();
    // Archives the active tree's current selection (property or value) —
    // moves it to the archive tree, preserving its statistics history.
    void _archiveSelected();
    // Permanently removes the archive tree's current selection.
    void _deleteArchived();

private:
    Ui::PaneProperties *ui;
    TreeProperties *m_properties;
    TreeProperties *m_archive;
    TableVideos *m_videos;
    TableAccounts *m_accounts;
    ResearchStore *m_researchStore = nullptr;
    QTreeView *m_treeActive;
    QTreeView *m_treeArchive;
    QPushButton *m_buttonRetrieveStatistics = nullptr;
    QCoro::Task<void> m_retrieveStatisticsTask;

    QCoro::Task<void> _retrieveStatistics();
    static void _setupTree(QTreeView *tree, TreeProperties *model);
};

#endif // PANEPROPERTIES_H
