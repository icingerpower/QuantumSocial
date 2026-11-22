#include "MainWindow.h"
#include "./ui_MainWindow.h"
#include "panes/PaneAccounts.h"
#include "panes/PaneGeneration.h"
#include "panes/PaneInspiration.h"
#include "panes/PanePublishing.h"
#include "panes/PaneResearch.h"
#include "panes/PaneWorkflow.h"
#include "model/research/ResearchStore.h"
#include "panes/PaneProperties.h"
#include "panes/PaneSettings.h"
#include "panes/PaneStatistics.h"

#include <QDir>
#include <QStandardPaths>

#include "AbstractCli.h"
#include "AvailableCliTable.h"

#include "../../common/workingdirectory/WorkingDirectoryManager.h"

#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/TableAccounts.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/TableGenerationSettings.h"
#include "model/videos/StatisticsScheduler.h"
#include "model/videos/TableVideos.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_availableCliTable(new AvailableCliTable(this))
{
    ui->setupUi(this);

    const QDir workingDir{WorkingDirectoryManager::instance()->workingDir().absolutePath()};
    m_accounts = new TableAccounts(workingDir.absolutePath(), this);
    m_videos = new TableVideos(workingDir.absolutePath(), this);
    m_properties = new TreeProperties(workingDir.absoluteFilePath(
        QStringLiteral("properties.json")), this);
    m_archive = new TreeProperties(workingDir.absoluteFilePath(
        QStringLiteral("properties_archive.json")), this);

    // The trees' statistics columns are computed from the video snapshots;
    // the videos table resolves value names through both trees (a video may
    // reference values that were archived since).
    m_properties->setVideosModel(m_videos);
    m_archive->setVideosModel(m_videos);
    m_videos->setValueNameResolver([this](const QUuid &valueId) {
        if (const auto *node = m_properties->nodeFromId(valueId))
        {
            return node->name;
        }
        if (const auto *node = m_archive->nodeFromId(valueId))
        {
            return node->name;
        }
        return QString{};
    });

    m_scheduler = new StatisticsScheduler(m_videos, this);
    m_generationSettings = new TableGenerationSettings(workingDir.absolutePath(), this);
    m_hashtags = new PreferredHashtags(workingDir.absolutePath(), this);
    m_promptLessons = new PromptLessons(workingDir.absolutePath());
    m_favoriteHooks = new FavoriteHooks(workingDir.absolutePath(), this);
    m_favoriteVideoPrompts = new FavoriteVideoPrompts(workingDir.absolutePath(), this);
    m_savedPrompts = new SavedPrompts(workingDir.absolutePath());

    // Synchronous PATH-based CLI availability check (same approach as
    // AmazonTemplate3): extra directories cover nvm-managed Node binaries
    // and user-local installs when the app is launched from a desktop
    // shortcut with a minimal PATH. AbstractCli resolves the same paths
    // again when actually running, so detection and execution agree.
    QStringList extraPaths;
    const QString home = QDir::homePath();
    extraPaths << home + QStringLiteral("/.local/bin");
    const QDir nvmNodeDir(home + QStringLiteral("/.nvm/versions/node"));
    for (const QString &version : nvmNodeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        extraPaths << nvmNodeDir.filePath(version + QStringLiteral("/bin"));
    }
    for (AbstractCli *cli : AbstractCli::ALL_CLIS())
    {
        if (!QStandardPaths::findExecutable(cli->getExecutable()).isEmpty()
            || !QStandardPaths::findExecutable(cli->getExecutable(), extraPaths).isEmpty())
        {
            m_availableClis.append(cli);
        }
    }

    auto *paneAccounts = new PaneAccounts(m_accounts, this);
    ui->tabWidget->addTab(paneAccounts, tr("Accounts"));
    auto *researchStore = new ResearchStore(workingDir.absolutePath());
    auto *research = new PaneResearch(m_accounts, researchStore, m_availableClis, this);
    researchStore->setParent(research);
    ui->tabWidget->addTab(research, tr("Research"));
    ui->tabWidget->addTab(new PaneInspiration(m_hashtags, m_favoriteHooks,
                                              m_favoriteVideoPrompts,
                                              m_availableCliTable, this),
                          tr("Inspiration"));
    auto *paneGeneration = new PaneGeneration(m_properties, m_archive, m_videos,
                                              m_generationSettings, m_hashtags,
                                              m_promptLessons, m_favoriteHooks,
                                              m_favoriteVideoPrompts, m_savedPrompts, this);
    paneGeneration->setAvailableClis(m_availableClis);
    ui->tabWidget->addTab(paneGeneration, tr("Generation"));
    auto *panePublishing = new PanePublishing(m_accounts, paneGeneration->projectsModel(),
                                              m_videos, m_availableClis, this,
                                              m_hashtags, m_favoriteHooks);
    ui->tabWidget->addTab(panePublishing, tr("Publishing"));

    auto *paneWorkflow = new PaneWorkflow(m_accounts, researchStore, research,
                                          paneGeneration, panePublishing, m_videos,
                                          m_availableClis, m_favoriteHooks, this);
    ui->tabWidget->insertTab(0, paneWorkflow, tr("Workflow"));
    ui->tabWidget->setCurrentIndex(0);

    connect(paneWorkflow, &PaneWorkflow::openResearchTabRequested, this,
            [this, research]() { ui->tabWidget->setCurrentWidget(research); });
    connect(paneWorkflow, &PaneWorkflow::openAccountsTabRequested, this,
            [this, paneAccounts]() { ui->tabWidget->setCurrentWidget(paneAccounts); });
    connect(paneGeneration, &PaneGeneration::autoPublishRequested, this,
            [this, panePublishing]() {
        ui->tabWidget->setCurrentWidget(panePublishing);
        panePublishing->openAutoPublishDialog();
    });
    connect(research, &PaneResearch::createProjectRequested, this,
            [this, paneGeneration](const QJsonObject &context, const QString &image) {
        if (paneGeneration->createResearchProject(context, image)) ui->tabWidget->setCurrentWidget(paneGeneration);
    });
    connect(research, &PaneResearch::autoBatchRequested, this,
            [this, paneGeneration](const QJsonObject &batchSpec) {
        ui->tabWidget->setCurrentWidget(paneGeneration);
        paneGeneration->startAutoBatch(batchSpec);
    });
    ui->tabWidget->addTab(new PaneProperties(m_properties, m_archive, m_videos, m_accounts, this),
                          tr("Properties"));
    ui->tabWidget->addTab(new PaneStatistics(m_videos, m_scheduler, m_accounts,
                                             paneGeneration->projectsModel(), this,
                                             m_hashtags, m_favoriteHooks),
                          tr("Statistics"));
    ui->tabWidget->addTab(new PaneSettings(m_generationSettings, m_availableCliTable,
                                          this),
                          tr("Settings"));
}

MainWindow::~MainWindow()
{
    delete ui;
    delete m_promptLessons;
    delete m_savedPrompts;
}
