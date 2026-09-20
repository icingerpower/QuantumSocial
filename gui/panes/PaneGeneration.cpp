// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code
// (this file hosts _runImageGenerationJob, a coroutine).
#pragma GCC optimize("O1")

#include "PaneGeneration.h"
#include "ui_PaneGeneration.h"

#include <algorithm>

#include <QAudioOutput>
#include <QColor>
#include <QDataWidgetMapper>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTextEdit>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "AbstractCli.h"

#include "../../../common/workingdirectory/WorkingDirectoryManager.h"

#include "../DialogGenerateAgain.h"
#include "../DialogGenerationOptions.h"
#include "../DialogGenerationPlan.h"
#include "../DialogHooks.h"
#include "../DialogNewProject.h"
#include "../DialogReviewNewProperties.h"
#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/TableProjects.h"
#include "model/imagegen/AbstractImageGenerator.h"
#include "model/properties/TreeProperties.h"
#include "model/properties/VideoPlanProxy.h"
#include "model/videogen/AbstractVideoGenerator.h"
#include "model/videogen/TableGenerationSettings.h"
#include "model/videogen/VideoGenerationWorkflow.h"
#include "model/videogen/VideoGenerationRecipe.h"
#include "model/videos/TableVideos.h"

namespace {

const QString SETTING_PROMPT_CLI = QStringLiteral("generation/promptCli");
// Copied once, at first-run working-directory setup (see main.cpp), from a
// folder of style-reference images the user picked — reused here to ground
// the very first A/B property catalog in real visual diversity instead of
// the CLI inventing values from nothing.
const QString REFERENCE_IMAGES_SUBDIR = QStringLiteral("reference_images");
// The CLI is always asked to suggest exactly 3 prompts per option kind (see
// _suggestPlan's metaPrompt) — kept as one named constant since the A/B
// sampling loop must draw exactly this many independent recipes to match.
constexpr int PROMPT_VARIANT_COUNT = 3;
// The exact file name the image-step prompts require the CLI to produce, in
// the run's staging folder (see TableProjects::stagingDir).
const QString GENERATED_IMAGE_NAME = QStringLiteral("generation_source.png");
const QString GENERATED_IMAGE_NAME_2 = QStringLiteral("generation_source2.png");
// The suggestion reply is written by the CLI to a FILE in the staging folder
// (WebsiteEmpire2's ClaudeRunner pattern): Claude's -p stdout is known to be
// tail-truncated on long replies, which kept corrupting the JSON. Stdout
// stays as a fallback for CLIs without file tools.
const QString SUGGESTIONS_FILE_NAME = QStringLiteral("suggestions.json");
// The suggested hooks/descriptions persist here (staging, then moved into
// the generation's temp/ folder) so they can be reopened, re-picked and
// copied at any time later ("Hooks..." button).
const QString HOOKS_FILE_NAME = QStringLiteral("hooks.json");
const QString HOOK_DESCRIPTION_FILE_NAME = QStringLiteral("hook-description.txt");

// "QS-0001" -> "#qs0001": the tag appended to descriptions; tolerant lookup
// (TableVideos::recordFromShortCode) maps it back to the property recipe.
QString codeTagFromShortCode(const QString &shortCode)
{
    if (shortCode.isEmpty())
    {
        return QString{};
    }
    QString digits = shortCode;
    digits.remove(QStringLiteral("QS-"));
    return QStringLiteral("#qs") + digits.toLower();
}

void saveHooksFile(const QDir &dir, const QList<QPair<QString, QString>> &hooks,
                   const QString &shortCode)
{
    QJsonArray array;
    for (const auto &hook : hooks)
    {
        array << QJsonObject{{QStringLiteral("hook"), hook.first},
                             {QStringLiteral("description"), hook.second}};
    }
    QFile file{dir.absoluteFilePath(HOOKS_FILE_NAME)};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument{QJsonObject{
        {QStringLiteral("code"), shortCode},
        {QStringLiteral("hooks"), array},
    }}.toJson(QJsonDocument::Indented));
}

QList<QPair<QString, QString>> loadHooksFile(const QDir &dir, QString *shortCode)
{
    QList<QPair<QString, QString>> hooks;
    QFile file{dir.absoluteFilePath(HOOKS_FILE_NAME)};
    if (!file.open(QFile::ReadOnly))
    {
        return hooks;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (shortCode)
    {
        *shortCode = root.value(QStringLiteral("code")).toString();
    }
    for (const QJsonValue &value : root.value(QStringLiteral("hooks")).toArray())
    {
        const QJsonObject obj = value.toObject();
        hooks << qMakePair(obj.value(QStringLiteral("hook")).toString(),
                           obj.value(QStringLiteral("description")).toString());
    }
    return hooks;
}

// hook-description.txt is "hook\ndescription\n" (previously "hook\n\ndescription\n").
// Splitting on the first double newline (if present) or first single newline separates
// the title from the description.
void readHookDescriptionFile(const QDir &dir, QString *hook, QString *description)
{
    QFile file{dir.absoluteFilePath(HOOK_DESCRIPTION_FILE_NAME)};
    if (!file.open(QFile::ReadOnly))
    {
        return;
    }
    const QString content = QString::fromUtf8(file.readAll());
    const int splitAtDouble = content.indexOf(QStringLiteral("\n\n"));
    if (splitAtDouble >= 0)
    {
        *hook = content.left(splitAtDouble).trimmed();
        *description = content.mid(splitAtDouble + 2).trimmed();
    }
    else
    {
        const int splitAtSingle = content.indexOf(QLatin1Char('\n'));
        if (splitAtSingle >= 0)
        {
            *hook = content.left(splitAtSingle).trimmed();
            *description = content.mid(splitAtSingle + 1).trimmed();
        }
        else
        {
            *hook = content.trimmed();
            *description = QString{};
        }
    }
}

// Turns a failed CLI run into something actionable: expired logins and hit
// quotas (classified per CLI by classifyError) get a clear instruction
// instead of a raw stderr dump.
QString friendlyCliError(const AbstractCli *cli, const CliRunResult &result)
{
    switch (cli->classifyError(result.errorOutput))
    {
    case CliErrorKind::AuthRequired:
        return QObject::tr("%1's login has expired — run `%2 login` in a "
                           "terminal, then retry.")
            .arg(cli->getName(), cli->getExecutable());
    case CliErrorKind::QuotaExceeded:
        return QObject::tr("%1 hit its usage quota — wait for the reset or "
                           "switch to another CLI.").arg(cli->getName());
    case CliErrorKind::Other:
        break;
    }
    return result.errorOutput.isEmpty()
        ? result.output.left(500) : result.errorOutput.left(500);
}

QString imageStepPrompt(DialogGenerationOptions::ImageMode mode, const QString &imageRef,
                        const QString &videoFormatLabel, bool whiteBackgroundProduct,
                        const QString &outputFileName = GENERATED_IMAGE_NAME)
{
    // Without this exact marker, Antigravity's preparePrompt() concludes
    // "plain text only, do NOT use tools" for the call (see
    // CliAntigravity::preparePrompt()) — which is exactly the failure
    // observed live: it dutifully replied with just the expected file name
    // as TEXT and never actually invoked its image tool, every single
    // attempt (deterministic, not flaky — retrying alone can never fix a
    // prompt that tells the CLI not to use tools). Harmless no-op for CLIs
    // that don't look for it (Claude, Codex) — same fix already applied to
    // ImageGeneratorCli::generate()'s per-image prompt.
    const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
    if (mode == DialogGenerationOptions::ImageMode::RegenerateInput
        && whiteBackgroundProduct)
    {
        // A dedicated base shot: the product ALONE, no model/human, on a
        // pure white studio background — asked for explicitly rather than
        // inferred, since the default RegenerateInput branch below
        // deliberately keeps the subject/framing identical (it must NOT
        // drop the model on its own).
        return marker + QStringLiteral(
            "You are given the image file '%1' in the current working directory.\n"
            "Extract ONLY the product itself (e.g. the shoes/accessory — not any "
            "model, body part or person) and generate a clean e-commerce-style "
            "product shot:\n"
            "- the product alone, isolated, no model/human, no body parts\n"
            "- plain pure white background, studio product-photography lighting\n"
            "- keep the product's exact real design — same silhouette, heel "
            "shape/height, straps/closures, material, color and hardware/"
            "decorative details as in the source photo\n"
            "- sharp, high quality, no watermarks/text/UI overlays\n"
            "Save the result in the current working directory as exactly '%2'.\n"
            "Do not modify the original file. Reply with just the output file name.")
            .arg(imageRef, outputFileName);
    }
    if (mode == DialogGenerationOptions::ImageMode::RegenerateInput)
    {
        return marker + QStringLiteral(
            "You are given the image file '%1' in the current working directory.\n"
            "Create a cleaned-up, high-quality version of it:\n"
            "- remove any social network icons, UI overlays, watermarks, captions and text\n"
            "- improve sharpness and overall quality\n"
            "- keep the subject, framing, colors and style otherwise identical\n"
            "Save the result in the current working directory as exactly '%2'.\n"
            "Do not modify the original file. Reply with just the output file name.")
            .arg(imageRef, outputFileName);
    }
    return marker + QStringLiteral(
        "You are given the image file '%1' in the current working directory as "
        "inspiration.\n"
        "Generate a NEW image that is close to it but clearly different: same "
        "subject, theme, mood and overall style, but a different scene, angle or "
        "composition, and with no social network icons, watermarks or text. "
        "Use a %3 format suited for a social-media video.\n"
        "Save the result in the current working directory as exactly '%2'.\n"
        "Do not modify the original file. Reply with just the output file name.")
        .arg(imageRef, outputFileName, videoFormatLabel);
}

} // namespace

PaneGeneration::PaneGeneration(TreeProperties *properties, TreeProperties *archive,
                               TableVideos *videos,
                               TableGenerationSettings *generationSettings,
                               PreferredHashtags *hashtags,
                               PromptLessons *promptLessons,
                               FavoriteHooks *favoriteHooks,
                               FavoriteVideoPrompts *favoriteVideoPrompts,
                               SavedPrompts *savedPrompts,
                               QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::PaneGeneration)
    , m_properties(properties)
    , m_archive(archive)
    , m_videos(videos)
    , m_planProxy(nullptr)
    , m_projects(nullptr)
    , m_projectMapper(nullptr)
    , m_generationSettings(generationSettings)
    , m_hashtags(hashtags)
    , m_promptLessons(promptLessons)
    , m_favoriteHooks(favoriteHooks)
    , m_favoriteVideoPrompts(favoriteVideoPrompts)
    , m_savedPrompts(savedPrompts)
    , m_filesModel(new QFileSystemModel(this))
    , m_hooksModel(new QStandardItemModel(this))
{
    ui->setupUi(this);

    // Default split: 1/4 for the project list, 3/4 for the source/generation
    // side (still user-adjustable).
    ui->splitter_3->setStretchFactor(0, 1);
    ui->splitter_3->setStretchFactor(1, 3);
    ui->splitter_3->setSizes({300, 900});

    m_planProxy = new VideoPlanProxy(this);
    m_planProxy->setSourceModel(m_properties);

    m_projects = new TableProjects(
        WorkingDirectoryManager::instance()->workingDir().absolutePath(), this);
    ui->tableViewProjects->setModel(m_projects);
    ui->tableViewProjects->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int column : {TableProjects::IND_KEYWORD, TableProjects::IND_HOOK,
                       TableProjects::IND_IMAGE, TableProjects::IND_GEN_PROMPT,
                       TableProjects::IND_GEN_HOOK, TableProjects::IND_GEN_DESC,
                       TableProjects::IND_IMAGE2, TableProjects::IND_ID})
    {
        ui->tableViewProjects->setColumnHidden(column, true);
    }
    // Name keeps a generous default width but stays user-resizable
    // (Stretch mode would lock it); the last visible column (Created)
    // fills the remaining space so the full date/time is readable.
    ui->tableViewProjects->horizontalHeader()->resizeSection(TableProjects::IND_NAME, 300);
    ui->tableViewProjects->horizontalHeader()->setStretchLastSection(true);

    // The Source page edits the selected row directly: focus-out submits.
    m_projectMapper = new QDataWidgetMapper(this);
    m_projectMapper->setModel(m_projects);
    m_projectMapper->addMapping(ui->lineEditKeyword, TableProjects::IND_KEYWORD);
    m_projectMapper->addMapping(ui->lineEditHookIdea, TableProjects::IND_HOOK);
    m_projectMapper->setSubmitPolicy(QDataWidgetMapper::AutoSubmit);

    ui->treeViewProperties->setModel(m_properties);
    // Plan-progress columns are per-video: only meaningful through m_planProxy.
    ui->treeViewProperties->setColumnHidden(TreeProperties::IND_GENERATED, true);
    ui->treeViewProperties->setColumnHidden(TreeProperties::IND_STATS, true);
    // The catalog is managed from the tree itself (right click).
    ui->treeViewProperties->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->treeViewProperties, &QWidget::customContextMenuRequested,
            this, &PaneGeneration::_propertiesContextMenu);

    // One row per past generation of the selected project; expanding shows
    // its A/B property values.
    ui->treeViewGenerations->setColumnCount(3);
    ui->treeViewGenerations->setHeaderLabels({tr("Generation"), tr("Type"), tr("Created")});
    ui->treeViewGenerations->setColumnWidth(0, 90);
    ui->treeViewGenerations->setColumnWidth(1, 100);
    ui->treeViewGenerations->header()->setStretchLastSection(true);
    connect(ui->treeViewGenerations, &QTreeWidget::currentItemChanged,
            this, &PaneGeneration::_onGenerationSelected);
    connect(ui->treeViewGenerations, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *, int) { _openHooksDialog(); });

    // Generated content of the selected project: its folder (live — files
    // appear as soon as generation writes them). The 10 suggestions
    // themselves are NOT shown here persistently: DialogHooks (the
    // post-generation step, and the "Hooks..." button any time after) is
    // the one place to look at/pick/copy them — once a pick is made, the
    // chosen hook/description is what matters day to day (shown below,
    // selectable). The model is still populated for whenever it might be
    // needed again, just not displayed.
    ui->treeViewFiles->setModel(m_filesModel);
    ui->treeViewFiles->setColumnWidth(0, 260);
    m_hooksModel->setHorizontalHeaderLabels({tr("Hook"), tr("Description")});
    ui->tableViewHooks->setModel(m_hooksModel);
    ui->tableViewHooks->horizontalHeader()->setStretchLastSection(true);
    ui->tableViewHooks->hide();

    // In-app preview panel, built into the empty widgetPreview placeholder:
    // the picked hook/description as SELECTABLE text (QLabel is view-only
    // by default — TextSelectableByMouse/Keyboard turns on selection and
    // the standard copy context menu), and a stacked image label /
    // embedded video player for whichever content the selected generation
    // has produced.
    auto *previewLayout = ui->layoutPreview;
    m_labelGenerationHook = ui->labelGenerationHook;
    m_labelGenerationDescription = ui->labelGenerationDescription;
    m_buttonFavoriteHook = ui->buttonFavoriteHook;
    m_buttonFavoriteVideoPrompt = ui->buttonFavoriteVideoPrompt;
    connect(m_buttonFavoriteHook, &QPushButton::clicked,
            this, &PaneGeneration::_favoriteCurrentHook);
    connect(m_buttonFavoriteVideoPrompt, &QPushButton::clicked,
            this, &PaneGeneration::_favoriteCurrentVideoPrompt);
    connect(ui->buttonGenerateAgain, &QPushButton::clicked,
            this, &PaneGeneration::_generateAgain);

    m_stackedPreview = new QStackedWidget{ui->widgetPreview};
    m_labelPreviewEmpty = new QLabel{tr("Select a generation to preview its content."),
                                     m_stackedPreview};
    m_labelPreviewEmpty->setAlignment(Qt::AlignCenter);
    m_labelPreviewImage = new QLabel{m_stackedPreview};
    m_labelPreviewImage->setAlignment(Qt::AlignCenter);
    m_videoPreview = new QVideoWidget{m_stackedPreview};
    m_stackedPreview->addWidget(m_labelPreviewEmpty);
    m_stackedPreview->addWidget(m_labelPreviewImage);
    m_stackedPreview->addWidget(m_videoPreview);
    previewLayout->addWidget(m_stackedPreview, 1);

    // Slideshow browsing: hidden whenever the current generation has zero
    // or one image (a plain video, or a single-image generation).
    auto *imageNavLayout = new QHBoxLayout{};
    m_buttonPrevImage = new QPushButton{tr("< Previous"), ui->widgetPreview};
    m_labelImageNav = new QLabel{ui->widgetPreview};
    m_labelImageNav->setAlignment(Qt::AlignCenter);
    m_buttonNextImage = new QPushButton{tr("Next >"), ui->widgetPreview};
    connect(m_buttonPrevImage, &QPushButton::clicked,
            this, &PaneGeneration::_previewPrevImage);
    connect(m_buttonNextImage, &QPushButton::clicked,
            this, &PaneGeneration::_previewNextImage);
    imageNavLayout->addWidget(m_buttonPrevImage);
    imageNavLayout->addWidget(m_labelImageNav, 1);
    imageNavLayout->addWidget(m_buttonNextImage);
    previewLayout->addLayout(imageNavLayout);
    m_buttonPrevImage->hide();
    m_buttonNextImage->hide();
    m_labelImageNav->hide();

    auto *buttonPlayPause = new QPushButton{tr("Play/Pause"), ui->widgetPreview};
    connect(buttonPlayPause, &QPushButton::clicked,
            this, &PaneGeneration::_togglePreviewPlayback);
    previewLayout->addWidget(buttonPlayPause);

    m_mediaPlayer = new QMediaPlayer(this);
    auto *audioOutput = new QAudioOutput(this);
    m_mediaPlayer->setAudioOutput(audioOutput);
    m_mediaPlayer->setVideoOutput(m_videoPreview);

    connect(ui->buttonProjectAdd, &QPushButton::clicked,
            this, &PaneGeneration::_addProject);
    connect(ui->buttonProjectRemove, &QPushButton::clicked,
            this, &PaneGeneration::_removeProject);
    connect(ui->tableViewProjects->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &PaneGeneration::_currentProjectChanged);
    connect(ui->buttonGenerate, &QPushButton::clicked,
            this, &PaneGeneration::_generate);
    connect(ui->buttonViewSource, &QPushButton::clicked,
            this, &PaneGeneration::_viewSourceImage);
    connect(ui->buttonDeleteGenerated, &QPushButton::clicked,
            this, &PaneGeneration::_deleteGenerated);
    connect(ui->buttonHooks, &QPushButton::clicked,
            this, &PaneGeneration::_openHooksDialog);
    connect(ui->buttonPublish, &QPushButton::clicked,
            this, &PaneGeneration::_togglePublishSelectedVideo);
    // Quick access to the generated files (videos, images, prompts) in the
    // system file manager.
    connect(ui->buttonOpenFolder, &QPushButton::clicked, this, [this]() {
        const int row = ui->tableViewProjects->currentIndex().row();
        if (row < 0)
        {
            QMessageBox::information(this, tr("No project selected"),
                tr("Select a project in the table first."));
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            m_projects->projectGenerationsDir(row).absolutePath()));
    });
    connect(ui->comboBoxCli, &QComboBox::currentIndexChanged, this, [this]() {
        if (AbstractCli *cli = _promptCli())
        {
            QSettings().setValue(SETTING_PROMPT_CLI, cli->getName());
        }
    });

    if (m_projects->rowCount() > 0)
    {
        ui->tableViewProjects->setCurrentIndex(m_projects->index(0, TableProjects::IND_NAME));
    }
}

PaneGeneration::~PaneGeneration()
{
    delete ui;
}

void PaneGeneration::setAvailableClis(const QList<AbstractCli *> &clis)
{
    m_availableClis = clis;

    QSignalBlocker blocker{ui->comboBoxCli};
    ui->comboBoxCli->clear();
    for (AbstractCli *cli : clis)
    {
        ui->comboBoxCli->addItem(cli->getName(), QVariant::fromValue(cli));
    }

    const QString saved = QSettings().value(SETTING_PROMPT_CLI).toString();
    int restored = -1;
    // Default to a text-oriented CLI: prompt writing does not need image
    // generation, and image/agentic CLIs are slower.
    int fallback = 0;
    for (int i = 0; i < clis.size(); ++i)
    {
        if (clis[i]->getName() == saved)
        {
            restored = i;
        }
        if (fallback == 0 && !clis[i]->canGenImages())
        {
            fallback = i;
        }
    }
    ui->comboBoxCli->setCurrentIndex(restored >= 0 ? restored : fallback);
}

void PaneGeneration::_addProject()
{
    DialogNewProject dialog{this};
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const int row = m_projects->addProject(
        dialog.imagePath(), dialog.imagePath2(), dialog.keyword(), dialog.hookIdea());
    ui->tableViewProjects->setCurrentIndex(m_projects->index(row, TableProjects::IND_NAME));
}

void PaneGeneration::_removeProject()
{
    const auto &selRows = ui->tableViewProjects->selectionModel()->selectedRows();
    if (selRows.isEmpty())
    {
        QMessageBox::information(this, tr("No selection"),
            tr("Select the project(s) to remove in the table first."));
        return;
    }

    QStringList names;
    QList<int> rows;
    for (const auto &index : selRows)
    {
        rows << index.row();
        names << m_projects->data(
            m_projects->index(index.row(), TableProjects::IND_NAME)).toString();
    }
    if (QMessageBox::question(this, tr("Remove project(s)?"),
            tr("Remove %1?\nThe project folder(s) and all their files "
               "will be deleted from disk.").arg(names.join(QStringLiteral(", "))))
        != QMessageBox::Yes)
    {
        return;
    }

    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows)
    {
        m_projects->removeProject(row);
    }
}

void PaneGeneration::_currentProjectChanged(const QModelIndex &current)
{
    if (!current.isValid())
    {
        ui->lineEditKeyword->clear();
        ui->lineEditHookIdea->clear();
        ui->labelImage->clear();
        ui->labelImage2->clear();
        m_hooksModel->removeRows(0, m_hooksModel->rowCount());
        ui->treeViewGenerations->clear();
        m_mediaPlayer->stop();
        m_previewImageFiles.clear();
        m_buttonPrevImage->hide();
        m_buttonNextImage->hide();
        m_labelImageNav->hide();
        m_stackedPreview->setCurrentWidget(m_labelPreviewEmpty);
        m_labelGenerationHook->clear();
        m_labelGenerationDescription->clear();
        m_currentPreviewHook.clear();
        m_buttonFavoriteHook->setEnabled(false);
        m_currentPreviewVideoPrompt.clear();
        m_buttonFavoriteVideoPrompt->setEnabled(false);
        ui->buttonGenerateAgain->setEnabled(false);
        ui->buttonPublish->setEnabled(false);
        ui->buttonPublish->setText(tr("Mark published"));
        ui->buttonPublish->setToolTip(tr("Mark this video as published"));
        return;
    }
    m_projectMapper->setCurrentModelIndex(current);
    const int row = current.row();

    // The Generation page follows the selection: project folder, past
    // generations and the suggestions of the latest one.
    const QString projectPath = m_projects->projectGenerationsDir(row).absolutePath();
    m_filesModel->setRootPath(projectPath);
    ui->treeViewFiles->setRootIndex(m_filesModel->index(projectPath));
    _refreshGenerationsView(row);

    m_hooksModel->removeRows(0, m_hooksModel->rowCount());
    const auto records = m_videos->recordsForProject(m_projects->projectId(row));
    if (!records.isEmpty())
    {
        _refreshHooksView(m_projects->generationTempDir(row, records.first()->shortCode));
    }

    const QPixmap pixmap{m_projects->absoluteImagePath(row)};
    if (pixmap.isNull())
    {
        ui->labelImage->clear();
    }
    else
    {
        ui->labelImage->setPixmap(pixmap.scaled(
            ui->labelImage->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    const QPixmap pixmap2{m_projects->absoluteImagePath2(row)};
    if (pixmap2.isNull())
    {
        ui->labelImage2->clear();
    }
    else
    {
        ui->labelImage2->setPixmap(pixmap2.scaled(
            ui->labelImage2->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

AbstractCli *PaneGeneration::_promptCli() const
{
    return ui->comboBoxCli->currentData().value<AbstractCli *>();
}

void PaneGeneration::_generate()
{
    const QModelIndex current = ui->tableViewProjects->currentIndex();
    if (!current.isValid())
    {
        QMessageBox::information(this, tr("No project selected"),
            tr("Select the project to generate in the table first."));
        return;
    }
    if (!_promptCli())
    {
        QMessageBox::information(this, tr("No CLI available"),
            tr("No AI CLI was found on this machine — install one "
               "(claude, codex, ...) and restart."));
        return;
    }
    const int row = current.row();
    const QUuid projectId = m_projects->projectId(row);

    const QString imagePath = m_projects->absoluteImagePath(row);
    const bool hasImage = !imagePath.isEmpty() && QFileInfo::exists(imagePath);
    // When not regenerated, rides along unchanged into every job (see
    // _resolveSecondaryImage). When regenerated ("apply this on 2 images"),
    // m_runImagePath2 will be updated to point at its regenerated output.
    m_runImagePath2 = m_projects->absoluteImagePath2(row);
    const bool hasSecondaryImage = !m_runImagePath2.isEmpty() && QFileInfo::exists(m_runImagePath2);
    const QString previousGenerated = _latestGeneratedImage(row);
    const bool hasPreviousGenerated = !previousGenerated.isEmpty();

    // Always shown: the video format is picked here even without an input
    // image (the image choices are then hidden).
    DialogGenerationOptions dialog{m_availableClis, hasImage,
                                   hasPreviousGenerated, previousGenerated,
                                   hasSecondaryImage, this};
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto mode = dialog.imageMode();
    AbstractCli *imageCli = dialog.imageCli();
    const bool whiteBackgroundProduct = dialog.whiteBackgroundProduct();
    const bool applyBothImages = dialog.applyToBothImages() && hasSecondaryImage;
    const QString videoFormatLabel
        = DialogGenerationOptions::formatLabel(dialog.videoFormat());

    _openProgress();

    // Every artifact of this run (image variant, prompts, suggestions,
    // frames, rejected takes, the final video) lands in one scratch folder
    // until the generation succeeds — then _finalizeGeneration sorts it into
    // its own generations/<shortCode>/ home. Reset now: a previous
    // cancelled/failed run's leftovers must never leak into this one.
    m_projects->resetStagingDir(row);
    const QDir staging = m_projects->stagingDir(row);
    m_runGenerations.clear();

    // A/B test: sample one value per property, weighted by the accumulated
    // statistics (the randomness lives here, deterministic to inspect — the
    // CLI downstream may only drop mismatches, never swap values). One
    // recipe PER SUGGESTED PROMPT SLOT (see PROMPT_VARIANT_COUNT below and
    // _suggestPlan) so the 3 prompts the user picks between in
    // DialogGenerationPlan carry genuinely different recipes to compare —
    // sampleVariants coordinates the 3 draws (weighted shuffle instead of 3
    // independent i.i.d. picks), so a property with >= 3 candidate values
    // is GUARANTEED to show 3 distinct ones instead of the same value
    // showing up 2-3 times by chance, which independent draws frequently did.
    m_runShortCode.clear();
    m_runSampledVariants = PropertySampler::sampleVariants(
        *m_properties, *m_videos, PROMPT_VARIANT_COUNT);
    for (int variant = 0; variant < m_runSampledVariants.size(); ++variant)
    {
        QStringList picks;
        for (const auto &sampled : m_runSampledVariants[variant])
        {
            picks << QStringLiteral("%1=%2").arg(sampled.propertyName, sampled.valueName);
        }
        _logProgress(tr("A/B recipe %1/%2: %3").arg(variant + 1)
            .arg(m_runSampledVariants.size()).arg(picks.join(QStringLiteral(", "))));
    }

    if (mode == DialogGenerationOptions::ImageMode::ReusePrevious
        && hasPreviousGenerated)
    {
        // Copied into this run's own staging folder rather than referenced
        // in place: staging is self-contained and gets swept as a whole.
        const QString target = staging.absoluteFilePath(GENERATED_IMAGE_NAME);
        QFile::copy(previousGenerated, target);
        m_runImagePath = target;
        _logProgress(tr("Reusing the previously regenerated image (%1).")
            .arg(GENERATED_IMAGE_NAME));

        const QFileInfo prevInfo{previousGenerated};
        const QString candidate2 = prevInfo.dir().absoluteFilePath(GENERATED_IMAGE_NAME_2);
        if (QFileInfo::exists(candidate2))
        {
            const QString target2 = staging.absoluteFilePath(GENERATED_IMAGE_NAME_2);
            QFile::remove(target2);
            QFile::copy(candidate2, target2);
            m_runImagePath2 = target2;
            _logProgress(tr("Reusing the previously regenerated secondary image (%1).")
                .arg(GENERATED_IMAGE_NAME_2));
        }

        _suggestPlan(projectId, videoFormatLabel);
        return;
    }
    if (mode == DialogGenerationOptions::ImageMode::KeepInput || !imageCli)
    {
        m_runImagePath = imagePath;
        _suggestPlan(projectId, videoFormatLabel);
        return;
    }

    // Image step first; the prompt suggestion runs once it produced its file.
    // The CLI runs with staging as its working directory — copy the source
    // image there first so "the file '<name>' in the current working
    // directory" is actually true (staging is otherwise empty at this point).
    const QString imageRef = QFileInfo{imagePath}.fileName();
    const QString stagedSourceImage = staging.absoluteFilePath(imageRef);
    QFile::remove(stagedSourceImage);
    QFile::copy(imagePath, stagedSourceImage);

    QString imageRef2;
    if (applyBothImages && !m_runImagePath2.isEmpty() && QFileInfo::exists(m_runImagePath2))
    {
        imageRef2 = QFileInfo{m_runImagePath2}.fileName();
        if (imageRef2 == imageRef)
        {
            imageRef2 = QStringLiteral("input_secondary_%1").arg(imageRef2);
        }
        const QString stagedSourceImage2 = staging.absoluteFilePath(imageRef2);
        QFile::remove(stagedSourceImage2);
        QFile::copy(m_runImagePath2, stagedSourceImage2);
    }

    if (applyBothImages && !imageRef2.isEmpty())
    {
        _logProgress(mode == DialogGenerationOptions::ImageMode::RegenerateInput
            ? (whiteBackgroundProduct
                ? tr("Isolating product on white (image 1/2) with %1...").arg(imageCli->getName())
                : tr("Regenerating input image 1/2 with %1...").arg(imageCli->getName()))
            : tr("Bootstrapping image 1/2 with %1...").arg(imageCli->getName()));
        _runImageStep(projectId, mode, imageCli, imageRef, videoFormatLabel,
                      whiteBackgroundProduct, GENERATED_IMAGE_NAME, imageRef2);
    }
    else
    {
        _logProgress(mode == DialogGenerationOptions::ImageMode::RegenerateInput
            ? (whiteBackgroundProduct
                ? tr("Isolating the product on white with %1...").arg(imageCli->getName())
                : tr("Regenerating the input image with %1...").arg(imageCli->getName()))
            : tr("Bootstrapping a new image with %1...").arg(imageCli->getName()));
        _runImageStep(projectId, mode, imageCli, imageRef, videoFormatLabel,
                      whiteBackgroundProduct, GENERATED_IMAGE_NAME, QString{});
    }
}

void PaneGeneration::_runImageStep(const QUuid &projectId,
                                   DialogGenerationOptions::ImageMode mode,
                                   AbstractCli *imageCli, const QString &imageRef,
                                   const QString &videoFormatLabel,
                                   bool whiteBackgroundProduct,
                                   const QString &outputFileName,
                                   const QString &nextImageRef,
                                   int attempt)
{
    // The loop keeps going on ordinary failures (an image CLI can be
    // flaky) — it only stops for login/quota errors, which need a human to
    // fix before anything can succeed, and as a safety net if the exact
    // same failure repeats many times in a row (a persistent bug will not
    // fix itself by retrying forever).
    constexpr int MAX_SAME_ERROR_REPEATS = 10;
    const int row = m_projects->rowOfId(projectId);
    if (row < 0)
    {
        return;
    }
    const QDir staging = m_projects->stagingDir(row);

    setEnabled(false);
    const QDateTime callStart = QDateTime::currentDateTime();
    imageCli->runPromptAsync(
        imageStepPrompt(mode, imageRef, videoFormatLabel, whiteBackgroundProduct, outputFileName),
        staging.absolutePath(),
        this,
        [this, projectId, mode, imageCli, imageRef, videoFormatLabel,
         whiteBackgroundProduct, outputFileName, nextImageRef, attempt, callStart](CliRunResult result) {
        setEnabled(true);
        const int row = m_projects->rowOfId(projectId);
        if (row < 0)
        {
            return;
        }
        const QDir staging = m_projects->stagingDir(row);
        const QString targetPath = staging.absoluteFilePath(outputFileName);

        bool producedFile = result.processStarted && result.exitCode == 0
            && QFileInfo::exists(targetPath);

        // Same Antigravity quirk already worked around in
        // ImageGeneratorCli::generate(): its file tool can silently write to
        // a fixed internal scratch dir instead of the requested working
        // directory — check there before concluding nothing was produced.
        // The mtime guard matters: that directory is a long-lived, shared
        // dump used across many unrelated tasks, so an old file that merely
        // happens to share this generic name must not be mistaken for this
        // call's output.
        QString recoveredFrom;
        if (!producedFile && result.processStarted && result.exitCode == 0)
        {
            for (const QString &fallbackDir : imageCli->outputFallbackDirs())
            {
                const QString fallbackPath
                    = QDir{fallbackDir}.absoluteFilePath(outputFileName);
                const QFileInfo fallbackInfo{fallbackPath};
                if (!fallbackInfo.exists() || fallbackInfo.lastModified() < callStart)
                {
                    continue;
                }
                QFile::remove(targetPath);
                if (QFile::rename(fallbackPath, targetPath)
                    || (QFile::copy(fallbackPath, targetPath) && QFile::remove(fallbackPath)))
                {
                    producedFile = true;
                    recoveredFrom = fallbackDir;
                    break;
                }
            }
        }

        if (producedFile)
        {
            if (outputFileName == GENERATED_IMAGE_NAME_2)
            {
                m_runImagePath2 = targetPath;
            }
            else
            {
                m_runImagePath = targetPath;
            }
            if (!recoveredFrom.isEmpty())
            {
                _logProgress(tr("Image step: the file landed in %1 instead of "
                    "the requested folder — recovered it from there.")
                    .arg(recoveredFrom));
            }
            _logProgress(tr("Image step done (%1).").arg(outputFileName));

            if (!nextImageRef.isEmpty())
            {
                _logProgress(mode == DialogGenerationOptions::ImageMode::RegenerateInput
                    ? (whiteBackgroundProduct
                        ? tr("Isolating product on white (image 2/2) with %1...").arg(imageCli->getName())
                        : tr("Regenerating input image 2/2 with %1...").arg(imageCli->getName()))
                    : tr("Bootstrapping image 2/2 with %1...").arg(imageCli->getName()));
                _runImageStep(projectId, mode, imageCli, nextImageRef, videoFormatLabel,
                              whiteBackgroundProduct, GENERATED_IMAGE_NAME_2, QString{}, 1);
                return;
            }

            _suggestPlan(projectId, videoFormatLabel);
            return;
        }

        const QString reason = !result.processStarted || result.exitCode != 0
            ? friendlyCliError(imageCli, result)
            : tr("the CLI finished but did not produce %1. CLI output: %2")
                .arg(outputFileName, result.output.left(500));

        // Login/quota problems will not fix themselves — pause here with the
        // real reason instead of burning attempts.
        if (result.processStarted
            && imageCli->classifyError(result.errorOutput) != CliErrorKind::Other)
        {
            _finishProgress(tr("Image step paused: %1").arg(reason));
            return;
        }
        if (attempt >= MAX_SAME_ERROR_REPEATS)
        {
            _finishProgress(tr("Image step failed after %1 attempts: %2")
                .arg(MAX_SAME_ERROR_REPEATS).arg(reason));
            return;
        }
        _logProgress(tr("Image step failed (%1) — trying again (attempt %2)...")
            .arg(reason.left(200)).arg(attempt + 1));
        _runImageStep(projectId, mode, imageCli, imageRef, videoFormatLabel,
                     whiteBackgroundProduct, outputFileName, nextImageRef, attempt + 1);
    });
}

QString PaneGeneration::_latestGeneratedImage(int row) const
{
    const QUuid projectId = m_projects->projectId(row);
    const QString idString = projectId.toString(QUuid::WithoutBraces);
    const QDir genBaseDir = m_projects->projectGenerationsDir(row);
    for (const auto *record : m_videos->recordsForProject(projectId))
    {
        // Constructed directly (not via generationTempDir()) so probing
        // never creates folders for old/deleted generations as a side effect.
        const QString candidateNew = genBaseDir.absoluteFilePath(
            QStringLiteral("%1/%2/%3").arg(record->shortCode, idString, GENERATED_IMAGE_NAME));
        if (QFileInfo::exists(candidateNew))
        {
            return candidateNew;
        }
        const QString candidateNewTemp = genBaseDir.absoluteFilePath(
            QStringLiteral("%1/temp/%2").arg(record->shortCode, GENERATED_IMAGE_NAME));
        if (QFileInfo::exists(candidateNewTemp))
        {
            return candidateNewTemp;
        }
        const QString candidateLegacy = m_projects->projectDir(row).absoluteFilePath(
            QStringLiteral("generations/%1/temp/%2")
                .arg(record->shortCode, GENERATED_IMAGE_NAME));
        if (QFileInfo::exists(candidateLegacy))
        {
            return candidateLegacy;
        }
    }
    // Legacy layout (generations existed before per-generation folders did):
    // the image sat straight in the project root.
    const QString legacy
        = m_projects->projectDir(row).absoluteFilePath(GENERATED_IMAGE_NAME);
    return QFileInfo::exists(legacy) ? legacy : QString{};
}

QString PaneGeneration::_latestGeneratedImage2(int row) const
{
    const QUuid projectId = m_projects->projectId(row);
    const QString idString = projectId.toString(QUuid::WithoutBraces);
    const QDir genBaseDir = m_projects->projectGenerationsDir(row);
    for (const auto *record : m_videos->recordsForProject(projectId))
    {
        const QString candidateNew = genBaseDir.absoluteFilePath(
            QStringLiteral("%1/%2/%3").arg(record->shortCode, idString, GENERATED_IMAGE_NAME_2));
        if (QFileInfo::exists(candidateNew))
        {
            return candidateNew;
        }
        const QString candidateNewTemp = genBaseDir.absoluteFilePath(
            QStringLiteral("%1/temp/%2").arg(record->shortCode, GENERATED_IMAGE_NAME_2));
        if (QFileInfo::exists(candidateNewTemp))
        {
            return candidateNewTemp;
        }
        const QString candidateLegacy = m_projects->projectDir(row).absoluteFilePath(
            QStringLiteral("generations/%1/temp/%2")
                .arg(record->shortCode, GENERATED_IMAGE_NAME_2));
        if (QFileInfo::exists(candidateLegacy))
        {
            return candidateLegacy;
        }
    }
    const QString legacy
        = m_projects->projectDir(row).absoluteFilePath(GENERATED_IMAGE_NAME_2);
    return QFileInfo::exists(legacy) ? legacy : QString{};
}

void PaneGeneration::_suggestPlan(const QUuid &projectId,
                                  const QString &videoFormatLabel, int attempt)
{
    constexpr int MAX_SUGGESTION_TRIES = 3;
    const int row = m_projects->rowOfId(projectId);
    AbstractCli *cli = _promptCli();
    if (row < 0 || !cli)
    {
        return;
    }
    const QDir staging = m_projects->stagingDir(row);

    const QString keyword = m_projects->data(
        m_projects->index(row, TableProjects::IND_KEYWORD)).toString().trimmed();
    const QString hookIdea = m_projects->data(
        m_projects->index(row, TableProjects::IND_HOOK)).toString().trimmed();
    const QString generationImage = m_runImagePath;

    QString metaPrompt = QStringLiteral(
        "Suggest content for a short %1 social-media video.\nContext:\n")
        .arg(videoFormatLabel);
    if (!keyword.isEmpty())
    {
        metaPrompt += QStringLiteral("- Keyword: %1\n").arg(keyword);
    }
    if (!hookIdea.isEmpty())
    {
        metaPrompt += QStringLiteral("- Hook idea: %1\n").arg(hookIdea);
    }
    if (!generationImage.isEmpty() && QFileInfo::exists(generationImage))
    {
        QString imageRef = generationImage;
        if (generationImage.startsWith(staging.absolutePath()))
        {
            imageRef = QFileInfo{generationImage}.fileName();
        }
        metaPrompt += QStringLiteral(
            "- The video starts from the image file '%1' (in the current working "
            "directory).\n").arg(imageRef);
    }
    // The user writes the actual content prompt themselves now (see
    // DialogGenerationPlan) — CLI-authored prompts kept drifting off-brief
    // (wrong outfit, changed background/product) with no reliable way to
    // pin them down through instructions alone. The CLI's job here is
    // reduced to: preselect/filter the A/B properties, propose new ones when
    // the catalog needs them, and suggest hooks/descriptions.
    metaPrompt += QStringLiteral(
        "Style guidance for any new A/B property value you suggest: keep "
        "everything tasteful, brand-safe fashion-editorial — NOT overtly "
        "sexual or lingerie-styled. Avoid crotch-level or upper-thigh "
        "close-up crops, garter belts/harnesses or fetish-coded wardrobe, "
        "and overtly seductive posing/expressions. Confidence and glamour "
        "are fine; explicit sexiness is not.\n");
    if (!keyword.isEmpty())
    {
        // Root-caused live: a "full_body_silhouette" camera-framing value
        // already in the catalog says only "framed as a full-body silhouette
        // shot from head to heel" — nothing preserving product visibility —
        // so a generation sampling it repeatedly produced heels reduced to a
        // near-invisible detail. Property fragments (this one and any future
        // one) get appended to the user's own prompt VERBATIM with no other
        // check on composition, so this constraint has to be a standing rule
        // applied when you create a new value, not something enforced after
        // the fact.
        metaPrompt += QStringLiteral(
            "Product focus — non-negotiable: this content is about \"%1\". "
            "Any new A/B property value you propose must never hide or "
            "shrink \"%1\" — its fragment text is appended verbatim to "
            "whatever prompt the user writes, so it must keep the product a "
            "clear, genuine focal point on its own, never reduced to a tiny, "
            "blurry or background detail.\n").arg(keyword);
    }
    metaPrompt += QStringLiteral(
        "Using your file tools, write ONE JSON object to the file '%1' in the "
        "current working directory. The file must contain valid JSON only — no "
        "markdown fences, no commentary — exactly this shape:\n"
        "{\"hooks\": [{\"hook\": \"...\", \"description\": \"...\"}]}\n"
        "\"hooks\": 10 short catchy hooks; each \"description\" is the "
        "matching post description, ending with relevant hashtags.\n"
        "After writing the file, reply with the single word: done. If you "
        "cannot write files, reply with the JSON object itself instead.")
        .arg(SUGGESTIONS_FILE_NAME);
    // CLI-invented hashtags are pure guesswork with zero engagement data
    // behind them — once the user has curated a list (Settings pane /
    // first-run setup), pick from it instead.
    const QStringList preferredHashtags = m_hashtags->hashtags();
    if (!preferredHashtags.isEmpty())
    {
        metaPrompt += QStringLiteral(
            "Preferred hashtags (chosen by the user — use ONLY these unless "
            "truly none fit a given description, in which case add at most "
            "one relevant new one): %1\n").arg(preferredHashtags.join(QStringLiteral(" ")));
    }
    // Curated examples of hooks that actually performed well (harvested
    // from top-ranked videos) — style/tone/pattern reference for the 10
    // hooks this call writes, same rationale as preferred hashtags: real
    // data beats guesswork. (PromptLessons/FavoriteVideoPrompts are NOT
    // injected here anymore — both existed to steer CLI-authored prompt
    // TEXT, which no longer happens in this call; PromptLessons still
    // reaches the actual generation prompt directly, see
    // ImageGeneratorCli::generate() / VideoGenerationWorkflow.)
    metaPrompt += m_favoriteHooks->asPromptSection();
    if (!keyword.isEmpty())
    {
        // Root-caused live: the keyword was only ever given as loose
        // creative context, never as a requirement — only 4/10 descriptions
        // ended up with the keyword's hashtag at all, and NONE had it as
        // plain caption text. TikTok's search indexes the caption itself,
        // not just hashtags, so both matter for SEO.
        const QString keywordHashtag = QStringLiteral("#")
            + QString{keyword}.remove(QLatin1Char(' ')).toLower();
        metaPrompt += QStringLiteral(
            "SEO — non-negotiable: the target keyword is \"%1\". EVERY one of "
            "the 10 descriptions (no exceptions, not just some of them) must "
            "include \"%1\" as plain readable text somewhere in the caption "
            "AND include %2 as one of its hashtags.\n").arg(keyword, keywordHashtag);
    }
    // Every strategy slot got its OWN independently-sampled recipe (see
    // _generate()) — only variant 0 is shown to the CLI as a REPRESENTATIVE
    // example per property (asking it to judge all 3 x N values would blow
    // the prompt budget for no benefit: the relevance question — "does this
    // DIMENSION fit the brief" — does not depend on which sibling value was
    // drawn). Its answer is keyed by PROPERTY id, not value id, so the same
    // keep/drop decision applies to whichever value each strategy slot
    // actually rolled for that property.
    const auto &canonicalSampled = m_runSampledVariants.value(0);
    if (!canonicalSampled.isEmpty())
    {
        metaPrompt += QStringLiteral(
            "\nThe content is part of an A/B test. These properties are "
            "being tested (id | property | example value | prompt "
            "fragment) — the user writes their own prompt (in up to 3 "
            "strategy slots) and each kept property's fragment is appended "
            "to it automatically by the app, you are NOT writing any prompt "
            "text here, only judging which of these DIMENSIONS are "
            "relevant:\n");
        for (const auto &sampled : canonicalSampled)
        {
            metaPrompt += QStringLiteral("- %1 | %2 | %3 | %4\n")
                .arg(sampled.propertyId.toString(QUuid::WithoutBraces),
                     sampled.propertyName, sampled.valueName,
                     sampled.promptFragment);
        }
        metaPrompt += QStringLiteral(
            "Also include in the JSON object: \"properties\": [\"<id>\", ...] — "
            "the ids of the PROPERTIES you KEEP (drop ONLY dimensions that "
            "clash with the brief; keep everything else).");
    }
    // The catalog grows CLI-driven: empty → the CLI creates the initial
    // properties; otherwise it may add a missing dimension.
    const QString newPropertiesShape = QStringLiteral(
        "\"newProperties\": [{\"property\": \"...\", \"values\": [{\"name\": "
        "\"...\", \"fragment\": \"<text appended to whatever prompt the user "
        "writes>\"}], \"used\": \"<the ONE value name to preselect as the "
        "default>\"}]");
    // Tags any brand-new property/value this call creates (see below) with
    // the right origin — shown in PaneProperties so it's visible which of
    // the catalog's dimensions came from analyzing real reference images
    // versus an ordinary CLI guess.
    bool usedReferenceImagesForBootstrap = false;
    if (canonicalSampled.isEmpty())
    {
        // Style-reference images picked once at working-directory setup
        // (main.cpp) — if present, ground the very first property catalog
        // in what they actually show instead of the CLI inventing values.
        // Copied into THIS run's staging folder so the CLI (given staging
        // as its working directory below) can view them by filename.
        QStringList referenceFileNames;
        const QDir referenceImagesDir{
            WorkingDirectoryManager::instance()->workingDir()
                .absoluteFilePath(REFERENCE_IMAGES_SUBDIR)};
        if (referenceImagesDir.exists())
        {
            for (const QFileInfo &entry : referenceImagesDir.entryInfoList(
                     {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                      QStringLiteral("*.jpeg"), QStringLiteral("*.webp")},
                     QDir::Files))
            {
                const QString target = staging.absoluteFilePath(entry.fileName());
                if (QFile::exists(target) || QFile::copy(entry.absoluteFilePath(), target))
                {
                    referenceFileNames << entry.fileName();
                }
            }
        }

        usedReferenceImagesForBootstrap = !referenceFileNames.isEmpty();
        metaPrompt += QStringLiteral(
            "\nThe A/B-test property catalog is EMPTY. Create the first "
            "properties to test: also include in the JSON object %1 — 2 or 3 "
            "properties relevant to this kind of video (e.g. background, mood, "
            "camera movement), each with 2 or 3 values. For each property "
            "pick ONE value (\"used\") to preselect as the default.")
            .arg(newPropertiesShape);
        if (!referenceFileNames.isEmpty())
        {
            metaPrompt += QStringLiteral(
                "\n%1 style-reference image(s) are also available in the "
                "current working directory: %2. Look at them and ground the "
                "properties/values in the REAL visual diversity they show "
                "(background, mood, camera angle, outfit...) rather than "
                "inventing — every value should correspond to something at "
                "least one of these images actually depicts.")
                .arg(referenceFileNames.size()).arg(referenceFileNames.join(
                    QStringLiteral(", ")));
        }
    }
    else
    {
        // Root-caused live: the CLI kept inventing properties that only
        // restate what is already fixed/visible in the reference product
        // photo itself (heel height, shoe silhouette, finish...) — pure
        // redundancy with zero real A/B value, that bloated the prompt
        // until longer ones started getting partially ignored by the video
        // backend. Every proposal is reviewed by the user before it can
        // reach the catalog (DialogReviewNewProperties) — but the bar for
        // suggesting one at all should be high regardless.
        metaPrompt += QStringLiteral(
            "\nOnly if a genuinely NEW, prompt-controllable dimension is "
            "clearly missing from the catalog (max 1) you may also include "
            "%1 — same rule: pick ONE value (\"used\") to preselect as the "
            "default. Do NOT propose a property for anything already fixed "
            "or visible in the reference product photo itself (the "
            "product's own design — heel height, silhouette, finish, "
            "material, hardware...) — restating those in a text fragment is "
            "pure redundancy. When in doubt, propose NOTHING — the existing "
            "catalog is usually already enough.")
            .arg(newPropertiesShape);
    }
    if (attempt > 1)
    {
        metaPrompt += QStringLiteral(
            "\nIMPORTANT: your previous reply was invalid or truncated JSON. "
            "Write COMPLETE, valid JSON — shorten the texts if needed.");
    }

    // Stale file from a previous run must never be mistaken for the reply.
    QFile::remove(staging.absoluteFilePath(SUGGESTIONS_FILE_NAME));

    _logProgress(attempt == 1
        ? tr("Asking %1 for the A/B property picks and 10 hooks...")
            .arg(cli->getName())
        : tr("Asking %1 again (attempt %2/%3)...")
            .arg(cli->getName()).arg(attempt).arg(MAX_SUGGESTION_TRIES));
    setEnabled(false);
    cli->runPromptAsync(metaPrompt, staging.absolutePath(),
                        this, [this, projectId, cli, videoFormatLabel,
                               attempt, staging,
                               usedReferenceImagesForBootstrap](CliRunResult result) {
        constexpr int MAX_SUGGESTION_TRIES = 3;
        setEnabled(true);
        if (!result.processStarted || result.exitCode != 0
            || result.output.trimmed().isEmpty())
        {
            // Login/quota problems will not fix themselves — surface the
            // real reason immediately instead of retrying.
            if (cli->classifyError(result.errorOutput) != CliErrorKind::Other
                || attempt >= MAX_SUGGESTION_TRIES)
            {
                _finishProgress(tr("Suggestion failed: %1")
                    .arg(friendlyCliError(cli, result)));
                return;
            }
            _logProgress(tr("Suggestion run failed (%1) — starting over.")
                .arg(friendlyCliError(cli, result).left(200)));
            _suggestPlan(projectId, videoFormatLabel, attempt + 1);
            return;
        }

        // Primary source: the file the CLI wrote (immune to the stdout
        // tail-truncation Claude's -p mode is known for).
        QJsonObject object;
        QFile replyFile{staging.absoluteFilePath(SUGGESTIONS_FILE_NAME)};
        if (replyFile.open(QFile::ReadOnly))
        {
            object = QJsonDocument::fromJson(replyFile.readAll()).object();
        }
        // Fallback for CLIs without file tools: parse the stdout between the
        // first '{' and the last '}'.
        const QString raw = result.output;
        if (object.isEmpty())
        {
            const qsizetype first = raw.indexOf(QLatin1Char('{'));
            const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
            if (first >= 0 && last > first)
            {
                object = QJsonDocument::fromJson(
                    raw.mid(first, last - first + 1).toUtf8()).object();
            }
        }
        QList<QPair<QString, QString>> hooks;
        for (const QJsonValue &value : object.value(QStringLiteral("hooks")).toArray())
        {
            const QJsonObject hookObj = value.toObject();
            hooks << qMakePair(hookObj.value(QStringLiteral("hook")).toString(),
                               hookObj.value(QStringLiteral("description")).toString());
        }
        // The CLI no longer writes any prompt text (see DialogGenerationPlan)
        // — hooks are the only thing left it must actually produce, so an
        // empty hooks list is the sign of an invalid/truncated reply now.
        if (hooks.isEmpty())
        {
            if (attempt < MAX_SUGGESTION_TRIES)
            {
                _logProgress(tr("The reply was not valid JSON (likely "
                    "truncated) — asking again."));
                _suggestPlan(projectId, videoFormatLabel, attempt + 1);
                return;
            }
            // Keep the full reply on disk so the failure can be debugged.
            const QString dumpPath
                = QStringLiteral("/tmp/quantumsocial/suggestion_reply.txt");
            QDir{}.mkpath(QStringLiteral("/tmp/quantumsocial"));
            QFile dumpFile{dumpPath};
            if (dumpFile.open(QFile::WriteOnly))
            {
                dumpFile.write(raw.toUtf8());
            }
            _finishProgress(tr("Suggestion failed after %1 attempts — the CLI "
                "did not reply with the expected JSON: %2 [full reply: %3]")
                .arg(MAX_SUGGESTION_TRIES).arg(raw.left(300), dumpPath));
            return;
        }

        // Hooks are picked MANUALLY, whenever the user is ready (the
        // "Hooks..." button/double-click) — never auto-popped up right after
        // generation, which forced a blind pick before anyone had actually
        // seen the result (e.g. no way to tell if "Black or nude high
        // heels?" fits until the generated shot is on screen). Persisted now
        // so they stay accessible once the generation exists.
        saveHooksFile(staging, hooks, QString{});
        _refreshHooksView(staging);

        // The CLI's relevance filter on the sampled A/B properties: absent
        // key = no filtering (all kept); present key = only listed PROPERTY
        // ids stay checked, the user overrides in the dialog. Applied
        // identically to every prompt slot's independently-sampled variant
        // — the decision is about the DIMENSION, not the specific value
        // rolled for it (see the metaPrompt comment above).
        QList<QList<DialogGenerationPlan::PlanProperty>> variantProperties;
        variantProperties.resize(m_runSampledVariants.size());
        const bool cliFiltered = object.contains(QStringLiteral("properties"));
        QSet<QUuid> keptByCli;
        for (const QJsonValue &value
             : object.value(QStringLiteral("properties")).toArray())
        {
            const QUuid id = QUuid::fromString(value.toString());
            if (!id.isNull())
            {
                keptByCli.insert(id);
            }
        }
        for (int variant = 0; variant < m_runSampledVariants.size(); ++variant)
        {
            for (const auto &sampled : m_runSampledVariants[variant])
            {
                DialogGenerationPlan::PlanProperty planProperty;
                planProperty.propertyId = sampled.propertyId;
                planProperty.propertyName = sampled.propertyName;
                for (const auto &sibling : sampled.siblings)
                {
                    planProperty.options << DialogGenerationPlan::PlanPropertyOption{
                        sibling.id, sibling.name, sibling.promptFragment};
                }
                planProperty.selectedOptionId = sampled.id;
                planProperty.checked
                    = !cliFiltered || keptByCli.contains(sampled.propertyId);
                variantProperties[variant] << planProperty;
            }
        }

        // Properties proposed by the CLI: NOTHING here reaches the catalog
        // without explicit approval first (DialogReviewNewProperties) — the
        // CLI kept inventing properties that only restate what the
        // reference photo already fixes/shows (heel height, silhouette...),
        // pure prompt bloat with no real A/B value.
        const QJsonArray newPropertiesArray
            = object.value(QStringLiteral("newProperties")).toArray();
        QSet<int> approvedNewProperties;
        {
            QList<DialogReviewNewProperties::Proposal> proposals;
            for (int i = 0; i < newPropertiesArray.size(); ++i)
            {
                const QJsonObject propertyObj = newPropertiesArray[i].toObject();
                const QString propertyName
                    = propertyObj.value(QStringLiteral("property")).toString().trimmed();
                if (propertyName.isEmpty())
                {
                    continue;
                }
                QStringList valueLines;
                for (const QJsonValue &valueEntry
                     : propertyObj.value(QStringLiteral("values")).toArray())
                {
                    const QJsonObject valueObj = valueEntry.toObject();
                    const QString valueName
                        = valueObj.value(QStringLiteral("name")).toString().trimmed();
                    const QString fragment
                        = valueObj.value(QStringLiteral("fragment")).toString().trimmed();
                    if (!valueName.isEmpty())
                    {
                        valueLines << tr("%1 — %2").arg(valueName, fragment);
                    }
                }
                proposals << DialogReviewNewProperties::Proposal{
                    i, propertyName, valueLines.join(QStringLiteral("\n"))};
            }
            if (!proposals.isEmpty())
            {
                DialogReviewNewProperties reviewDialog{proposals, this};
                if (reviewDialog.exec() == QDialog::Accepted)
                {
                    const auto approved = reviewDialog.approvedIndices();
                    approvedNewProperties = QSet<int>{approved.begin(), approved.end()};
                }
                // Cancelled, or nothing ticked: approvedNewProperties stays
                // empty — every proposal is discarded, exactly as if the
                // CLI had never suggested them.
                _logProgress(tr("%1/%2 new propertie(s) approved.")
                    .arg(approvedNewProperties.size()).arg(proposals.size()));
            }
        }
        for (int newPropertyIndex = 0; newPropertyIndex < newPropertiesArray.size();
             ++newPropertyIndex)
        {
            if (!approvedNewProperties.contains(newPropertyIndex))
            {
                continue;
            }
            const QJsonObject propertyObj = newPropertiesArray[newPropertyIndex].toObject();
            const QString propertyName
                = propertyObj.value(QStringLiteral("property")).toString().trimmed();
            if (propertyName.isEmpty())
            {
                continue;
            }
            QModelIndex propertyIndex;
            for (TreeProperties::PropertyNode *node : m_properties->topLevelNodes())
            {
                if (node->name.compare(propertyName, Qt::CaseInsensitive) == 0)
                {
                    propertyIndex = m_properties->indexFromNode(node);
                    break;
                }
            }
            const QString origin = usedReferenceImagesForBootstrap
                ? QStringLiteral("image-bootstrap") : QStringLiteral("cli");
            if (!propertyIndex.isValid())
            {
                propertyIndex = m_properties->addProperty(propertyName, origin);
                _logProgress(tr("New A/B property from the CLI: %1")
                    .arg(propertyName));
            }
            TreeProperties::PropertyNode *propertyNode = m_properties->nodeFromId(
                propertyIndex.data(TreeProperties::RoleId).toUuid());
            const QString usedName
                = propertyObj.value(QStringLiteral("used")).toString().trimmed();

            for (const QJsonValue &valueEntry
                 : propertyObj.value(QStringLiteral("values")).toArray())
            {
                const QJsonObject valueObj = valueEntry.toObject();
                const QString valueName
                    = valueObj.value(QStringLiteral("name")).toString().trimmed();
                if (valueName.isEmpty() || !propertyNode)
                {
                    continue;
                }
                const QString fragment
                    = valueObj.value(QStringLiteral("fragment")).toString().trimmed();
                QUuid valueId;
                for (TreeProperties::PropertyNode *child : propertyNode->children)
                {
                    if (child->name.compare(valueName, Qt::CaseInsensitive) == 0)
                    {
                        valueId = child->id;
                        break;
                    }
                }
                if (valueId.isNull())
                {
                    const QModelIndex valueIndex
                        = m_properties->addValue(propertyIndex, valueName, fragment, origin);
                    valueId = valueIndex.data(TreeProperties::RoleId).toUuid();
                }
                if (valueName.compare(usedName, Qt::CaseInsensitive) == 0)
                {
                    // A brand-new property has exactly one value so far (the
                    // one the CLI just wove in) — every prompt slot shares
                    // it identically, unlike the already-established
                    // properties above, which each slot samples on its own.
                    DialogGenerationPlan::PlanProperty planProperty;
                    planProperty.propertyId = propertyNode->id;
                    planProperty.propertyName = propertyName;
                    planProperty.options << DialogGenerationPlan::PlanPropertyOption{
                        valueId, valueName, fragment};
                    planProperty.selectedOptionId = valueId;
                    planProperty.checked = true;
                    for (QList<DialogGenerationPlan::PlanProperty> &properties
                         : variantProperties)
                    {
                        properties << planProperty;
                    }
                }
            }
        }

        _logProgress(tr("Received the suggestions — waiting for your choices..."));
        DialogGenerationPlan dialog{variantProperties, m_availableClis,
                                    m_savedPrompts, videoFormatLabel, this};
        if (dialog.exec() != QDialog::Accepted)
        {
            _finishProgress(tr("Cancelled."));
            return;
        }
        const int row = m_projects->rowOfId(projectId);
        if (row < 0)
        {
            _finishProgress(tr("The project no longer exists."));
            return;
        }
        const auto plan = dialog.plan();
        // Every ticked option confirmed its OWN A/B values (whichever prompt
        // slot it selected) — summed here only for a friendly log line, the
        // actual per-job attribution travels on each GenerationJob below.
        QSet<QUuid> allKeptValueIds{plan.imagePropertyValueIds.begin(),
                                    plan.imagePropertyValueIds.end()};
        allKeptValueIds.unite(QSet<QUuid>{plan.slideshowPropertyValueIds.begin(),
                                          plan.slideshowPropertyValueIds.end()});
        for (const auto &video : plan.videos)
        {
            allKeptValueIds.unite(
                QSet<QUuid>{video.propertyValueIds.begin(), video.propertyValueIds.end()});
        }
        _logProgress(tr("%1 A/B propertie(s) confirmed across the plan.")
            .arg(allKeptValueIds.size()));
        // The project's stored generation prompt: the "most video" one picked.
        const QString mainPrompt = !plan.videos.isEmpty() ? plan.videos.first().prompt
            : plan.slideshow ? plan.slideshowPrompt
            : plan.imagePrompt;
        m_projects->setData(m_projects->index(row, TableProjects::IND_GEN_PROMPT),
                            mainPrompt);
        _logProgress(tr("Plan saved on the project."));

        // Every ticked option is its own independent generation (job), each
        // carrying the A/B values confirmed for the SPECIFIC prompt slot it
        // selected — two ticked options can point at different slots, so
        // this cannot be a single plan-wide list. Jobs are grouped by the
        // external resource they'd contend for — a specific video backend,
        // or a specific CLI executable — so jobs in DIFFERENT groups run
        // CONCURRENTLY (a video via Gemini's browser alongside a slideshow
        // via an AI CLI are independent processes with nothing to fight
        // over), while jobs in the SAME group (two ticked options both
        // wanting the same CLI, say) run one after another so they never
        // race for one session. An earlier version silently dropped every
        // option but the first ticked video.
        QList<GenerationJob> jobs;
        if (plan.oneImage && !plan.imagePrompt.isEmpty())
        {
            for (int take = 0; take < plan.imageCount; ++take)
            {
                jobs << GenerationJob{GenerationJob::Image, plan.imagePrompt, QString{},
                                      dialog.imageCli(), plan.imagePropertyValueIds};
            }
        }
        if (plan.slideshow && !plan.slideshowPrompt.isEmpty())
        {
            for (int take = 0; take < plan.slideshowCount; ++take)
            {
                jobs << GenerationJob{GenerationJob::Slideshow, plan.slideshowPrompt,
                                      QString{}, dialog.slideshowCli(),
                                      plan.slideshowPropertyValueIds};
            }
        }
        for (const auto &video : plan.videos)
        {
            // The video format line is NOT added here: VideoGenerationWorkflow
            // re-ensures it on the final text of EVERY attempt instead (its
            // per-attempt perturbation drops the prompt's last sentence, and
            // its CLI rewrites replace the prompt wholesale — both used to
            // silently lose a format line baked in this early).
            for (int take = 0; take < video.count; ++take)
            {
                jobs << GenerationJob{GenerationJob::Video, video.prompt,
                                      video.generatorId, nullptr, video.propertyValueIds};
            }
        }
        m_runVideoFormatLabel = videoFormatLabel;
        _queueJobs(row, projectId, jobs);
    });
}

void PaneGeneration::_queueJobs(int row, const QUuid &projectId,
                                 const QList<GenerationJob> &jobs)
{
    if (jobs.isEmpty())
    {
        _finishProgress(tr("Nothing ticked to generate."));
        return;
    }

    m_jobGroups.clear();
    m_jobCounter = 0;
    for (GenerationJob job : jobs)
    {
        if (job.kind == GenerationJob::Video && !job.repeatUnchanged)
        {
            job.settings = m_generationSettings->settingsFor(job.generatorId);
        }
        m_jobGroups[_jobGroupKey(job)] << job;
    }
    // Snapshotted ONCE, before any job folder exists, so concurrent
    // jobs never see each other's in-progress files (see
    // _prepareJobStaging).
    m_baseStagingEntries = m_projects->stagingDir(row).entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    _logProgress(tr("%1 job(s) in %2 group(s) queued — different groups "
        "run in parallel.").arg(jobs.size()).arg(m_jobGroups.size()));
    if (m_progress.cancelBtn)
    {
        m_progress.cancelBtn->show();
    }
    setEnabled(false);
    // Deferred to the next event-loop turn: a job that finishes
    // SYNCHRONOUSLY (e.g. "no CLI chosen" skip) could otherwise empty
    // m_jobGroups and trigger the all-done wrap-up (hooks dialog)
    // before every OTHER group even got a chance to start.
    const auto groupKeys = m_jobGroups.keys();
    for (const QString &groupKey : groupKeys)
    {
        QTimer::singleShot(0, this, [this, row, projectId, groupKey]() {
            _runNextInGroup(row, projectId, groupKey);
        });
    }
}

QString PaneGeneration::_jobGroupKey(const GenerationJob &job)
{
    if (job.kind == GenerationJob::Video)
    {
        return QStringLiteral("video:") + job.generatorId;
    }
    return QStringLiteral("cli:")
        + (job.cli ? job.cli->getExecutable() : QStringLiteral("none"));
}

QString PaneGeneration::_jobLabel(const GenerationJob &job)
{
    if (job.kind == GenerationJob::Video)
    {
        AbstractVideoGenerator *generator
            = AbstractVideoGenerator::ALL_VIDEO_GENERATORS().value(job.generatorId);
        return tr("Video — %1").arg(generator ? generator->getName() : job.generatorId);
    }
    const QString kind
        = job.kind == GenerationJob::Slideshow ? tr("Slideshow") : tr("Image");
    return QStringLiteral("%1 — %2").arg(kind, job.cli ? job.cli->getName() : tr("no CLI"));
}

QDir PaneGeneration::_prepareJobStaging(int row, const QString &jobTag)
{
    const QDir jobDir{m_projects->stagingDir(row).absoluteFilePath(jobTag)};
    QDir{}.mkpath(jobDir.absolutePath());
    for (const QFileInfo &entry : m_baseStagingEntries)
    {
        const QString target = jobDir.absoluteFilePath(entry.fileName());
        if (entry.isDir())
        {
            _copyDirRecursively(entry.absoluteFilePath(), target);
        }
        else
        {
            QFile::copy(entry.absoluteFilePath(), target);
        }
    }
    return jobDir;
}

QString PaneGeneration::_resolveJobImage(const QDir &jobStaging) const
{
    if (m_runImagePath.isEmpty() || !QFileInfo::exists(m_runImagePath))
    {
        return QString{};
    }
    // m_runImagePath already points inside the SHARED base staging (image
    // step / reuse-previous) or at the original source in the project root
    // ("keep input") — either way, this job's isolated folder needs its own
    // copy; the base snapshot already put one there for the former case.
    const QString target = jobStaging.absoluteFilePath(
        QFileInfo{m_runImagePath}.fileName());
    if (!QFileInfo::exists(target))
    {
        QFile::copy(m_runImagePath, target);
    }
    return target;
}

QString PaneGeneration::_resolveSecondaryImage(const QDir &jobStaging) const
{
    if (m_runImagePath2.isEmpty() || !QFileInfo::exists(m_runImagePath2))
    {
        return QString{};
    }
    const QString target = jobStaging.absoluteFilePath(
        QFileInfo{m_runImagePath2}.fileName());
    if (!QFileInfo::exists(target))
    {
        QFile::copy(m_runImagePath2, target);
    }
    return target;
}

void PaneGeneration::_runNextInGroup(int row, const QUuid &projectId, QString groupKey)
{
    QList<GenerationJob> &queue = m_jobGroups[groupKey];
    if (m_batchCancelled)
    {
        queue.clear();
    }
    if (queue.isEmpty())
    {
        m_jobGroups.remove(groupKey);
        if (m_jobGroups.isEmpty())
        {
            // Every group is done (finished, or aborted after its own
            // failure) — the shared staging scratch, and every per-job
            // subfolder under it, is no longer needed by anything.
            setEnabled(true);
            m_projects->stagingDir(row).removeRecursively();
            if (m_batchCancelled)
            {
                _finishProgress(tr("Cancelled. %1 completed generation(s) saved.")
                    .arg(m_runGenerations.size()));
            }
            else if (!m_runGenerations.isEmpty())
            {
                // The last generation to finish is already selected/previewed
                // (see _runImageGenerationJob / _startVideoGeneration) — pick
                // a hook only once the result has actually been seen, via the
                // "Hooks..." button (or double-click), not blindly right now.
                _finishProgress(tr("Done. Review the result, then use "
                    "\"Hooks...\" to pick a hook/description for it."));
            }
            else
            {
                _finishProgress(tr("All jobs failed."));
            }
        }
        return;
    }
    const GenerationJob job = queue.takeFirst();
    const QDir jobStaging = _prepareJobStaging(
        row, QStringLiteral("job_%1").arg(++m_jobCounter));
    if (job.kind == GenerationJob::Video)
    {
        _startVideoGeneration(row, projectId, groupKey, job, jobStaging);
    }
    else
    {
        // insert_or_assign (not operator[]/assignment), which would require
        // QCoro::Task to be default-constructible before it can move-assign.
        m_imageJobTasks.insert_or_assign(groupKey.toStdString(),
            _runImageGenerationJob(row, projectId, groupKey, job, jobStaging));
    }
}

QCoro::Task<void> PaneGeneration::_runImageGenerationJob(int row, QUuid projectId,
                                                         QString groupKey,
                                                         GenerationJob job,
                                                         QDir jobStaging)
{
    AbstractImageGenerator *generator
        = AbstractImageGenerator::ALL_IMAGE_GENERATORS().value(QStringLiteral("cli"));
    const QString label = _jobLabel(job);
    if (!generator || !job.cli)
    {
        _logProgress(tr("[%1] Skipped: no image-capable CLI was chosen.").arg(label));
        QTimer::singleShot(0, this, [this, row, projectId, groupKey]() {
            _runNextInGroup(row, projectId, groupKey);
        });
        co_return;
    }

    QStringList referenceImages;
    const QString referenceImage = _resolveJobImage(jobStaging);
    if (!referenceImage.isEmpty())
    {
        referenceImages << referenceImage;
    }
    // The project's optional second image (a different angle of the same
    // product) — extra reference material for the design-fidelity check
    // (see ImageGeneratorCli::generate()), never itself regenerated.
    const QString secondaryImage = _resolveSecondaryImage(jobStaging);
    if (!secondaryImage.isEmpty())
    {
        referenceImages << secondaryImage;
    }
    const int imageCount = job.kind == GenerationJob::Slideshow ? 5 : 1;

    // The image CLI has no separate aspect-ratio control (unlike a video
    // backend, which can enforce it outside the prompt too) — the only way
    // to make it respect the chosen format is to state it explicitly in
    // every per-image prompt, the same way the video prompts already do.
    QVariantMap settings;
    if (!m_runVideoFormatLabel.isEmpty())
    {
        settings.insert(QStringLiteral("formatLabel"), m_runVideoFormatLabel);
    }
    const QString lessonsSection = m_promptLessons->asPromptSection();
    if (!lessonsSection.isEmpty())
    {
        settings.insert(QStringLiteral("knownPitfalls"), lessonsSection);
    }

    _logProgress(tr("[%1] Generating...").arg(label));
    // Named local, assigned (not copy-initialized) from the co_await
    // expression: GCC 13 ICEs on temporaries owned by a co_await call.
    AbstractImageGenerator::Result result;
    result = co_await generator->generate(job.prompt, referenceImages, imageCount,
        jobStaging.absolutePath(), settings, job.cli,
        [this, label](const QString &message) {
            _logProgress(tr("[%1] %2").arg(label, message));
        },
        [this](const QString &lesson) {
            m_promptLessons->addLesson(lesson);
        });

    if (!result.errorMessage.isEmpty())
    {
        // A slideshow can fail partway (e.g. image 3/5) with some real
        // images already produced — errorMessage is the failure signal so
        // a partial set is never silently treated as a success. Only THIS
        // job's group is aborted; independent groups keep going.
        _logProgress(tr("[%1] Failed: %2").arg(label, result.errorMessage));
        m_jobGroups[groupKey].clear();
        QTimer::singleShot(0, this, [this, row, projectId, groupKey]() {
            _runNextInGroup(row, projectId, groupKey);
        });
        co_return;
    }

    QString shortCode;
    const QDir genDir = _finalizeGeneration(row, projectId, result.imagePaths,
                                            jobStaging, job.keptValueIds, &shortCode);
    m_runGenerations << qMakePair(genDir, shortCode);
    _logProgress(tr("[%1] Record %2 created (%3 A/B propertie(s)) — filed in %4.")
        .arg(label, shortCode).arg(job.keptValueIds.size())
        .arg(genDir.absolutePath()));

    _refreshGenerationsView(row);
    for (int i = 0; i < ui->treeViewGenerations->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem *item = ui->treeViewGenerations->topLevelItem(i);
        if (item->text(0) == shortCode)
        {
            ui->treeViewGenerations->setCurrentItem(item);
            break;
        }
    }
    QTimer::singleShot(0, this, [this, row, projectId, groupKey]() {
        _runNextInGroup(row, projectId, groupKey);
    });
}

AbstractCli *PaneGeneration::_imageCli() const
{
    const QString savedName
        = QSettings().value(QStringLiteral("generation/imageCli")).toString();
    AbstractCli *fallback = nullptr;
    for (AbstractCli *cli : m_availableClis)
    {
        if (!cli->canGenImages())
        {
            continue;
        }
        if (cli->getName() == savedName)
        {
            return cli;
        }
        if (!fallback)
        {
            fallback = cli;
        }
    }
    return fallback;
}

void PaneGeneration::_startVideoGeneration(int row, const QUuid &projectId,
                                           const QString &groupKey,
                                           const GenerationJob &job,
                                           const QDir &jobStaging)
{
    const QString label = _jobLabel(job);
    // Each job owns its workflow and signal connections. Its finished handler
    // runs queued, after the coroutine has returned, before starting the next.
    auto *workflow = new VideoGenerationWorkflow(this);

    VideoGenerationWorkflow::Request request;
    request.generatorId = job.generatorId;
    request.repeatUnchanged = job.repeatUnchanged;
    request.prompt = job.prompt;
    request.imagePath = job.repeatUnchanged ? QString{} : _resolveJobImage(jobStaging);
    // The project's optional second image — extra static reference material
    // (see VideoGenerationWorkflow::Request::extraImagePaths), never subject
    // to the self-correction loop that regenerates imagePath.
    const QString secondaryImage = job.repeatUnchanged ? QString{} : _resolveSecondaryImage(jobStaging);
    if (!secondaryImage.isEmpty())
    {
        request.extraImagePaths << secondaryImage;
    }
    if (job.repeatUnchanged)
    {
        for (const QString &fileName : job.sourceImageFiles)
        {
            request.extraImagePaths << jobStaging.absoluteFilePath(fileName);
        }
    }
    // Everything the workflow produces (video, prompt file, rejected takes,
    // extracted frames) lands in this job's OWN isolated staging folder —
    // _finalizeGeneration sorts it all out on success.
    request.outputDir = jobStaging.absolutePath();
    request.contextSummary = QStringLiteral("%1 — %2").arg(
        m_projects->data(m_projects->index(row, TableProjects::IND_KEYWORD)).toString(),
        m_projects->data(m_projects->index(row, TableProjects::IND_HOOK)).toString());
    request.videoFormatLabel = m_runVideoFormatLabel;
    request.settings = job.settings;
    request.decisionCli = _promptCli();
    request.imageCli = _imageCli();
    request.knownPitfalls = m_promptLessons->asPromptSection();
    request.recordLesson = [this](const QString &lesson) {
        m_promptLessons->addLesson(lesson);
    };

    // The workflow logs into the shared progress dialog; connections have
    // the dialog as receiver, so they die with it and the next run starts
    // clean.
    if (!m_progressDlg)
    {
        _openProgress();
    }
    if (m_progress.cancelBtn)
    {
        m_progress.cancelBtn->show();
        connect(m_progress.cancelBtn, &QPushButton::clicked,
                workflow, &VideoGenerationWorkflow::cancel);
    }
    connect(workflow, &VideoGenerationWorkflow::progress,
            m_progressDlg, [this, label](const QString &message) {
        _logProgress(QStringLiteral("[%1] %2").arg(label, message));
    });
    connect(workflow, &VideoGenerationWorkflow::finished, m_progressDlg,
            [this, row, projectId, groupKey, label, job, jobStaging, workflow]
            (const QString &videoPath, const QString &error) {
        workflow->deleteLater();
        if (!videoPath.isEmpty())
        {
            // Close the statistics loop: the generated video becomes a
            // VideoRecord referencing the confirmed A/B values, filed into
            // its own generations/<shortCode>/ folder — its short code
            // identifies the recipe from any published copy, and the
            // snapshots fetched later feed the next sampling round.
            QString shortCode;
            const QDir genDir = _finalizeGeneration(row, projectId, {videoPath},
                                                    jobStaging, job.keptValueIds,
                                                    &shortCode);
            m_runGenerations << qMakePair(genDir, shortCode);

            const QString newVideoPath
                = genDir.absoluteFilePath(QFileInfo{videoPath}.fileName());
            _logProgress(tr("[%1] Video generated and saved: %2")
                .arg(label, newVideoPath));

            _refreshGenerationsView(row);
            for (int i = 0; i < ui->treeViewGenerations->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem *item = ui->treeViewGenerations->topLevelItem(i);
                if (item->text(0) == shortCode)
                {
                    ui->treeViewGenerations->setCurrentItem(item);
                    break;
                }
            }
            _runNextInGroup(row, projectId, groupKey);
        }
        else
        {
            // A failure needs attention — abort only THIS group's
            // remaining jobs; independent groups already running keep going.
            _logProgress(tr("[%1] %2").arg(label,
                error.isEmpty() ? tr("Video generation stopped.") : error));
            m_jobGroups[groupKey].clear();
            _runNextInGroup(row, projectId, groupKey);
        }
    }, Qt::QueuedConnection);
    _logProgress(tr("[%1] Starting...").arg(label));
    workflow->start(request);
}

QDir PaneGeneration::_finalizeGeneration(int row, const QUuid &projectId,
                                         const QStringList &outputPaths,
                                         const QDir &jobStaging,
                                         const QList<QUuid> &keptValueIds,
                                         QString *outShortCode)
{
    const QUuid recordId
        = m_videos->addVideo(keptValueIds, {}, QStringLiteral("original"), projectId);
    for (const QUuid &valueId : keptValueIds)
    {
        m_videos->setValueChecked(recordId, valueId,
                                  TableVideos::ValueCheck::Generated, true);
    }
    QString shortCode;
    if (const auto *record = m_videos->recordFromId(recordId))
    {
        shortCode = record->shortCode;
    }
    m_runShortCode = shortCode; // convenience for any remaining single-job reader
    if (outShortCode)
    {
        *outShortCode = shortCode;
    }

    const QDir genDir = m_projects->generationDir(row, shortCode);
    const QDir tempDir = m_projects->generationTempDir(row, shortCode);

    // This job's own output(s) are the "important" artifact — MOVED to the
    // top level of the generation folder.
    for (const QString &outputPath : outputPaths)
    {
        const QString name = QFileInfo{outputPath}.fileName();
        const QString target = genDir.absoluteFilePath(name);
        if (QFile::exists(outputPath) && outputPath != target)
        {
            QFile::remove(target);
            QDir{}.rename(outputPath, target);
        }
    }
    // Everything else in THIS JOB'S OWN isolated staging folder (its copy
    // of the shared source-image/prompts/suggestions context, plus
    // whatever else it produced) is working material — MOVED into this
    // generation's temp/. Safe to move (not copy): jobStaging is exclusive
    // to this one job, no sibling job reads it.
    for (const QFileInfo &entry
         : jobStaging.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
    {
        const QString target = tempDir.absoluteFilePath(entry.fileName());
        if (entry.isDir())
        {
            QDir{target}.removeRecursively();
        }
        else
        {
            QFile::remove(target);
        }
        QDir{}.rename(entry.absoluteFilePath(), target);
    }
    return genDir;
}

void PaneGeneration::_copyDirRecursively(const QString &sourcePath, const QString &targetPath)
{
    const QDir sourceDir{sourcePath};
    QDir{}.mkpath(targetPath);
    for (const QFileInfo &entry
         : sourceDir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
    {
        const QString target = QDir{targetPath}.absoluteFilePath(entry.fileName());
        if (entry.isDir())
        {
            _copyDirRecursively(entry.absoluteFilePath(), target);
        }
        else
        {
            QFile::copy(entry.absoluteFilePath(), target);
        }
    }
}

void PaneGeneration::_saveChosenHookDescription(
    int row, const QList<QPair<QDir, QString>> &generations,
    const QString &hook, const QString &description)
{
    // Recorded on the project: a convenient "most recent pick" default for
    // the Hooks... button and the plan dialog's preselect.
    m_projects->setData(m_projects->index(row, TableProjects::IND_GEN_HOOK), hook);
    m_projects->setData(m_projects->index(row, TableProjects::IND_GEN_DESC),
                        description);

    // Also next to the content in each generation's own folder — tagged
    // with THAT generation's OWN recipe code, not a shared one, since
    // several independent generations (with different property recipes in
    // principle) can share one hook idea — so browsing the Dropbox folder
    // alone is enough to view AND publish.
    for (const auto &generation : generations)
    {
        QString taggedDescription = description;
        const QString tag = codeTagFromShortCode(generation.second);
        if (!tag.isEmpty() && !taggedDescription.contains(tag))
        {
            taggedDescription += QLatin1Char(' ') + tag;
        }
        QFile file{generation.first.absoluteFilePath(HOOK_DESCRIPTION_FILE_NAME)};
        if (file.open(QFile::WriteOnly))
        {
            QTextStream stream{&file};
            if (!hook.isEmpty() && !taggedDescription.isEmpty())
            {
                stream << hook << "\n" << taggedDescription << "\n";
            }
            else if (!hook.isEmpty())
            {
                stream << hook << "\n";
            }
            else
            {
                stream << taggedDescription << "\n";
            }
        }
    }

    if (auto *current = ui->treeViewGenerations->currentItem())
    {
        _onGenerationSelected(current, nullptr);
    }
}

void PaneGeneration::_refreshHooksView(const QDir &hooksDir)
{
    m_hooksModel->removeRows(0, m_hooksModel->rowCount());
    QString shortCode;
    const auto hooks = loadHooksFile(hooksDir, &shortCode);
    const QString tag = codeTagFromShortCode(shortCode);
    for (const auto &hook : hooks)
    {
        QString description = hook.second.trimmed();
        if (!tag.isEmpty() && !description.contains(tag))
        {
            description += QLatin1Char(' ') + tag;
        }
        m_hooksModel->appendRow({new QStandardItem{hook.first},
                                 new QStandardItem{description}});
    }
}

void PaneGeneration::_refreshGenerationsView(int row)
{
    ui->treeViewGenerations->clear();
    if (row < 0)
    {
        return;
    }
    const auto resolveLabel = [this](const QUuid &valueId) {
        TreeProperties::PropertyNode *node = m_properties->nodeFromId(valueId);
        if (!node)
        {
            node = m_archive->nodeFromId(valueId);
        }
        if (!node)
        {
            return valueId.toString(QUuid::WithoutBraces).left(8);
        }
        return node->parent
            ? QStringLiteral("%1 — %2").arg(node->parent->name, node->name)
            : node->name;
    };

    const auto records = m_videos->recordsForProject(m_projects->projectId(row));
    for (const auto *record : records)
    {
        auto *topItem = new QTreeWidgetItem{ui->treeViewGenerations};
        topItem->setText(0, record->shortCode);
        topItem->setText(1, _generationTypeLabel(
            m_projects->generationDir(row, record->shortCode)));
        topItem->setText(2, record->generatedDate.toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd hh:mm")));
        topItem->setData(0, Qt::UserRole, record->id);
        if (m_videos->isPublished(record->id))
        {
            _applyGenerationPublishedStyle(topItem, true);
        }
        for (const QUuid &valueId : record->propertyValueIds)
        {
            new QTreeWidgetItem{topItem, {resolveLabel(valueId)}};
        }
    }
    ui->treeViewGenerations->expandAll();
}

void PaneGeneration::_onGenerationSelected(QTreeWidgetItem *current, QTreeWidgetItem *)
{
    const int row = ui->tableViewProjects->currentIndex().row();
    const TableVideos::VideoRecord *record = nullptr;
    if (current && row >= 0)
    {
        QTreeWidgetItem *topItem = current->parent() ? current->parent() : current;
        record = m_videos->recordFromId(topItem->data(0, Qt::UserRole).toUuid());
    }
    if (!record)
    {
        m_labelGenerationHook->clear();
        m_labelGenerationDescription->clear();
        m_currentPreviewHook.clear();
        m_buttonFavoriteHook->setEnabled(false);
        m_currentPreviewVideoPrompt.clear();
        m_buttonFavoriteVideoPrompt->setEnabled(false);
        ui->buttonGenerateAgain->setEnabled(false);
        ui->buttonPublish->setEnabled(false);
        ui->buttonPublish->setText(tr("Mark published"));
        ui->buttonPublish->setToolTip(tr("Mark this video as published"));
        m_mediaPlayer->stop();
        m_previewImageFiles.clear();
        m_buttonPrevImage->hide();
        m_buttonNextImage->hide();
        m_labelImageNav->hide();
        m_stackedPreview->setCurrentWidget(m_labelPreviewEmpty);
        return;
    }

    const QDir genDir = m_projects->generationDir(row, record->shortCode);
    QString hook, description;
    readHookDescriptionFile(genDir, &hook, &description);
    // The generation's prompt (video only — image prompts aren't persisted
    // the same way) lives in its temp/ folder as generation_prompt.txt,
    // written by AbstractVideoGenerator::runGeneratorScript for whichever
    // attempt actually succeeded.
    const QString promptPath = m_projects->generationTempDir(row, record->shortCode)
        .absoluteFilePath(QStringLiteral("generation_prompt.txt"));
    QFile promptFile{promptPath};
    m_currentPreviewVideoPrompt.clear();
    if (promptFile.open(QFile::ReadOnly))
    {
        m_currentPreviewVideoPrompt = QString::fromUtf8(promptFile.readAll()).trimmed();
    }
    m_buttonFavoriteVideoPrompt->setEnabled(!m_currentPreviewVideoPrompt.isEmpty());
    ui->buttonGenerateAgain->setEnabled(!m_currentPreviewVideoPrompt.isEmpty());
    const bool published = m_videos->isPublished(record->id);
    ui->buttonPublish->setEnabled(true);
    ui->buttonPublish->setText(published ? tr("Unpublish") : tr("Mark published"));
    ui->buttonPublish->setToolTip(published
        ? tr("Unmark this video as published")
        : tr("Mark this video as published"));
    m_labelGenerationHook->setText(
        hook.isEmpty() ? tr("(no hook chosen yet — use Hooks...)") : hook);
    m_labelGenerationDescription->setText(description);
    m_currentPreviewHook = hook;
    m_buttonFavoriteHook->setEnabled(!hook.isEmpty());
    _showPreview(genDir);
}

QString PaneGeneration::_generationTypeLabel(const QDir &generationDir)
{
    if (!generationDir.entryList(
            {QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm"),
             QStringLiteral("*.mkv")}, QDir::Files).isEmpty())
    {
        return tr("Video");
    }
    const int imageCount = generationDir.entryList(
        {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
         QStringLiteral("*.webp")}, QDir::Files).size();
    if (imageCount > 1)
    {
        return tr("Slideshow (%1)").arg(imageCount);
    }
    if (imageCount == 1)
    {
        return tr("Image");
    }
    // Reached whenever a generation has a VideoRecord (so it's kept, along
    // with its A/B statistics) but no video/image at its folder — in
    // practice always "Delete generated" ran: it intentionally clears the
    // files but keeps the record, so past recipes remain visible for
    // statistics history. Spelled out rather than a bare "—" since that
    // was easy to mistake for something not having worked.
    return tr("(files deleted)");
}

void PaneGeneration::_togglePublishSelectedVideo()
{
    QTreeWidgetItem *current = ui->treeViewGenerations->currentItem();
    if (!current)
    {
        return;
    }
    QTreeWidgetItem *topItem = current->parent() ? current->parent() : current;
    const QUuid videoId = topItem->data(0, Qt::UserRole).toUuid();
    const auto *record = m_videos->recordFromId(videoId);
    if (!record)
    {
        return;
    }

    const bool nowPublished = !m_videos->isPublished(videoId);
    m_videos->setPublished(videoId, nowPublished);
    _applyGenerationPublishedStyle(topItem, nowPublished);
    ui->buttonPublish->setText(nowPublished ? tr("Unpublish") : tr("Mark published"));
    ui->buttonPublish->setToolTip(nowPublished
        ? tr("Unmark this video as published")
        : tr("Mark this video as published"));
}

void PaneGeneration::_applyGenerationPublishedStyle(QTreeWidgetItem *topItem, bool published)
{
    if (!topItem)
    {
        return;
    }
    const QColor darkGreen(34, 100, 48);
    const QColor darkGreenText(230, 255, 230);
    const int cols = ui->treeViewGenerations->columnCount();
    for (int col = 0; col < cols; ++col)
    {
        if (published)
        {
            topItem->setBackground(col, darkGreen);
            topItem->setForeground(col, darkGreenText);
        }
        else
        {
            topItem->setBackground(col, QBrush{});
            topItem->setForeground(col, QBrush{});
        }
    }
}

void PaneGeneration::_showPreview(const QDir &generationDir)
{
    m_mediaPlayer->stop();
    m_previewImageFiles.clear();
    m_previewImageIndex = 0;
    m_buttonPrevImage->hide();
    m_buttonNextImage->hide();
    m_labelImageNav->hide();

    const QStringList videoFiles = generationDir.entryList(
        {QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm"),
         QStringLiteral("*.mkv")}, QDir::Files);
    if (!videoFiles.isEmpty())
    {
        m_mediaPlayer->setSource(
            QUrl::fromLocalFile(generationDir.absoluteFilePath(videoFiles.first())));
        m_stackedPreview->setCurrentWidget(m_videoPreview);
        m_mediaPlayer->play();
        return;
    }
    // A slideshow produces several images at the generation folder's top
    // level (image_01.png...) — load all of them and let prev/next browse
    // the set, instead of only ever showing the first one.
    const QStringList imageFiles = generationDir.entryList(
        {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
         QStringLiteral("*.webp")}, QDir::Files, QDir::Name);
    if (!imageFiles.isEmpty())
    {
        for (const QString &fileName : imageFiles)
        {
            m_previewImageFiles << generationDir.absoluteFilePath(fileName);
        }
        const bool multipleImages = m_previewImageFiles.size() > 1;
        m_buttonPrevImage->setVisible(multipleImages);
        m_buttonNextImage->setVisible(multipleImages);
        m_labelImageNav->setVisible(multipleImages);
        _showPreviewImage(0);
        m_stackedPreview->setCurrentWidget(m_labelPreviewImage);
        return;
    }
    m_stackedPreview->setCurrentWidget(m_labelPreviewEmpty);
}

void PaneGeneration::_showPreviewImage(int index)
{
    if (m_previewImageFiles.isEmpty())
    {
        return;
    }
    m_previewImageIndex = qBound(0, index, m_previewImageFiles.size() - 1);
    const QPixmap pixmap{m_previewImageFiles.at(m_previewImageIndex)};
    m_labelPreviewImage->setPixmap(pixmap.scaled(m_labelPreviewImage->size(),
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_labelImageNav->setText(
        tr("%1 / %2").arg(m_previewImageIndex + 1).arg(m_previewImageFiles.size()));
}

void PaneGeneration::_previewPrevImage()
{
    _showPreviewImage(m_previewImageIndex - 1);
}

void PaneGeneration::_previewNextImage()
{
    _showPreviewImage(m_previewImageIndex + 1);
}

void PaneGeneration::_favoriteCurrentHook()
{
    if (m_currentPreviewHook.isEmpty())
    {
        return;
    }
    m_favoriteHooks->addHook(m_currentPreviewHook);
    _logProgress(tr("Saved as a favorite hook: %1").arg(m_currentPreviewHook));
}

void PaneGeneration::_favoriteCurrentVideoPrompt()
{
    if (m_currentPreviewVideoPrompt.isEmpty())
    {
        return;
    }
    m_favoriteVideoPrompts->addPrompt(m_currentPreviewVideoPrompt);
    _logProgress(tr("Saved as a favorite video prompt: %1")
        .arg(m_currentPreviewVideoPrompt.left(150)));
}

void PaneGeneration::_generateAgain()
{
    const int row = ui->tableViewProjects->currentIndex().row();
    QTreeWidgetItem *item = ui->treeViewGenerations->currentItem();
    if (row < 0 || !item || !m_jobGroups.isEmpty() || m_currentPreviewVideoPrompt.isEmpty())
    {
        return;
    }
    if (item->parent())
    {
        item = item->parent();
    }
    const auto *record = m_videos->recordFromId(item->data(0, Qt::UserRole).toUuid());
    if (!record)
    {
        return;
    }
    const QUuid projectId = m_projects->projectId(row);
    const auto propertyIds = record->propertyValueIds;
    const QString shortCode = record->shortCode;
    const QDir sourceDir = m_projects->generationTempDir(row, shortCode);
    VideoGenerationRecipe recipe;
    QString error;
    QString note;
    const auto &generators = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    if (sourceDir.exists(QStringLiteral("generation_config.json")))
    {
        if (!VideoGenerationRecipe::load(sourceDir, &recipe, &error))
        {
            QMessageBox::warning(this, tr("Cannot generate again"), error);
            return;
        }
        note = tr("Saved backend settings and source images will be reused.");
    }
    else
    {
        // Legacy generations saved the prompt and source files but no backend
        // snapshot. Be explicit about using current settings in the dialog.
        if (generators.size() != 1)
        {
            QMessageBox::warning(this, tr("Cannot generate again"),
                tr("This older video has no saved backend configuration. "
                   "Use Generate to choose its backend and settings."));
            return;
        }
        recipe.generatorId = generators.constBegin().key();
        recipe.settings = m_generationSettings->settingsFor(recipe.generatorId);
        recipe.prompt = m_currentPreviewVideoPrompt;
        if (sourceDir.exists(QStringLiteral("generation_source.png")))
        {
            recipe.imagePaths << sourceDir.absoluteFilePath(QStringLiteral("generation_source.png"));
        }
        else
        {
            const auto images = sourceDir.entryList({QStringLiteral("source.*")}, QDir::Files);
            if (!images.isEmpty())
            {
                recipe.imagePaths << sourceDir.absoluteFilePath(images.first());
            }
        }
        const auto secondary = sourceDir.entryList({QStringLiteral("source2.*")}, QDir::Files);
        if (!secondary.isEmpty())
        {
            recipe.imagePaths << sourceDir.absoluteFilePath(secondary.first());
        }
        if ((recipe.imagePaths.isEmpty() && !m_projects->absoluteImagePath(row).isEmpty())
            || (secondary.isEmpty() && !m_projects->absoluteImagePath2(row).isEmpty()))
        {
            QMessageBox::warning(this, tr("Cannot generate again"),
                tr("This older video's source images are missing. Use Generate "
                   "to select the images again."));
            return;
        }
        note = tr("Older video: the saved prompt and recovered source images will be used "
                  "with the current backend settings. Its original settings were not saved.");
    }
    AbstractVideoGenerator *generator = generators.value(recipe.generatorId);
    if (!generator)
    {
        QMessageBox::warning(this, tr("Cannot generate again"),
            tr("The saved backend is not available: %1").arg(recipe.generatorId));
        return;
    }
    DialogGenerateAgain dialog{tr("%1 — %2\n%3 source image(s).\n%4")
        .arg(shortCode, generator->getName()).arg(recipe.imagePaths.size()).arg(note), this};
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    _openProgress();
    m_projects->resetStagingDir(row);
    const QDir staging = m_projects->stagingDir(row);
    m_runGenerations.clear();
    m_runImagePath.clear();
    m_runImagePath2.clear();
    m_runVideoFormatLabel.clear(); // the saved prompt already contains the format
    GenerationJob job{GenerationJob::Video, recipe.prompt, recipe.generatorId,
                      nullptr, propertyIds};
    job.repeatUnchanged = true;
    job.settings = recipe.settings;
    for (int i = 0; i < recipe.imagePaths.size(); ++i)
    {
        const QString name = QStringLiteral("repeat_source_%1.%2")
            .arg(i + 1).arg(QFileInfo(recipe.imagePaths[i]).suffix());
        if (!QFile::copy(recipe.imagePaths[i], staging.absoluteFilePath(name)))
        {
            _finishProgress(tr("Could not copy a source image: %1").arg(recipe.imagePaths[i]));
            return;
        }
        job.sourceImageFiles << name;
    }
    // Keep hook suggestions available on the new takes as well.
    for (const QString &name : {QStringLiteral("hooks.json"), QStringLiteral("suggestions.json")})
    {
        if (sourceDir.exists(name))
        {
            QFile::copy(sourceDir.absoluteFilePath(name), staging.absoluteFilePath(name));
        }
    }
    QList<GenerationJob> jobs;
    for (int take = 0; take < dialog.generationCount(); ++take)
    {
        jobs << job;
    }
    _logProgress(tr("Generating %1 additional video(s) from %2.")
        .arg(jobs.size()).arg(shortCode));
    _queueJobs(row, projectId, jobs);
}

void PaneGeneration::_togglePreviewPlayback()
{
    if (m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
    {
        m_mediaPlayer->pause();
    }
    else
    {
        m_mediaPlayer->play();
    }
}

void PaneGeneration::_viewSourceImage()
{
    const int row = ui->tableViewProjects->currentIndex().row();
    if (row < 0)
    {
        QMessageBox::information(this, tr("No project selected"),
            tr("Select a project in the table first."));
        return;
    }
    const QString imagePath = m_projects->absoluteImagePath(row);
    const QPixmap pixmap{imagePath};
    if (imagePath.isEmpty() || pixmap.isNull())
    {
        QMessageBox::information(this, tr("No source image"),
            tr("This project has no source image."));
        return;
    }
    const QString imagePath2 = m_projects->absoluteImagePath2(row);
    const QPixmap pixmap2{imagePath2};
    const bool hasSecond = !imagePath2.isEmpty() && !pixmap2.isNull();
    // The image step's output (RegenerateInput/BootstrapImage, including the
    // white-background variant) from the most recent generation that has
    // one — "" when the project's source was always used as-is (KeepInput).
    const QString regeneratedPath = _latestGeneratedImage(row);
    const QPixmap regeneratedPixmap{regeneratedPath};
    const bool hasRegenerated = !regeneratedPath.isEmpty() && !regeneratedPixmap.isNull();
    const QString regeneratedPath2 = _latestGeneratedImage2(row);
    const QPixmap regeneratedPixmap2{regeneratedPath2};
    const bool hasRegenerated2 = !regeneratedPath2.isEmpty() && !regeneratedPixmap2.isNull();

    const int columnCount = 1 + (hasRegenerated ? 1 : 0) + (hasSecond ? 1 : 0) + (hasRegenerated2 ? 1 : 0);
    const QSize maxEach = columnCount >= 4 ? QSize(350, 900)
        : columnCount == 3 ? QSize(430, 900)
        : columnCount == 2 ? QSize(650, 900) : QSize(900, 900);

    // One titled column: a caption (source file name / "Regenerated") above
    // its image, so the left-vs-right meaning is unambiguous at a glance.
    const auto addColumn = [&](QBoxLayout *layout, const QString &caption,
                               const QPixmap &columnPixmap) {
        auto *columnLayout = new QVBoxLayout{};
        auto *captionLabel = new QLabel{caption};
        captionLabel->setAlignment(Qt::AlignCenter);
        columnLayout->addWidget(captionLabel);
        auto *imageLabel = new QLabel{};
        imageLabel->setAlignment(Qt::AlignCenter);
        const QSize size = columnPixmap.size().boundedTo(maxEach);
        imageLabel->setPixmap(columnPixmap.scaled(size, Qt::KeepAspectRatio,
                                                  Qt::SmoothTransformation));
        columnLayout->addWidget(imageLabel);
        layout->addLayout(columnLayout);
        return size;
    };

    auto *dialog = new QDialog{this};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle((hasRegenerated || hasRegenerated2)
        ? tr("Source vs. regenerated image — %1").arg(QFileInfo{imagePath}.fileName())
        : tr("Source image(s) — %1").arg(QFileInfo{imagePath}.fileName()));
    auto *layout = new QHBoxLayout{dialog};

    QSize totalSize = addColumn(layout, tr("Source — %1").arg(QFileInfo{imagePath}.fileName()),
                                pixmap);
    if (hasRegenerated)
    {
        const QSize sizeRegen = addColumn(layout,
            hasRegenerated2 ? tr("Regenerated 1") : tr("Regenerated"), regeneratedPixmap);
        totalSize = QSize(totalSize.width() + sizeRegen.width(),
                          std::max(totalSize.height(), sizeRegen.height()));
    }
    if (hasSecond)
    {
        const QSize size2 = addColumn(layout,
            tr("Source 2 — %1").arg(QFileInfo{imagePath2}.fileName()), pixmap2);
        totalSize = QSize(totalSize.width() + size2.width(),
                          std::max(totalSize.height(), size2.height()));
    }
    if (hasRegenerated2)
    {
        const QSize sizeRegen2 = addColumn(layout, tr("Regenerated 2"), regeneratedPixmap2);
        totalSize = QSize(totalSize.width() + sizeRegen2.width(),
                          std::max(totalSize.height(), sizeRegen2.height()));
    }
    if (!hasRegenerated && !hasRegenerated2)
    {
        auto *notRegeneratedLabel = new QLabel{
            tr("Not regenerated yet\n(image mode: keep input as-is,\nor no "
               "generation has run)")};
        notRegeneratedLabel->setAlignment(Qt::AlignCenter);
        notRegeneratedLabel->setWordWrap(true);
        layout->addWidget(notRegeneratedLabel, 1);
    }
    dialog->resize(totalSize + QSize(60, 60));
    dialog->show();
}

void PaneGeneration::_deleteGenerated()
{
    const int row = ui->tableViewProjects->currentIndex().row();
    if (row < 0)
    {
        QMessageBox::information(this, tr("No project selected"),
            tr("Select a project in the table first."));
        return;
    }

    // Scoped to whichever ONE generation is selected in the Generations
    // list — not every generation of the project. A property-value child
    // row may be selected instead of its generation's own top-level row;
    // resolve up to that.
    QTreeWidgetItem *item = ui->treeViewGenerations->currentItem();
    QTreeWidgetItem *topItem = item && item->parent() ? item->parent() : item;
    const TableVideos::VideoRecord *record = topItem
        ? m_videos->recordFromId(topItem->data(0, Qt::UserRole).toUuid()) : nullptr;
    if (!record)
    {
        QMessageBox::information(this, tr("No generation selected"),
            tr("Select a generation in the Generations list first."));
        return;
    }

    // A generation is protected once it has retrieved statistics (published
    // AND actually measured) — real training data TableVideos otherwise
    // never lets go of.
    if (m_videos->hasRetrievedStatistics(record->id))
    {
        QMessageBox::information(this, tr("Cannot delete"),
            tr("%1 has retrieved statistics (published and measured) and is "
               "kept — deleting it would lose real training data.")
                .arg(record->shortCode));
        return;
    }

    if (QMessageBox::question(this, tr("Delete this generation?"),
            tr("Delete %1 and its files (folder, prompts, hooks)? This "
               "cannot be undone.").arg(record->shortCode))
        != QMessageBox::Yes)
    {
        return;
    }

    const QString shortCode = record->shortCode;
    const QUuid videoId = record->id;
    const QDir genDir = m_projects->generationDir(row, shortCode);
    if (genDir.exists())
    {
        QDir{genDir.absolutePath()}.removeRecursively();
    }
    // removeVideo() is the one way a record can be deleted at all — it
    // re-checks hasRetrievedStatistics() itself, so this stays safe even if
    // something changed between the check above and here.
    m_videos->removeVideo(videoId);

    m_mediaPlayer->stop();
    m_previewImageFiles.clear();
    m_buttonPrevImage->hide();
    m_buttonNextImage->hide();
    m_labelImageNav->hide();
    m_stackedPreview->setCurrentWidget(m_labelPreviewEmpty);
    m_labelGenerationHook->clear();
    m_labelGenerationDescription->clear();
    m_currentPreviewHook.clear();
    m_buttonFavoriteHook->setEnabled(false);
    m_currentPreviewVideoPrompt.clear();
    m_buttonFavoriteVideoPrompt->setEnabled(false);
    ui->buttonGenerateAgain->setEnabled(false);

    _refreshGenerationsView(row);
    _logProgress(tr("Deleted generation %1.").arg(shortCode));
}

void PaneGeneration::_propertiesContextMenu(const QPoint &position)
{
    const QModelIndex index = ui->treeViewProperties->indexAt(position);
    QMenu menu{this};
    QAction *actionBootstrap = menu.addAction(
        tr("Bootstrap from reference images..."));
    menu.addSeparator();
    QAction *actionAddProperty = menu.addAction(tr("Add property..."));
    QAction *actionAddValue = index.isValid() ? menu.addAction(tr("Add value..."))
                                              : nullptr;
    QAction *actionArchive = index.isValid() ? menu.addAction(tr("Archive"))
                                             : nullptr;
    QAction *actionDelete = index.isValid() ? menu.addAction(tr("Delete"))
                                            : nullptr;
    QAction *chosen = menu.exec(
        ui->treeViewProperties->viewport()->mapToGlobal(position));
    if (!chosen)
    {
        return;
    }

    if (chosen == actionBootstrap)
    {
        _bootstrapPropertiesFromImages();
    }
    else if (chosen == actionAddProperty)
    {
        const QString name = QInputDialog::getText(this, tr("Add property"),
            tr("Property name (e.g. \"Background\"):"));
        if (!name.trimmed().isEmpty())
        {
            ui->treeViewProperties->setCurrentIndex(
                m_properties->addProperty(name.trimmed()));
        }
    }
    else if (chosen == actionAddValue)
    {
        QModelIndex property = index.siblingAtColumn(0);
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
        ui->treeViewProperties->expand(property);
        ui->treeViewProperties->setCurrentIndex(value);
    }
    else if (chosen == actionArchive)
    {
        m_properties->archiveTo(*m_archive, index.siblingAtColumn(0));
    }
    else if (chosen == actionDelete)
    {
        if (QMessageBox::question(this, tr("Delete?"),
                tr("Delete \"%1\" permanently? (Archive keeps its statistics "
                   "history instead.)")
                    .arg(index.siblingAtColumn(0).data().toString()))
            == QMessageBox::Yes)
        {
            m_properties->removeNode(index.siblingAtColumn(0));
        }
    }
}

void PaneGeneration::_bootstrapPropertiesFromImages()
{
    AbstractCli *cli = _promptCli();
    if (!cli)
    {
        QMessageBox::information(this, tr("No CLI available"),
            tr("Choose a CLI at the top of the Generation page first."));
        return;
    }
    const QStringList imagePaths = QFileDialog::getOpenFileNames(this,
        tr("Select reference images"), QString{},
        tr("Images (*.png *.jpg *.jpeg *.webp)"));
    if (imagePaths.isEmpty())
    {
        return;
    }

    // A throwaway analysis workspace — never touches any project's staging,
    // since this step is meant to run before any project even has one.
    QDir bootstrapDir{WorkingDirectoryManager::instance()->workingDir()
        .absoluteFilePath(QStringLiteral("_property_bootstrap"))};
    bootstrapDir.removeRecursively();
    QDir{}.mkpath(bootstrapDir.absolutePath());

    QStringList fileNames;
    for (const QString &path : imagePaths)
    {
        const QString fileName = QFileInfo{path}.fileName();
        if (QFile::copy(path, bootstrapDir.absoluteFilePath(fileName)))
        {
            fileNames << fileName;
        }
    }
    if (fileNames.isEmpty())
    {
        QMessageBox::warning(this, tr("Nothing to analyze"),
            tr("None of the selected images could be copied for analysis."));
        bootstrapDir.removeRecursively();
        return;
    }

    _runPropertyBootstrap(fileNames, bootstrapDir);
}

void PaneGeneration::_runPropertyBootstrap(const QStringList &fileNames,
                                           const QDir &bootstrapDir, int attempt)
{
    constexpr int MAX_BOOTSTRAP_TRIES = 3;
    AbstractCli *cli = _promptCli();
    if (!cli)
    {
        QDir{bootstrapDir}.removeRecursively();
        return;
    }

    // Existing catalog, shown so the CLI adds NEW values/properties instead
    // of duplicating what is already there.
    QStringList existing;
    for (TreeProperties::PropertyNode *property : m_properties->topLevelNodes())
    {
        QStringList values;
        for (TreeProperties::PropertyNode *value : property->children)
        {
            values << value->name;
        }
        existing << QStringLiteral("%1 (%2)").arg(property->name, values.join(QStringLiteral(", ")));
    }

    QString metaPrompt = QStringLiteral(
        "Analyze these %1 reference image(s), available in the current working "
        "directory: %2.\n"
        "For each image, note its background/setting, mood/lighting, camera "
        "angle, outfit/styling, and any other visually distinctive dimension.\n"
        "Then propose an A/B-test property catalog for short social-media videos "
        "that captures the REAL visual diversity actually seen ACROSS these "
        "images — every value must be grounded in something an image genuinely "
        "shows, never invented. Group into 2-5 properties (e.g. background, "
        "mood, camera_angle, outfit), each with 2-5 distinct values seen in "
        "the set.\n").arg(fileNames.size()).arg(fileNames.join(QStringLiteral(", ")));
    if (!existing.isEmpty())
    {
        metaPrompt += QStringLiteral(
            "The catalog already has: %1. Add NEW values to these where an "
            "image shows something not already covered, and/or propose new "
            "properties — never duplicate an existing property or value "
            "name.\n").arg(existing.join(QStringLiteral("; ")));
    }
    metaPrompt += QStringLiteral(
        "Using your file tools, write ONE JSON object to the file "
        "'bootstrap.json' in the current working directory. Valid JSON only — "
        "no markdown fences, no commentary — exactly this shape:\n"
        "{\"properties\": [{\"property\": \"...\", \"values\": [{\"name\": "
        "\"...\", \"fragment\": \"<text to inject in a generation prompt, e.g. "
        "'on a rooftop terrace at golden-hour sunset'>\"}, ...]}]}\n"
        "After writing the file, reply with the single word: done. If you "
        "cannot write files, reply with the JSON object itself instead.");
    if (attempt > 1)
    {
        metaPrompt += QStringLiteral(
            "\nIMPORTANT: your previous reply was invalid or truncated JSON. "
            "Write COMPLETE, valid JSON — shorten the texts if needed.");
    }

    const QString bootstrapFile = bootstrapDir.absoluteFilePath(
        QStringLiteral("bootstrap.json"));
    QFile::remove(bootstrapFile); // a stale file must never be mistaken for the reply

    _logProgress(attempt == 1
        ? tr("Analyzing %1 reference image(s) with %2 to bootstrap the "
             "property catalog...").arg(fileNames.size()).arg(cli->getName())
        : tr("Asking %1 again (attempt %2/%3)...")
            .arg(cli->getName()).arg(attempt).arg(MAX_BOOTSTRAP_TRIES));
    if (!m_progressDlg)
    {
        _openProgress();
    }
    setEnabled(false);
    cli->runPromptAsync(metaPrompt, bootstrapDir.absolutePath(), this,
        [this, cli, fileNames, bootstrapDir, bootstrapFile, attempt,
         MAX_BOOTSTRAP_TRIES](CliRunResult result) {
        setEnabled(true);
        if (!result.processStarted || result.exitCode != 0
            || result.output.trimmed().isEmpty())
        {
            if (cli->classifyError(result.errorOutput) != CliErrorKind::Other
                || attempt >= MAX_BOOTSTRAP_TRIES)
            {
                _finishProgress(tr("Bootstrap failed: %1")
                    .arg(friendlyCliError(cli, result)));
                QDir{bootstrapDir}.removeRecursively();
                return;
            }
            _logProgress(tr("Bootstrap run failed (%1) — starting over.")
                .arg(friendlyCliError(cli, result).left(200)));
            _runPropertyBootstrap(fileNames, bootstrapDir, attempt + 1);
            return;
        }

        QJsonObject object;
        QFile replyFile{bootstrapFile};
        if (replyFile.open(QFile::ReadOnly))
        {
            object = QJsonDocument::fromJson(replyFile.readAll()).object();
        }
        if (object.isEmpty())
        {
            const QString raw = result.output;
            const qsizetype first = raw.indexOf(QLatin1Char('{'));
            const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
            if (first >= 0 && last > first)
            {
                object = QJsonDocument::fromJson(
                    raw.mid(first, last - first + 1).toUtf8()).object();
            }
        }
        const QJsonArray properties = object.value(QStringLiteral("properties")).toArray();
        if (properties.isEmpty())
        {
            if (attempt < MAX_BOOTSTRAP_TRIES)
            {
                _logProgress(tr("The reply was not valid JSON (likely "
                    "truncated) — asking again."));
                _runPropertyBootstrap(fileNames, bootstrapDir, attempt + 1);
                return;
            }
            _finishProgress(tr("Bootstrap failed after %1 attempts — the CLI "
                "did not reply with the expected JSON: %2")
                .arg(MAX_BOOTSTRAP_TRIES).arg(result.output.left(300)));
            QDir{bootstrapDir}.removeRecursively();
            return;
        }

        int propertiesAdded = 0;
        int valuesAdded = 0;
        for (const QJsonValue &propertyValue : properties)
        {
            const QJsonObject propertyObj = propertyValue.toObject();
            const QString propertyName
                = propertyObj.value(QStringLiteral("property")).toString().trimmed();
            if (propertyName.isEmpty())
            {
                continue;
            }
            QModelIndex propertyIndex;
            for (TreeProperties::PropertyNode *node : m_properties->topLevelNodes())
            {
                if (node->name.compare(propertyName, Qt::CaseInsensitive) == 0)
                {
                    propertyIndex = m_properties->indexFromNode(node);
                    break;
                }
            }
            if (!propertyIndex.isValid())
            {
                propertyIndex = m_properties->addProperty(propertyName,
                    QStringLiteral("image-bootstrap"));
                ++propertiesAdded;
            }
            TreeProperties::PropertyNode *propertyNode = m_properties->nodeFromId(
                propertyIndex.data(TreeProperties::RoleId).toUuid());
            if (!propertyNode)
            {
                continue;
            }
            for (const QJsonValue &valueEntry
                 : propertyObj.value(QStringLiteral("values")).toArray())
            {
                const QJsonObject valueObj = valueEntry.toObject();
                const QString valueName
                    = valueObj.value(QStringLiteral("name")).toString().trimmed();
                if (valueName.isEmpty())
                {
                    continue;
                }
                bool alreadyExists = false;
                for (TreeProperties::PropertyNode *child : propertyNode->children)
                {
                    if (child->name.compare(valueName, Qt::CaseInsensitive) == 0)
                    {
                        alreadyExists = true;
                        break;
                    }
                }
                if (alreadyExists)
                {
                    continue;
                }
                const QString fragment
                    = valueObj.value(QStringLiteral("fragment")).toString().trimmed();
                m_properties->addValue(propertyIndex, valueName, fragment,
                    QStringLiteral("image-bootstrap"));
                ++valuesAdded;
            }
        }
        ui->treeViewProperties->expandAll();
        QDir{bootstrapDir}.removeRecursively();
        _finishProgress(tr("Bootstrap complete: %1 new propertie(s), %2 new "
            "value(s) added from %3 reference image(s).")
            .arg(propertiesAdded).arg(valuesAdded).arg(fileNames.size()));
    });
}

void PaneGeneration::_openHooksDialog()
{
    const int row = ui->tableViewProjects->currentIndex().row();
    if (row < 0)
    {
        QMessageBox::information(this, tr("No project selected"),
            tr("Select a project in the table first."));
        return;
    }

    // Prefer the generation selected in the tree; else the project's latest.
    QString shortCode;
    if (QTreeWidgetItem *item = ui->treeViewGenerations->currentItem())
    {
        QTreeWidgetItem *topItem = item->parent() ? item->parent() : item;
        if (const auto *record
            = m_videos->recordFromId(topItem->data(0, Qt::UserRole).toUuid()))
        {
            shortCode = record->shortCode;
        }
    }
    if (shortCode.isEmpty())
    {
        const auto records = m_videos->recordsForProject(m_projects->projectId(row));
        if (!records.isEmpty())
        {
            shortCode = records.first()->shortCode;
        }
    }

    const QDir genDir = !shortCode.isEmpty()
        ? m_projects->generationDir(row, shortCode)
        : m_projects->projectGenerationsDir(row);
    const QDir tempDir = !shortCode.isEmpty()
        ? m_projects->generationTempDir(row, shortCode)
        : m_projects->stagingDir(row);
    auto hooks = loadHooksFile(tempDir, nullptr);
    if (hooks.isEmpty() && !shortCode.isEmpty())
    {
        hooks = loadHooksFile(m_projects->stagingDir(row), nullptr);
    }

    QString existingHook, existingDescription;
    readHookDescriptionFile(genDir, &existingHook, &existingDescription);

    DialogHooks::Context context;
    context.keyword = m_projects->data(
        m_projects->index(row, TableProjects::IND_KEYWORD)).toString().trimmed();
    context.hookIdea = m_projects->data(
        m_projects->index(row, TableProjects::IND_HOOK)).toString().trimmed();
    context.videoFormatLabel = m_runVideoFormatLabel.isEmpty()
        ? QStringLiteral("9:16 vertical") : m_runVideoFormatLabel;
    context.preferredHashtags = m_hashtags ? m_hashtags->hashtags() : QStringList{};
    context.favoriteHooksPrompt = m_favoriteHooks ? m_favoriteHooks->asPromptSection() : QString{};
    context.codeTag = codeTagFromShortCode(shortCode);
    context.workingDir = tempDir.absolutePath();
    context.availableClis = m_availableClis;
    context.selectedCli = _promptCli();

    if (!shortCode.isEmpty())
    {
        QFile promptFile{tempDir.filePath(QStringLiteral("generation_prompt.txt"))};
        if (!promptFile.exists())
        {
            promptFile.setFileName(genDir.filePath(QStringLiteral("generation_prompt.txt")));
        }
        if (promptFile.open(QFile::ReadOnly))
        {
            context.videoPrompt = QString::fromUtf8(promptFile.readAll()).trimmed();
        }
    }

    DialogHooks dialog{hooks, context, this};
    dialog.preselect(existingHook, existingDescription);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    if (!dialog.selectedHook().isEmpty() || !dialog.selectedDescription().isEmpty())
    {
        _saveChosenHookDescription(row, {qMakePair(genDir, shortCode)}, dialog.selectedHook(),
                                   dialog.selectedDescription());
    }
    if (!shortCode.isEmpty())
    {
        saveHooksFile(tempDir, dialog.allHooks(), shortCode);
        _refreshHooksView(tempDir);
    }
    else
    {
        saveHooksFile(m_projects->stagingDir(row), dialog.allHooks(), QString{});
        _refreshHooksView(m_projects->stagingDir(row));
    }
}

void PaneGeneration::_openProgress()
{
    m_batchCancelled = false;
    if (m_progressDlg)
    {
        m_progressDlg->close(); // previous run's dialog (WA_DeleteOnClose)
    }
    // Parented to the main window, not the pane: the pane is disabled while
    // a CLI runs, which would disable a child dialog's Copy/Cancel buttons.
    m_progressDlg = makeProgressDlg(window(), tr("Generation"), &m_progress);
    if (m_progress.cancelBtn)
    {
        connect(m_progress.cancelBtn, &QPushButton::clicked, m_progressDlg, [this]() {
            m_batchCancelled = true;
            _logProgress(tr("Cancelling remaining generations after the current steps finish..."));
        });
    }
    m_progressDlg->show();
}

void PaneGeneration::_logProgress(const QString &message)
{
    if (m_progress.log)
    {
        m_progress.log->append(
            QTime::currentTime().toString(QStringLiteral("hh:mm:ss  ")) + message);
    }
    if (m_progress.status)
    {
        m_progress.status->setText(message);
    }
}

void PaneGeneration::_finishProgress(const QString &message)
{
    _logProgress(message);
    if (m_progress.bar)
    {
        m_progress.bar->setRange(0, 1);
        m_progress.bar->setValue(1);
    }
    if (m_progress.closeBtn)
    {
        m_progress.closeBtn->setEnabled(true);
    }
    if (m_progress.cancelBtn)
    {
        m_progress.cancelBtn->hide();
    }
}
