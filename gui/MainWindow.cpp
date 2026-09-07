#include "MainWindow.h"
#include "./ui_MainWindow.h"
#include "panes/PaneAccounts.h"
#include "panes/PaneGeneration.h"
#include "panes/PaneProperties.h"
#include "panes/PaneSettings.h"
#include "panes/PaneStatistics.h"

#include <QDir>
#include <QStandardPaths>

#include "AbstractCli.h"

#include "../../common/workingdirectory/WorkingDirectoryManager.h"

#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/TableGenerationSettings.h"
#include "model/videos/StatisticsScheduler.h"
#include "model/videos/TableVideos.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    const QDir workingDir{WorkingDirectoryManager::instance()->workingDir().absolutePath()};
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
    m_hashtags = new PreferredHashtags(workingDir.absolutePath());
    m_promptLessons = new PromptLessons(workingDir.absolutePath());
    m_favoriteHooks = new FavoriteHooks(workingDir.absolutePath());
    m_favoriteVideoPrompts = new FavoriteVideoPrompts(workingDir.absolutePath());
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

    ui->tabWidget->addTab(new PaneAccounts(this), tr("Accounts"));
    auto *paneGeneration = new PaneGeneration(m_properties, m_archive, m_videos,
                                              m_generationSettings, m_hashtags,
                                              m_promptLessons, m_favoriteHooks,
                                              m_favoriteVideoPrompts, m_savedPrompts, this);
    paneGeneration->setAvailableClis(m_availableClis);
    ui->tabWidget->addTab(paneGeneration, tr("Generation"));
    ui->tabWidget->addTab(new PaneProperties(m_properties, m_archive, this),
                          tr("Properties"));
    ui->tabWidget->addTab(new PaneStatistics(m_videos, m_scheduler, this), tr("Statistics"));
    ui->tabWidget->addTab(new PaneSettings(m_generationSettings, m_hashtags,
                                          m_favoriteHooks, m_favoriteVideoPrompts,
                                          this),
                          tr("Settings"));
}

MainWindow::~MainWindow()
{
    delete ui;
    delete m_hashtags;
    delete m_promptLessons;
    delete m_favoriteHooks;
    delete m_favoriteVideoPrompts;
    delete m_savedPrompts;
}
