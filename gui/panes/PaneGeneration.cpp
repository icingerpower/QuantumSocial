// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code
// (this file hosts _runImageGenerationJob, a coroutine).
#pragma GCC optimize("O1")

#include "PaneGeneration.h"
#include "model/research/InspirationAdaptationMethod.h"
#include "model/research/ResearchProject.h"
#include "model/videos/SocialStatistics.h"
#include "ui_PaneGeneration.h"

#include <algorithm>
#include <chrono>

#include <QCoro/QCoroTimer>
#include <QAudioOutput>
#include <QClipboard>
#include <QColor>
#include <QDataWidgetMapper>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
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
#include "../DialogMultiVideoGeneration.h"
#include "../DialogNewProject.h"
#include "../DialogPickIntroVideo.h"
#include "../DialogReviewNewProperties.h"
#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/accounts/AbstractSocialAccount.h"
#include "model/PromptLessons.h"
#include "model/videogen/ProductShowcaseMultiVideoWorkflow.h"
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

// "QS-0001" -> "qs0001": the tag appended to descriptions (without '#' so platforms
// like TikTok with a 5-hashtag limit don't count it as a hashtag); tolerant lookup
// (TableVideos::recordFromShortCode) maps it back to the property recipe.
QString codeTagFromShortCode(const QString &shortCode)
{
    if (shortCode.isEmpty())
    {
        return QString{};
    }
    QString digits = shortCode;
    digits.remove(QStringLiteral("QS-"));
    return QStringLiteral("qs") + digits.toLower();
}

void saveHooksFile(const QDir &dir, const QList<QPair<QString, QString>> &hooks,
                   const QString &shortCode, const QString &videoIdea = QString{})
{
    QJsonArray array;
    for (const auto &hook : hooks)
    {
        array << QJsonObject{{QStringLiteral("hook"), hook.first},
                             {QStringLiteral("description"), hook.second}};
    }
    QJsonObject root{
        {QStringLiteral("code"), shortCode},
        {QStringLiteral("hooks"), array},
    };
    if (!videoIdea.isEmpty())
    {
        root[QStringLiteral("videoIdea")] = videoIdea;
    }
    QFile file{dir.absoluteFilePath(HOOKS_FILE_NAME)};
    if (!file.open(QFile::WriteOnly))
    {
        return;
    }
    file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
}

QList<QPair<QString, QString>> loadHooksFile(const QDir &dir, QString *shortCode = nullptr,
                                             QString *videoIdea = nullptr)
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
    if (videoIdea)
    {
        *videoIdea = root.value(QStringLiteral("videoIdea")).toString();
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
                        const QString &outputFileName = GENERATED_IMAGE_NAME,
                        const QString &referenceSceneFile = QString{},
                        const QString &productSummary = QString{},
                        const QString &adaptationNotes = QString{},
                        const QString &swapInstruction = QString{})
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
    if (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct
        && !referenceSceneFile.isEmpty())
    {
        QString prompt = marker + QStringLiteral(
            "You are given TWO image files in the current working directory:\n"
            "1. '%1' — the REFERENCE SCENE (captured from a social-media reference video/pin). "
            "Use this ONLY for the scene composition, environment/background, lighting, camera "
            "angle, framing, model pose, and outfit styling.\n"
            "2. '%2' — OUR REAL PRODUCT%3. This is the EXACT product that must appear in the "
            "generated image.\n\n"
            "Generate a NEW high-quality starting frame for a %4 social-media video by "
            "REPLACING the featured shoes/product in '%1' with OUR REAL PRODUCT from '%2':\n"
            "- Scene & composition: preserve the setting, camera angle, framing, lighting, mood, "
            "model pose, and outfit styling from '%1', adapted cleanly to a full %4 canvas "
            "(no letterboxing, pillarboxing, or blank bars).\n"
            "- Product replacement (NON-NEGOTIABLE): completely remove the original shoes/product "
            "shown in '%1' and replace them with the EXACT product from '%2' — matching its exact "
            "silhouette, heel shape and height, straps/closures, print/pattern, color, material, "
            "and hardware details. Do NOT blend features of the reference product with our product.\n"
            "- Clean starting frame for video animation: remove all social network icons, UI "
            "overlays, watermarks, captions, logos, and text.\n")
            .arg(referenceSceneFile,
                 imageRef,
                 productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(productSummary),
                 videoFormatLabel);
        if (!adaptationNotes.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Adaptation notes: %1\n").arg(adaptationNotes.trimmed());
        }
        if (!swapInstruction.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Specific user adjustment: %1\n").arg(swapInstruction.trimmed());
        }
        prompt += QStringLiteral(
            "Save the result in the current working directory as exactly '%1'.\n"
            "Do not modify the input files. Reply with just the output file name.")
            .arg(outputFileName);
        return prompt;
    }
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

QString referenceVariantStepPrompt(const QString &imageRef,
                                   const QString &videoFormatLabel,
                                   const QString &outputFileName,
                                   const QString &referenceSceneFile,
                                   const QString &productSummary,
                                   const QString &adaptationNotes,
                                   const QString &variantDirection,
                                   const QString &swapInstruction = QString{})
{
    const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
    if (!referenceSceneFile.isEmpty())
    {
        QString prompt = marker + QStringLiteral(
            "You are given TWO image files in the current working directory:\n"
            "1. '%1' — the REFERENCE SCENE (captured from a winning social-media video/pin).\n"
            "2. '%2' — OUR REAL PRODUCT%3. This is the EXACT product that must appear in the generated image.\n\n"
            "Generate a NEW high-quality starting frame for a %4 social-media video by keeping the core "
            "winning visual hook of '%1' while introducing a tasteful sibling variation:\n"
            "- Keep from '%1': the flattering camera angle, body/leg framing, pose energy, and prominent "
            "visual focus on the featured product.\n"
            "- Controlled variation (change 'a little more' than just the product): introduce a fresh "
            "complementary backdrop/setting, lighting mood, or outfit color palette so this feels like a "
            "distinct sibling creative rather than a duplicate of '%1'%5.\n"
            "- Product accuracy (NON-NEGOTIABLE): completely replace the reference shoes/product with "
            "OUR EXACT REAL PRODUCT from '%2' — matching its exact silhouette, heel shape/height, straps, "
            "print/pattern, color, material, and hardware.\n"
            "- Clean starting frame for video animation: full %4 canvas, zero social UI icons, watermarks, "
            "logos, or overlaid text.\n")
            .arg(referenceSceneFile,
                 imageRef,
                 productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(productSummary),
                 videoFormatLabel,
                 variantDirection.trimmed().isEmpty()
                     ? QString{}
                     : QStringLiteral(" (%1)").arg(variantDirection.trimmed()));
        if (!adaptationNotes.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Adaptation notes: %1\n").arg(adaptationNotes.trimmed());
        }
        if (!swapInstruction.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Refinement adjustment: %1\n").arg(swapInstruction.trimmed());
        }
        prompt += QStringLiteral(
            "Save the result in the current working directory as exactly '%1'.\n"
            "Do not modify the input files. Reply with just the output file name.")
            .arg(outputFileName);
        return prompt;
    }

    QString prompt = marker + QStringLiteral(
        "You are given '%1' — OUR REAL PRODUCT%2 — in the current working directory.\n"
        "Generate a scroll-stopping %3 fashion-editorial starting frame for a social-media video "
        "showcasing our exact product in a realistic lifestyle scene%4:\n"
        "- Product accuracy (NON-NEGOTIABLE): match '%1' exactly (silhouette, heel shape/height, "
        "straps, print/pattern, color, material).\n"
        "- Clean starting frame: full %3 canvas, zero watermarks, social icons, or text.\n")
        .arg(imageRef,
             productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(productSummary),
             videoFormatLabel,
             variantDirection.trimmed().isEmpty()
                 ? QString{}
                 : QStringLiteral(" (%1)").arg(variantDirection.trimmed()));
    if (!swapInstruction.trimmed().isEmpty())
    {
        prompt += QStringLiteral("- Refinement adjustment: %1\n").arg(swapInstruction.trimmed());
    }
    prompt += QStringLiteral(
        "Save the result in the current working directory as exactly '%1'.\n"
        "Do not modify the original file. Reply with just the output file name.")
        .arg(outputFileName);
    return prompt;
}

QString seoKeywordForAutoProduct(const QJsonObject &product)
{
    const QString directCat = product.value(QStringLiteral("category")).toString().trimmed();
    if (!directCat.isEmpty())
    {
        return directCat.toLower();
    }
    const QString title = product.value(QStringLiteral("title")).toString();
    static const QStringList kFootwearAndFashionTerms{
        QStringLiteral("slingback"), QStringLiteral("stiletto"), QStringLiteral("pumps"),
        QStringLiteral("heels"), QStringLiteral("sandals"), QStringLiteral("mules"),
        QStringLiteral("boots"), QStringLiteral("platform"), QStringLiteral("ankle strap"),
        QStringLiteral("pointed toe"), QStringLiteral("loafers"), QStringLiteral("sneakers")
    };
    QStringList matched;
    for (const QString &term : kFootwearAndFashionTerms)
    {
        if (title.contains(term, Qt::CaseInsensitive) && matched.size() < 2)
        {
            matched << term;
        }
    }
    if (!matched.isEmpty())
    {
        return matched.join(QLatin1Char(' '));
    }
    const QJsonArray cats = product.value(QStringLiteral("categories")).toArray();
    if (!cats.isEmpty() && !cats.first().toString().trimmed().isEmpty())
    {
        return cats.first().toString().trimmed().toLower();
    }
    return QString{};
}

QString buildMeasuredWinnersPromptSection(const TableProjects *projects, const TableVideos *videos)
{
    struct MeasuredEntry
    {
        qint64 views = 0;
        qint64 likes = 0;
        qint64 shares = 0;
        QString caption;
        QString extraMeta;
    };
    QList<MeasuredEntry> entries;
    QSet<QString> seenCaptions;

    const QDir wd = WorkingDirectoryManager::instance()->workingDir();
    QFile topFile{wd.filePath(QStringLiteral("account_top_posts.json"))};
    if (topFile.open(QIODevice::ReadOnly))
    {
        const QJsonArray arr = QJsonDocument::fromJson(topFile.readAll())
            .object().value(QStringLiteral("posts")).toArray();
        for (const QJsonValue &v : arr)
        {
            const QJsonObject o = v.toObject();
            const qint64 views = o.value(QStringLiteral("views")).toInteger(-1);
            const QString cap = o.value(QStringLiteral("caption")).toString().simplified();
            if (views < 0 || cap.isEmpty() || seenCaptions.contains(cap.toLower()))
            {
                continue;
            }
            seenCaptions.insert(cap.toLower());
            QString extra;
            const QString sc = o.value(QStringLiteral("shortCode")).toString();
            if (!sc.isEmpty() && projects && videos)
            {
                if (const auto *rec = videos->recordFromShortCode(sc))
                {
                    const int pRow = projects->rowOfId(rec->projectId);
                    if (pRow >= 0)
                    {
                        QFile df{projects->generationDir(pRow, rec->shortCode, false).filePath(QStringLiteral("creative_dna.json"))};
                        if (df.open(QIODevice::ReadOnly))
                        {
                            const QJsonObject dna = QJsonDocument::fromJson(df.readAll()).object();
                            const QString fmt = dna.value(QStringLiteral("format")).toString();
                            const QString snd = dna.value(QStringLiteral("tiktokSound")).toString();
                            if (!fmt.isEmpty()) extra += QStringLiteral("format=%1").arg(fmt);
                            if (!snd.isEmpty()) extra += (extra.isEmpty() ? QString{} : QStringLiteral(", ")) + QStringLiteral("sound=\"%1\"").arg(snd);
                        }
                    }
                }
            }
            entries.append(MeasuredEntry{
                views,
                o.value(QStringLiteral("likes")).toInteger(0),
                o.value(QStringLiteral("shares")).toInteger(0),
                cap,
                extra
            });
        }
    }

    if (entries.isEmpty())
    {
        return QString{};
    }
    std::sort(entries.begin(), entries.end(),
              [](const MeasuredEntry &a, const MeasuredEntry &b) { return a.views > b.views; });

    QString section = QStringLiteral(
        "\nPROVEN MEASURED PERFORMANCE ON OUR ACCOUNT (use real data to improve over time):\n"
        "Top-performing published posts (replicate their hook rhythm, engagement question style, and hashtag mix):\n");
    const int topCount = qMin<int>(5, entries.size());
    for (int i = 0; i < topCount; ++i)
    {
        const auto &e = entries[i];
        section += QStringLiteral("  - %1 views, %2 likes, %3 shares%4: \"%5\"\n")
            .arg(e.views)
            .arg(e.likes)
            .arg(e.shares)
            .arg(e.extraMeta.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(e.extraMeta),
                 e.caption.left(220));
    }
    if (entries.size() >= 4 && entries.last().views < entries.first().views / 5)
    {
        const auto &low = entries.last();
        section += QStringLiteral(
            "Underperforming post to AVOID repeating (%1 views): \"%2\" — never output bare codes or vague captions without a strong hook and niche #hashtags.\n")
            .arg(low.views)
            .arg(low.caption.left(140));
    }
    return section;
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
    ui->splitter_3->setSizes({420, 820});

    auto *buttonAutoPublish = new QPushButton{tr("Auto-publish…"), this};
    buttonAutoPublish->setObjectName(QStringLiteral("buttonAutoPublish"));
    buttonAutoPublish->setStyleSheet(QStringLiteral("font-weight: 600;"));
    buttonAutoPublish->setToolTip(
        tr("Schedule and auto-publish mature creatives (videos & carousels) to social accounts with live browser logs."));
    ui->horizontalLayout->addWidget(buttonAutoPublish);
    connect(buttonAutoPublish, &QPushButton::clicked, this, &PaneGeneration::autoPublishRequested);

    m_planProxy = new VideoPlanProxy(this);
    m_planProxy->setSourceModel(m_properties);

    m_projects = new TableProjects(
        WorkingDirectoryManager::instance()->workingDir().absolutePath(), this);
    m_projects->setVideosModel(m_videos);
    ui->tableViewProjects->setModel(m_projects);
    ui->tableViewProjects->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int column : {TableProjects::IND_CREATED, TableProjects::IND_KEYWORD,
                       TableProjects::IND_HOOK, TableProjects::IND_IMAGE,
                       TableProjects::IND_GEN_PROMPT, TableProjects::IND_GEN_HOOK,
                       TableProjects::IND_GEN_DESC, TableProjects::IND_IMAGE2,
                       TableProjects::IND_ID})
    {
        ui->tableViewProjects->setColumnHidden(column, true);
    }
    ui->tableViewProjects->verticalHeader()->setVisible(false);
    ui->tableViewProjects->horizontalHeader()->setStretchLastSection(false);
    ui->tableViewProjects->horizontalHeader()->setSectionResizeMode(
        TableProjects::IND_NAME, QHeaderView::Stretch);
    ui->tableViewProjects->horizontalHeader()->setSectionResizeMode(
        TableProjects::IND_TYPE, QHeaderView::Interactive);
    ui->tableViewProjects->horizontalHeader()->resizeSection(TableProjects::IND_TYPE, 100);
    ui->tableViewProjects->horizontalHeader()->setSectionResizeMode(
        TableProjects::IND_LAST_UPDATE, QHeaderView::Interactive);
    ui->tableViewProjects->horizontalHeader()->resizeSection(TableProjects::IND_LAST_UPDATE, 130);

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
    ui->treeViewGenerations->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->treeViewGenerations, &QWidget::customContextMenuRequested,
            this, &PaneGeneration::_generationsContextMenu);

    auto *copyShortcut = new QShortcut(QKeySequence::Copy, ui->treeViewGenerations);
    connect(copyShortcut, &QShortcut::activated,
            this, &PaneGeneration::_copySelectedGenerationPrompt);

    connect(ui->treeViewGenerations, &QTreeWidget::currentItemChanged,
            this, &PaneGeneration::_onGenerationSelected);
    connect(ui->treeViewGenerations, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) {
        if (item && item->data(0, RoleIsPromptItem).toBool())
        {
            const QString promptText = item->data(0, RolePromptText).toString();
            if (!promptText.isEmpty())
            {
                QGuiApplication::clipboard()->setText(promptText);
                _logProgress(tr("Copied prompt to clipboard."));
                return;
            }
        }
        _openHooksDialog();
    });

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
    m_buttonCopyHook = new QPushButton{tr("Copy hook"), ui->widgetPreview};
    m_buttonCopyHook->setObjectName(QStringLiteral("buttonCopyHook"));
    m_buttonCopyHook->setEnabled(false);
    m_buttonCopyHook->setToolTip(tr("Copy the suggested hook text to clipboard."));
    m_buttonCopyDescription = new QPushButton{tr("Copy caption + #tags"), ui->widgetPreview};
    m_buttonCopyDescription->setObjectName(QStringLiteral("buttonCopyDescription"));
    m_buttonCopyDescription->setEnabled(false);
    m_buttonCopyDescription->setToolTip(tr("Copy the post description, hashtags, and tracking code to clipboard."));
    ui->layoutHook->insertWidget(1, m_buttonCopyHook);
    ui->layoutHook->insertWidget(2, m_buttonCopyDescription);
    connect(m_buttonCopyHook, &QPushButton::clicked, this, [this]() {
        if (!m_currentPreviewHook.isEmpty())
        {
            QGuiApplication::clipboard()->setText(m_currentPreviewHook);
            _logProgress(tr("Copied hook to clipboard."));
        }
    });
    connect(m_buttonCopyDescription, &QPushButton::clicked, this, [this]() {
        const QString desc = m_labelGenerationDescription->text().trimmed();
        if (!desc.isEmpty())
        {
            QGuiApplication::clipboard()->setText(desc);
            _logProgress(tr("Copied caption + hashtags to clipboard."));
        }
    });
    connect(m_buttonFavoriteHook, &QPushButton::clicked,
            this, &PaneGeneration::_favoriteCurrentHook);
    connect(m_buttonFavoriteVideoPrompt, &QPushButton::clicked,
            this, &PaneGeneration::_favoriteCurrentVideoPrompt);
    connect(ui->buttonGenerateAgain, &QPushButton::clicked,
            this, &PaneGeneration::_generateAgain);
    connect(ui->lineEditExtraPrompt, &QLineEdit::returnPressed, this, [this]() {
        if (ui->buttonGenerateAgain->isEnabled())
        {
            ui->buttonGenerateAgain->click();
        }
    });

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
    connect(ui->buttonProjectAddMultiVideo, &QPushButton::clicked,
            this, &PaneGeneration::_addMultiVideoProject);
    connect(ui->buttonProjectRemove, &QPushButton::clicked,
            this, &PaneGeneration::_removeProject);
    connect(ui->tableViewProjects->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &PaneGeneration::_currentProjectChanged);
    connect(ui->buttonGenerate, &QPushButton::clicked,
            this, &PaneGeneration::_generate);
    connect(ui->buttonResumeGeneration, &QPushButton::clicked,
            this, &PaneGeneration::_resumeMultiVideoGeneration);
    connect(ui->buttonResetStep, &QPushButton::clicked,
            this, &PaneGeneration::_resetStepClicked);
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
            m_filesModel->rootPath()));
    });
    connect(ui->comboBoxCli, &QComboBox::currentIndexChanged, this, [this]() {
        if (AbstractCli *cli = _promptCli())
        {
            QSettings().setValue(SETTING_PROMPT_CLI, cli->getName());
        }
    });

    // Keep treeViewGenerations and buttonPublish in live sync whenever a creative
    // is marked published externally (e.g. via Auto-Publish or Statistics -> Retrieve).
    if (m_videos)
    {
        connect(m_videos, &QAbstractItemModel::dataChanged, this, [this]() {
            const int count = ui->treeViewGenerations->topLevelItemCount();
            for (int i = 0; i < count; ++i)
            {
                QTreeWidgetItem *topItem = ui->treeViewGenerations->topLevelItem(i);
                const QUuid vid = topItem->data(0, Qt::UserRole).toUuid();
                if (!vid.isNull())
                {
                    _applyGenerationPublishedStyle(topItem, m_videos->isPublished(vid));
                }
            }
            if (QTreeWidgetItem *cur = ui->treeViewGenerations->currentItem())
            {
                QTreeWidgetItem *topCur = cur->parent() ? cur->parent() : cur;
                const QUuid curVid = topCur->data(0, Qt::UserRole).toUuid();
                if (!curVid.isNull() && ui->buttonPublish->isEnabled())
                {
                    const bool pub = m_videos->isPublished(curVid);
                    ui->buttonPublish->setText(pub ? tr("Unpublish") : tr("Mark published"));
                    ui->buttonPublish->setToolTip(pub
                        ? tr("Unmark this video as published")
                        : tr("Mark this video as published"));
                }
            }
        });
    }

    if (m_projects->rowCount() > 0)
    {
        ui->tableViewProjects->setCurrentIndex(m_projects->index(0, TableProjects::IND_NAME));
    }
}

PaneGeneration::~PaneGeneration()
{
    AbstractVideoGenerator::stopAllWorkers();
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

bool PaneGeneration::createResearchProject(const QJsonObject &context, const QString &imagePath)
{
    QString error;
    const int row = ResearchProject::create(*m_projects, context, imagePath, &error);
    if (row < 0)
    {
        QMessageBox::warning(this, tr("Could not create project"), error);
        return false;
    }
    ui->tableViewProjects->setCurrentIndex(m_projects->index(row, TableProjects::IND_NAME));
    // Open the Generation page directly so the "Generate" button is immediately visible.
    ui->toolBox->setCurrentIndex(1);
    return true;
}

void PaneGeneration::_addMultiVideoProject()
{
    DialogMultiVideoGeneration dialog{m_availableClis, m_savedPrompts, m_properties, m_videos, this};
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const auto stepImages = dialog.stepImages();
    QString firstImagePath;
    for (const auto &stepImgs : stepImages)
    {
        if (!stepImgs.isEmpty() && !stepImgs.first().originalPath.isEmpty())
        {
            firstImagePath = stepImgs.first().originalPath;
            break;
        }
    }

    const int row = m_projects->addMultiVideoProject(dialog.keyword(), firstImagePath);
    ui->tableViewProjects->setCurrentIndex(m_projects->index(row, TableProjects::IND_NAME));

    const QUuid projectId = m_projects->projectId(row);
    const QString projPath = m_projects->projectDir(row).absolutePath();
    const QString genPath = m_projects->projectGenerationsDir(row).absolutePath();

    auto *workflow = new ProductShowcaseMultiVideoWorkflow(this);
    workflow->configure(dialog.totalSteps(),
                        dialog.introPrompt(), dialog.introTakes(),
                        dialog.afterIntroPrompt(), dialog.afterIntroTakes(),
                        stepImages);

    // Persist configuration to the project directory
    const QString manifestPath = QDir{projPath}.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    workflow->saveToFile(manifestPath);

    const QVariantMap settings = m_generationSettings->settingsFor(dialog.generatorId());
    _startMultiVideoGeneration(row, projectId, workflow);
    workflow->start(projPath, genPath, dialog.generatorId(), settings,
                    dialog.decisionCli(), dialog.imageCli(), dialog.videoFormatLabel());
}

void PaneGeneration::_resumeMultiVideoGeneration()
{
    const QModelIndex current = ui->tableViewProjects->currentIndex();
    if (!current.isValid())
    {
        return;
    }
    const int row = current.row();
    const QUuid projectId = m_projects->projectId(row);
    const QDir projDir = m_projects->projectDir(row);
    QString manifestPath = m_projects->projectGenerationsDir(row).absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    if (!QFile::exists(manifestPath))
    {
        manifestPath = projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    }

    ProductShowcaseMultiVideoWorkflow *workflow = nullptr;
    // Reuse an in-flight merge when the user reopens selection. Separate
    // workflow instances could otherwise publish older selections last.
    for (auto *candidate : findChildren<ProductShowcaseMultiVideoWorkflow *>())
    {
        if (candidate->isMerging()
            && candidate->toJson().value(QStringLiteral("projectDir")).toString() == projDir.absolutePath())
        {
            workflow = candidate;
            break;
        }
    }
    if (!workflow)
    {
        workflow = new ProductShowcaseMultiVideoWorkflow(this);
        if (!workflow->loadFromFile(manifestPath))
        {
            QMessageBox::warning(this, tr("Cannot resume"),
                                 tr("Could not load multi-video state from %1").arg(manifestPath));
            delete workflow;
            return;
        }
    }

    // If all videos are generated, allow reviewing / changing take selections
    if (workflow->completedVideoCount() == workflow->totalVideoCount())
    {
        _connectMultiVideoMerge(workflow, row);
        if (!_prepareMultiVideoGeneration(row, workflow))
        {
            workflow->deleteLater();
            return;
        }
        for (int sIdx = 0; sIdx < workflow->steps().size(); ++sIdx)
        {
            const auto &st = workflow->steps().at(sIdx);
            if (st.videoCount > 1)
            {
                QList<AbstractMultiVideoWorkflow::TakeInfo> stepTakes;
                for (const auto &t : workflow->takes())
                {
                    if (t.stepIndex == sIdx && t.completed)
                    {
                        stepTakes << t;
                    }
                }
                if (!stepTakes.isEmpty())
                {
                    if (!_promptTakeSelection(workflow, sIdx, stepTakes, row))
                    {
                        break;
                    }
                }
            }
        }
        workflow->updateMergedVideo();
        if (workflow->state() != AbstractMultiVideoWorkflow::State::Running && !workflow->isMerging())
        {
            workflow->deleteLater();
        }
        // In-flight workflows remain owned by the pane and can be reused
        // if selection is reopened before encoding finishes.
        return;
    }

    const QString genPath = m_projects->projectGenerationsDir(row).absolutePath();
    const QString genId = !workflow->toJson().value(QStringLiteral("generatorId")).toString().isEmpty()
        ? workflow->toJson().value(QStringLiteral("generatorId")).toString()
        : QStringLiteral("gemini-browser");
    const QVariantMap settings = m_generationSettings->settingsFor(genId);

    _startMultiVideoGeneration(row, projectId, workflow);
    workflow->start(projDir.absolutePath(), genPath, genId, settings,
                    _promptCli(), _imageCli(), QStringLiteral("vertical (9:16)"));
}

void PaneGeneration::_resetStepClicked()
{
    const QModelIndex current = ui->tableViewProjects->currentIndex();
    if (!current.isValid())
    {
        return;
    }
    const int row = current.row();
    if (!m_projects->isMultiVideoProject(row))
    {
        return;
    }

    const QUuid projectId = m_projects->projectId(row);
    const QDir projDir = m_projects->projectDir(row);
    QString manifestPath = m_projects->projectGenerationsDir(row).absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    if (!QFile::exists(manifestPath))
    {
        manifestPath = projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    }
    if (!QFile::exists(manifestPath))
    {
        QMessageBox::information(this, tr("No workflow"), tr("No multi-video workflow manifest was found for this project."));
        return;
    }

    ProductShowcaseMultiVideoWorkflow workflow;
    if (!workflow.loadFromFile(manifestPath))
    {
        QMessageBox::warning(this, tr("Cannot read workflow"), tr("Could not read workflow from %1").arg(manifestPath));
        return;
    }

    QMenu menu(this);
    menu.setTitle(tr("Reset / Regenerate Multi-Video Steps"));

    const auto &steps = workflow.steps();
    for (int sIdx = 0; sIdx < steps.size(); ++sIdx)
    {
        const auto &step = steps.at(sIdx);
        int completedTakes = 0;
        for (const auto &t : workflow.takes())
        {
            if (t.stepIndex == sIdx && t.completed)
            {
                completedTakes++;
            }
        }
        const QString stepLabel = step.title.isEmpty() ? tr("Step %1").arg(sIdx + 1) : step.title;
        auto *actReset = menu.addAction(tr("Reset %1 (%2/%3 takes done)...")
            .arg(stepLabel).arg(completedTakes).arg(step.videoCount));
        connect(actReset, &QAction::triggered, this, [this, row, projectId, sIdx, stepLabel]() {
            const auto reply = QMessageBox::question(
                this,
                tr("Reset Step"),
                tr("Are you sure you want to reset %1?\nGenerated video files for this step will be deleted so you can regenerate them.").arg(stepLabel),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (reply == QMessageBox::Yes)
            {
                _resetMultiVideoStep(row, projectId, nullptr, sIdx, false);
            }
        });
    }

    if (steps.size() > 1)
    {
        menu.addSeparator();
        auto *actResetAfterIntro = menu.addAction(tr("Reset from Step 2 onwards (keep Step 1 Intro)..."));
        connect(actResetAfterIntro, &QAction::triggered, this, [this, row, projectId]() {
            const auto reply = QMessageBox::question(
                this,
                tr("Reset Steps from Step 2"),
                tr("Are you sure you want to reset all steps after Step 1?\nGenerated video files for Step 2 and subsequent steps will be deleted while keeping your chosen Intro video."),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (reply == QMessageBox::Yes)
            {
                _resetMultiVideoStep(row, projectId, nullptr, 1, true);
            }
        });

        auto *actResetAll = menu.addAction(tr("Reset all steps (including Intro)..."));
        connect(actResetAll, &QAction::triggered, this, [this, row, projectId]() {
            const auto reply = QMessageBox::question(
                this,
                tr("Reset All Steps"),
                tr("Are you sure you want to reset ALL steps?\nAll generated video files for this showcase will be deleted so you can restart cleanly."),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (reply == QMessageBox::Yes)
            {
                _resetMultiVideoStep(row, projectId, nullptr, 0, true);
            }
        });
    }

    menu.addSeparator();
    auto *subMenuAddTakes = menu.addMenu(tr("Generate again (add more takes)..."));
    for (int sIdx = 0; sIdx < steps.size(); ++sIdx)
    {
        const auto &step = steps.at(sIdx);
        const QString stepLabel = step.title.isEmpty() ? tr("Step %1").arg(sIdx + 1) : step.title;
        auto *actAdd = subMenuAddTakes->addAction(tr("Add takes to %1...").arg(stepLabel));
        connect(actAdd, &QAction::triggered, this, [this, row, projectId, sIdx, stepLabel]() {
            bool ok = false;
            const int count = QInputDialog::getInt(
                this,
                tr("Add Takes"),
                tr("How many new takes to generate for %1?").arg(stepLabel),
                1, 1, 10, 1, &ok);
            if (ok && count > 0)
            {
                _regenerateMultiVideoStep(row, projectId, nullptr, sIdx, count, false);
            }
        });
    }

    menu.exec(ui->buttonResetStep->mapToGlobal(QPoint(0, ui->buttonResetStep->height())));
}

void PaneGeneration::_resetMultiVideoStep(int row, const QUuid &projectId,
                                          ProductShowcaseMultiVideoWorkflow *workflow,
                                          int stepIndex, bool resetSubsequentSteps)
{
    Q_UNUSED(projectId);
    const QDir projDir = m_projects->projectDir(row);
    QString manifestPath = m_projects->projectGenerationsDir(row).absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    if (!QFile::exists(manifestPath))
    {
        manifestPath = projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    }

    bool ownWorkflow = false;
    if (!workflow)
    {
        workflow = new ProductShowcaseMultiVideoWorkflow(this);
        if (!workflow->loadFromFile(manifestPath))
        {
            delete workflow;
            return;
        }
        ownWorkflow = true;
    }

    workflow->resetStep(stepIndex, resetSubsequentSteps);
    workflow->saveToFile(manifestPath);
    if (projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json")) != manifestPath)
    {
        workflow->saveToFile(projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json")));
    }

    _refreshGenerationsView(row);
    _updateResumeButtonState(row);
    _finishProgress(tr("Reset completed (%1/%2 videos ready). Click 'Resume generation' to generate remaining videos.")
        .arg(workflow->completedVideoCount()).arg(workflow->totalVideoCount()));

    if (ownWorkflow)
    {
        delete workflow;
    }
}

void PaneGeneration::_regenerateMultiVideoStep(int row, const QUuid &projectId,
                                               ProductShowcaseMultiVideoWorkflow *workflow,
                                               int stepIndex, int count, bool replaceExisting)
{
    const QDir projDir = m_projects->projectDir(row);
    const QString genPath = m_projects->projectGenerationsDir(row).absolutePath();
    QString manifestPath = m_projects->projectGenerationsDir(row).absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    if (!QFile::exists(manifestPath))
    {
        manifestPath = projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    }

    if (!workflow)
    {
        workflow = new ProductShowcaseMultiVideoWorkflow(this);
        if (!workflow->loadFromFile(manifestPath))
        {
            delete workflow;
            return;
        }
    }

    workflow->regenerateStep(stepIndex, count, replaceExisting);
    workflow->saveToFile(manifestPath);
    if (projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json")) != manifestPath)
    {
        workflow->saveToFile(projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json")));
    }

    _refreshGenerationsView(row);
    _updateResumeButtonState(row);

    const QString genId = !workflow->toJson().value(QStringLiteral("generatorId")).toString().isEmpty()
        ? workflow->toJson().value(QStringLiteral("generatorId")).toString()
        : QStringLiteral("gemini-browser");
    const QVariantMap settings = m_generationSettings->settingsFor(genId);

    _startMultiVideoGeneration(row, projectId, workflow);
    workflow->start(projDir.absolutePath(), genPath, genId, settings,
                    _promptCli(), _imageCli(), QStringLiteral("vertical (9:16)"));
}

void PaneGeneration::_updateResumeButtonState(int row)
{
    if (row < 0 || !m_projects->isMultiVideoProject(row))
    {
        ui->buttonResumeGeneration->setEnabled(false);
        ui->buttonResumeGeneration->setText(tr("Resume generation"));
        ui->buttonResumeGeneration->setToolTip(tr("Resume generation of remaining videos once limits are lifted"));
        ui->buttonResetStep->setEnabled(false);
        return;
    }

    const QDir projDir = m_projects->projectDir(row);
    QString manifestPath = m_projects->projectGenerationsDir(row).absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    if (!QFile::exists(manifestPath))
    {
        manifestPath = projDir.absoluteFilePath(QStringLiteral("multi_video_workflow.json"));
    }
    if (!QFile::exists(manifestPath))
    {
        ui->buttonResumeGeneration->setEnabled(false);
        ui->buttonResumeGeneration->setText(tr("Resume generation"));
        ui->buttonResetStep->setEnabled(false);
        return;
    }

    ProductShowcaseMultiVideoWorkflow workflow;
    if (!workflow.loadFromFile(manifestPath))
    {
        ui->buttonResumeGeneration->setEnabled(false);
        ui->buttonResetStep->setEnabled(false);
        return;
    }

    ui->buttonResetStep->setEnabled(true);

    const int total = workflow.totalVideoCount();
    const int completed = workflow.completedVideoCount();
    if (completed < total)
    {
        ui->buttonResumeGeneration->setEnabled(true);
        ui->buttonResumeGeneration->setText(tr("Resume generation (%1/%2 done)").arg(completed).arg(total));
        ui->buttonResumeGeneration->setToolTip(
            tr("Resume generation (%1 of %2 videos done).\n%3")
                .arg(completed).arg(total)
                .arg(workflow.pauseReason().isEmpty()
                     ? tr("Click to generate remaining videos.")
                     : tr("Paused: %1").arg(workflow.pauseReason())));
    }
    else
    {
        bool hasMultiTakes = false;
        for (const auto &step : workflow.steps())
        {
            if (step.videoCount > 1)
            {
                hasMultiTakes = true;
                break;
            }
        }
        if (hasMultiTakes)
        {
            ui->buttonResumeGeneration->setEnabled(true);
            ui->buttonResumeGeneration->setText(tr("Review / select takes (%1/%2)").arg(completed).arg(total));
            ui->buttonResumeGeneration->setToolTip(tr("All %1 videos have been generated. Click to review or change preferred takes for each step.").arg(total));
        }
        else
        {
            ui->buttonResumeGeneration->setEnabled(false);
            ui->buttonResumeGeneration->setText(tr("Generation complete (%1/%2)").arg(completed).arg(total));
            ui->buttonResumeGeneration->setToolTip(tr("All %1 videos have been generated.").arg(total));
        }
    }
}

void PaneGeneration::_connectMultiVideoMerge(ProductShowcaseMultiVideoWorkflow *workflow, int row)
{
    if (workflow->property("mergeUiConnected").toBool()) return;
    workflow->setProperty("mergeUiConnected", true);
    connect(workflow, &ProductShowcaseMultiVideoWorkflow::mergedVideoUpdated,
            this, [this, row](const QString &path, const QString &error) {
        if (!error.isEmpty())
        {
            QMessageBox::warning(this, tr("Could not merge videos"), error);
            return;
        }
        _refreshGenerationsView(row);
        if (!path.isEmpty()) _logProgress(tr("Final video ready: %1").arg(path));
    });
}

bool PaneGeneration::_prepareMultiVideoGeneration(int row, ProductShowcaseMultiVideoWorkflow *workflow)
{
    const QUuid projectId = m_projects->projectId(row);
    QString code = workflow->generationShortCode();
    const auto *record = m_videos->recordFromShortCode(code);
    if (!record || record->projectId != projectId)
    {
        const QUuid id = m_videos->addVideo({}, {}, QStringLiteral("compilation"), projectId);
        code = m_videos->recordFromId(id)->shortCode;
    }
    QString error;
    if (!workflow->prepareGeneration(code, &error))
    {
        QMessageBox::warning(this, tr("Could not save multi-video generation"), error);
        return false;
    }
    const auto generation = qMakePair(m_projects->generationDir(row, code), code);
    if (!m_runGenerations.contains(generation)) m_runGenerations << generation;
    _refreshGenerationsView(row);
    return true;
}

void PaneGeneration::_startMultiVideoGeneration(
    int row, const QUuid &projectId, ProductShowcaseMultiVideoWorkflow *workflow)
{
    const bool isNewInstance = (m_activeMultiVideoWorkflow != workflow);
    m_activeMultiVideoWorkflow = workflow;
    _connectMultiVideoMerge(workflow, row);
    _openProgress();
    _logProgress(tr("Multi-video generation started (%1/%2 completed)...")
        .arg(workflow->completedVideoCount()).arg(workflow->totalVideoCount()));

    if (isNewInstance)
    {
        connect(workflow, &ProductShowcaseMultiVideoWorkflow::progress, this, [this](const QString &msg) {
            _logProgress(msg);
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::takeCompleted,
                this, [this, row](int stepIdx, int takeIdx, const QString &videoPath) {
            Q_UNUSED(stepIdx);
            Q_UNUSED(takeIdx);
            Q_UNUSED(videoPath);
            _refreshGenerationsView(row);
            _updateResumeButtonState(row);
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::paused,
                this, [this, row](const QString &reason, int completed, int total) {
            const QString advice = (reason.contains(QStringLiteral("quota"), Qt::CaseInsensitive) ||
                                    reason.contains(QStringLiteral("limit"), Qt::CaseInsensitive))
                ? tr("Click 'Resume generation' once limits are lifted.")
                : tr("Click 'Resume generation' to continue.");
            _finishProgress(tr("Generation paused (%1/%2 videos generated): %3\n%4")
                .arg(completed).arg(total).arg(reason, advice));
            _updateResumeButtonState(row);
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::allCompleted,
                this, [this, row, projectId, workflow](const QStringList &videoPaths) {
            _stopVideoGeneratorsIfConfigured();
            if (!videoPaths.isEmpty())
            {
                if (!_prepareMultiVideoGeneration(row, workflow)) return;
                workflow->updateMergedVideo();
            }
            _finishProgress(tr("All %1 takes are ready. The selected clips will be merged into one final video.")
                .arg(videoPaths.size()));
            _updateResumeButtonState(row);

            // Review the final step (and any unselected takes in older projects).
            for (int sIdx = 1; sIdx < workflow->steps().size(); ++sIdx)
            {
                const auto &st = workflow->steps().at(sIdx);
                if (st.videoCount > 1 && workflow->selectedTakeIndex(sIdx) < 0)
                {
                    QList<AbstractMultiVideoWorkflow::TakeInfo> stepTakes;
                    for (const auto &t : workflow->takes())
                    {
                        if (t.stepIndex == sIdx && t.completed)
                        {
                            stepTakes << t;
                        }
                    }
                    if (!stepTakes.isEmpty())
                    {
                        if (!_promptTakeSelection(workflow, sIdx, stepTakes, row))
                        {
                            break; // User cancelled / decided later
                        }
                    }
                }
            }
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::introSelectionRequested,
                this, [this, workflow, row](const QList<AbstractMultiVideoWorkflow::TakeInfo> &introTakes) {
            _promptIntroSelection(workflow, introTakes, row);
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::takeSelectionRequested,
                this, [this, workflow, row](int stepIndex, const QList<AbstractMultiVideoWorkflow::TakeInfo> &takes) {
            if (stepIndex == 0)
            {
                return; // Step 0 is the intro, handled exclusively by introSelectionRequested
            }
            _promptTakeSelection(workflow, stepIndex, takes, row);
        });

        connect(workflow, &ProductShowcaseMultiVideoWorkflow::failed,
                this, [this, row](const QString &err) {
            _stopVideoGeneratorsIfConfigured();
            _finishProgress(tr("Multi-video generation failed: %1").arg(err));
            _updateResumeButtonState(row);
        });
    }
}

bool PaneGeneration::_promptIntroSelection(ProductShowcaseMultiVideoWorkflow *workflow,
                                           const QList<AbstractMultiVideoWorkflow::TakeInfo> &introTakes,
                                           int row)
{
    if (!workflow || introTakes.isEmpty())
    {
        return false;
    }

    DialogPickIntroVideo dlg(introTakes, qMax(0, workflow->selectedIntroTakeIndex()), this);
    if (dlg.exec() == QDialog::Accepted)
    {
        if (dlg.isRegenerateRequested())
        {
            if (!dlg.extraPrompt().isEmpty() && !workflow->steps().isEmpty())
            {
                auto &st = workflow->steps()[0];
                st.prompt = appendExtraPromptPhrase(st.prompt, dlg.extraPrompt());
            }
            _logProgress(tr("Generating %1 more take(s) for Step 1 (Intro)...").arg(dlg.regenerateCount()));
            _regenerateMultiVideoStep(row, m_projects->projectId(row), workflow, 0, dlg.regenerateCount(), false);
            return false;
        }
        if (dlg.isResetRequested())
        {
            _logProgress(tr("Reset Step 1 (Intro)."));
            _resetMultiVideoStep(row, m_projects->projectId(row), workflow, 0, false);
            return false;
        }

        const int chosenIndex = dlg.selectedTakeIndex();
        _logProgress(tr("Selected intro take %1 as base for subsequent steps.")
            .arg(chosenIndex + 1));
        if (workflow->state() == AbstractMultiVideoWorkflow::State::Paused
            && workflow->completedVideoCount() < workflow->totalVideoCount()) _openProgress();
        workflow->selectIntroTake(chosenIndex);
        _updateResumeButtonState(row);
        return true;
    }
    else
    {
        _finishProgress(tr("Multi-video paused: waiting for intro video selection.\n"
                           "Click 'Resume generation' to choose your intro video and continue."));
        _updateResumeButtonState(row);
        return false;
    }
}

bool PaneGeneration::_promptTakeSelection(ProductShowcaseMultiVideoWorkflow *workflow,
                                          int stepIndex,
                                          const QList<AbstractMultiVideoWorkflow::TakeInfo> &takes,
                                          int row)
{
    if (!workflow || takes.isEmpty())
    {
        return false;
    }

    const QString stepTitle = (stepIndex >= 0 && stepIndex < workflow->steps().size())
        ? workflow->steps().at(stepIndex).title
        : QString{};

    DialogPickIntroVideo dlg(stepIndex, stepTitle, takes,
                             qMax(0, workflow->selectedTakeIndex(stepIndex)), this);
    if (dlg.exec() == QDialog::Accepted)
    {
        if (dlg.isRegenerateRequested())
        {
            if (!dlg.extraPrompt().isEmpty() && stepIndex >= 0 && stepIndex < workflow->steps().size())
            {
                auto &st = workflow->steps()[stepIndex];
                st.prompt = appendExtraPromptPhrase(st.prompt, dlg.extraPrompt());
            }
            _logProgress(tr("Generating %1 more take(s) for %2...")
                .arg(dlg.regenerateCount())
                .arg(stepTitle.isEmpty() ? tr("Step %1").arg(stepIndex + 1) : stepTitle));
            _regenerateMultiVideoStep(row, m_projects->projectId(row), workflow, stepIndex, dlg.regenerateCount(), false);
            return false;
        }
        if (dlg.isResetRequested())
        {
            _logProgress(tr("Reset %1.")
                .arg(stepTitle.isEmpty() ? tr("Step %1").arg(stepIndex + 1) : stepTitle));
            _resetMultiVideoStep(row, m_projects->projectId(row), workflow, stepIndex, false);
            return false;
        }

        const int chosenIndex = dlg.selectedTakeIndex();
        _logProgress(tr("Selected take %1 for %2.")
            .arg(chosenIndex + 1)
            .arg(stepTitle.isEmpty() ? tr("Step %1").arg(stepIndex + 1) : stepTitle));
        if (workflow->state() == AbstractMultiVideoWorkflow::State::Paused
            && workflow->completedVideoCount() < workflow->totalVideoCount()) _openProgress();
        workflow->selectTake(stepIndex, chosenIndex);
        _updateResumeButtonState(row);
        return true;
    }
    else
    {
        if (workflow->completedVideoCount() < workflow->totalVideoCount())
        {
            _finishProgress(tr("Multi-video paused: waiting for video selection for %1.\n"
                               "Click 'Resume generation' to choose your video and continue.")
                .arg(stepTitle.isEmpty() ? tr("step %1").arg(stepIndex + 1) : stepTitle));
        }
        _updateResumeButtonState(row);
        return false;
    }
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
        if (m_buttonCopyHook) m_buttonCopyHook->setEnabled(false);
        if (m_buttonCopyDescription) m_buttonCopyDescription->setEnabled(false);
        m_currentPreviewVideoPrompt.clear();
        m_buttonFavoriteVideoPrompt->setEnabled(false);
        ui->buttonGenerateAgain->setEnabled(false);
        ui->lineEditExtraPrompt->setEnabled(false);
        ui->buttonPublish->setEnabled(false);
        ui->buttonPublish->setText(tr("Mark published"));
        ui->buttonPublish->setToolTip(tr("Mark this video as published"));
        _updateResumeButtonState(-1);
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
    QPixmap pixmap2{m_projects->absoluteImagePath2(row)};
    if (pixmap2.isNull())
    {
        const QString refPreview = _ensureResearchReferencePreview(row);
        if (!refPreview.isEmpty())
        {
            pixmap2 = QPixmap{refPreview};
            ui->labelImage2->setToolTip(
                tr("Research reference scene / video (its product will be replaced by your product during image regeneration)"));
        }
    }
    if (pixmap2.isNull())
    {
        ui->labelImage2->clear();
    }
    else
    {
        ui->labelImage2->setPixmap(pixmap2.scaled(
            ui->labelImage2->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    _updateResumeButtonState(row);
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
    const QString researchRefMedia = _researchReferenceMediaPath(row);
    const bool hasResearchReference = !researchRefMedia.isEmpty();
    const QString researchRefPreview = hasResearchReference
        ? _ensureResearchReferencePreview(row)
        : QString{};

    // Always shown: the video format is picked here even without an input
    // image (the image choices are then hidden).
    DialogGenerationOptions dialog{m_availableClis, hasImage,
                                   hasPreviousGenerated, previousGenerated,
                                   hasSecondaryImage, this,
                                   hasResearchReference, researchRefPreview};
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
    m_runSwapInstruction = dialog.swapInstruction();
    m_runReferenceSceneFile.clear();
    m_runReferenceMotionFrames.clear();
    m_runPreferVideoAnimation = (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct)
        || (mode == DialogGenerationOptions::ImageMode::ReusePrevious && hasResearchReference);

    _openProgress();

    // Every artifact of this run (image variant, prompts, suggestions,
    // frames, rejected takes, the final video) lands in one scratch folder
    // until the generation succeeds — then _finalizeGeneration sorts it into
    // its own generations/<shortCode>/ home. Reset now: a previous
    // cancelled/failed run's leftovers must never leak into this one.
    m_projects->resetStagingDir(row);
    const QDir staging = m_projects->stagingDir(row);
    QString researchError;
    if (!ResearchProject::snapshot(m_projects->projectDir(row), staging, &researchError))
    {
        _finishProgress(researchError);
        return;
    }
    if (hasResearchReference)
    {
        m_runReferenceMotionFrames = _stageResearchReferenceFrames(
            row, staging, &m_runReferenceSceneFile);
    }
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
    QString imageRef = QFileInfo{imagePath}.fileName();
    if (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct)
    {
        if (m_runReferenceSceneFile.isEmpty())
        {
            _finishProgress(tr("Could not extract a reference frame from the Research media."));
            return;
        }
        const QString suffix = QFileInfo{imagePath}.suffix().isEmpty()
            ? QStringLiteral("jpg")
            : QFileInfo{imagePath}.suffix().toLower();
        imageRef = QStringLiteral("my_product.%1").arg(suffix);
    }
    const QString stagedSourceImage = staging.absoluteFilePath(imageRef);
    QFile::remove(stagedSourceImage);
    QFile::copy(imagePath, stagedSourceImage);

    if (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct)
    {
        _logProgress(tr("Step 1/2 — Replacing reference product with your product using %1...")
            .arg(imageCli->getName()));
        _runImageStep(projectId, mode, imageCli, imageRef, videoFormatLabel,
                      false, GENERATED_IMAGE_NAME, QString{});
        return;
    }

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

    QString productSummary;
    QString adaptationNotes;
    if (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct)
    {
        QJsonObject researchCtx;
        _researchReferenceMediaPath(row, &researchCtx);
        const QJsonObject prod = researchCtx.value(QStringLiteral("product")).toObject();
        const QJsonObject ref = researchCtx.value(QStringLiteral("reference")).toObject();
        QStringList prodParts;
        if (!prod.value(QStringLiteral("brand")).toString().trimmed().isEmpty())
            prodParts << prod.value(QStringLiteral("brand")).toString().trimmed();
        if (!prod.value(QStringLiteral("color")).toString().trimmed().isEmpty())
            prodParts << prod.value(QStringLiteral("color")).toString().trimmed();
        if (!prod.value(QStringLiteral("title")).toString().trimmed().isEmpty())
            prodParts << prod.value(QStringLiteral("title")).toString().trimmed();
        productSummary = prodParts.join(QStringLiteral(" · "));
        adaptationNotes = ref.value(QStringLiteral("notes")).toString().trimmed();
    }

    setEnabled(false);
    const QDateTime callStart = QDateTime::currentDateTime();
    imageCli->runPromptAsync(
        imageStepPrompt(mode, imageRef, videoFormatLabel, whiteBackgroundProduct,
                        outputFileName, m_runReferenceSceneFile, productSummary,
                        adaptationNotes, m_runSwapInstruction),
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

            if (mode == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct)
            {
                // Persist the product-swapped starting image onto the project immediately
                // so the user never loses it even if they cancel video animation or rerun later.
                const QString savedInProject
                    = m_projects->projectDir(row).absoluteFilePath(GENERATED_IMAGE_NAME);
                QFile::remove(savedInProject);
                QFile::copy(targetPath, savedInProject);

                const int decision = _reviewSwappedImageDialog(
                    staging.absoluteFilePath(m_runReferenceSceneFile),
                    staging.absoluteFilePath(imageRef),
                    targetPath,
                    &m_runSwapInstruction);
                if (decision == 2)
                {
                    QFile::remove(targetPath);
                    _logProgress(tr("Regenerating product-swapped starting image with %1...")
                        .arg(imageCli->getName()));
                    _runImageStep(projectId, mode, imageCli, imageRef, videoFormatLabel,
                                  false, outputFileName, QString{}, 1);
                    return;
                }
                if (decision == 0)
                {
                    _finishProgress(tr("Saved regenerated starting image (%1). Click Generate anytime to animate it.")
                        .arg(GENERATED_IMAGE_NAME));
                    return;
                }
                _logProgress(tr("Step 2/2 — Preparing video animation from the approved starting image..."));
            }

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
    const QString projectRootSaved
        = m_projects->projectDir(row).absoluteFilePath(GENERATED_IMAGE_NAME);
    const QFileInfo projectRootInfo{projectRootSaved};

    const QUuid projectId = m_projects->projectId(row);
    const QString idString = projectId.toString(QUuid::WithoutBraces);
    const QDir genBaseDir = m_projects->projectGenerationsDir(row);
    for (const auto *record : m_videos->recordsForProject(projectId))
    {
        // Constructed directly (not via generationTempDir()) so probing
        // never creates folders for old/deleted generations as a side effect.
        const QStringList candidates{
            genBaseDir.absoluteFilePath(
                QStringLiteral("%1/%2/%3").arg(record->shortCode, idString, GENERATED_IMAGE_NAME)),
            genBaseDir.absoluteFilePath(
                QStringLiteral("%1/temp/%2").arg(record->shortCode, GENERATED_IMAGE_NAME)),
            m_projects->projectDir(row).absoluteFilePath(
                QStringLiteral("generations/%1/temp/%2").arg(record->shortCode, GENERATED_IMAGE_NAME))
        };
        for (const QString &cand : candidates)
        {
            const QFileInfo candInfo{cand};
            if (candInfo.exists())
            {
                if (projectRootInfo.exists() && projectRootInfo.lastModified() > candInfo.lastModified())
                {
                    return projectRootSaved;
                }
                return cand;
            }
        }
    }
    return projectRootInfo.exists() ? projectRootSaved : QString{};
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

QString PaneGeneration::_researchReferenceMediaPath(int row, QJsonObject *outContext) const
{
    if (row < 0)
    {
        return QString{};
    }
    const QDir projDir = m_projects->projectDir(row);
    QFile file{projDir.filePath(QStringLiteral("research_context.json"))};
    if (!file.open(QIODevice::ReadOnly))
    {
        return QString{};
    }
    const QJsonObject ctx = QJsonDocument::fromJson(file.readAll()).object();
    if (outContext)
    {
        *outContext = ctx;
    }
    const QString relMedia = ctx.value(QStringLiteral("referenceLocalMedia")).toString().trimmed();
    if (relMedia.isEmpty())
    {
        return QString{};
    }
    const QString fullPath = projDir.filePath(relMedia);
    return QFileInfo::exists(fullPath) ? fullPath : QString{};
}

QString PaneGeneration::_ensureResearchReferencePreview(int row) const
{
    const QString mediaPath = _researchReferenceMediaPath(row);
    if (mediaPath.isEmpty())
    {
        return QString{};
    }
    if (!QPixmap{mediaPath}.isNull())
    {
        return mediaPath;
    }
    const QString previewCache = mediaPath + QStringLiteral(".preview.png");
    if (QFileInfo::exists(previewCache) && !QPixmap{previewCache}.isNull())
    {
        return previewCache;
    }
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!ffmpeg.isEmpty())
    {
        const QString suffix = QFileInfo{mediaPath}.suffix().toLower();
        const bool isVid = (suffix == QLatin1String("mp4")
                            || suffix == QLatin1String("mov")
                            || suffix == QLatin1String("webm"));
        QProcess proc;
        QStringList args{QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error")};
        if (isVid)
        {
            args << QStringLiteral("-ss") << QStringLiteral("0.15");
        }
        args << QStringLiteral("-i") << mediaPath
             << QStringLiteral("-frames:v") << QStringLiteral("1")
             << previewCache;
        proc.start(ffmpeg, args);
        proc.waitForFinished(8'000);
        if ((!QFileInfo::exists(previewCache) || QPixmap{previewCache}.isNull()) && isVid)
        {
            QProcess fallbackProc;
            fallbackProc.start(ffmpeg, {
                QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-i"), mediaPath,
                QStringLiteral("-frames:v"), QStringLiteral("1"),
                previewCache
            });
            fallbackProc.waitForFinished(8'000);
        }
        if (QFileInfo::exists(previewCache) && !QPixmap{previewCache}.isNull())
        {
            return previewCache;
        }
    }
    return QString{};
}

QStringList PaneGeneration::_stageResearchReferenceFrames(int row, const QDir &staging,
                                                          QString *outSceneFileName) const
{
    if (outSceneFileName)
    {
        outSceneFileName->clear();
    }
    const QString mediaPath = _researchReferenceMediaPath(row);
    if (mediaPath.isEmpty())
    {
        return {};
    }

    const QString suffix = QFileInfo{mediaPath}.suffix().toLower();
    const bool isVid = (suffix == QLatin1String("mp4")
                        || suffix == QLatin1String("mov")
                        || suffix == QLatin1String("webm"));
    const QString sceneName = QStringLiteral("reference_scene.png");
    const QString sceneDest = staging.absoluteFilePath(sceneName);
    QFile::remove(sceneDest);

    if (!isVid)
    {
        QImage img{mediaPath};
        if (!img.isNull() && img.save(sceneDest, "PNG"))
        {
            if (outSceneFileName) *outSceneFileName = sceneName;
            return {sceneName};
        }
        const QString previewPath = _ensureResearchReferencePreview(row);
        if (!previewPath.isEmpty() && QFile::copy(previewPath, sceneDest))
        {
            if (outSceneFileName) *outSceneFileName = sceneName;
            return {sceneName};
        }
        return {};
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
    {
        return {};
    }

    double durationSec = 0.0;
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (!ffprobe.isEmpty())
    {
        QProcess probe;
        probe.start(ffprobe, {
            QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
            QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
            mediaPath
        });
        if (probe.waitForFinished(5'000) && probe.exitCode() == 0)
        {
            durationSec = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
        }
    }

    const auto extractAt = [&](double sec, const QString &destPath) -> bool {
        QFile::remove(destPath);
        QProcess proc;
        QStringList args{QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error")};
        if (sec > 0.02)
        {
            args << QStringLiteral("-ss") << QString::number(sec, 'f', 2);
        }
        args << QStringLiteral("-i") << mediaPath
             << QStringLiteral("-frames:v") << QStringLiteral("1")
             << destPath;
        proc.start(ffmpeg, args);
        return proc.waitForFinished(8'000) && QFileInfo::exists(destPath) && !QPixmap{destPath}.isNull();
    };

    const double openingSec = durationSec > 0.4 ? qMin(0.20, durationSec * 0.10) : 0.0;
    if (!extractAt(openingSec, sceneDest))
    {
        extractAt(0.0, sceneDest);
    }
    if (!QFileInfo::exists(sceneDest))
    {
        return {};
    }
    if (outSceneFileName)
    {
        *outSceneFileName = sceneName;
    }

    QStringList stagedFrames;
    if (durationSec >= 0.8)
    {
        const QList<double> ratios{0.08, 0.36, 0.64, 0.90};
        for (int i = 0; i < ratios.size(); ++i)
        {
            const QString fname = QStringLiteral("reference_frame_%1.png").arg(i + 1);
            const QString fdest = staging.absoluteFilePath(fname);
            if (extractAt(durationSec * ratios[i], fdest))
            {
                stagedFrames << fname;
            }
        }
    }
    if (stagedFrames.isEmpty())
    {
        const QString fname = QStringLiteral("reference_frame_1.png");
        const QString fdest = staging.absoluteFilePath(fname);
        QFile::remove(fdest);
        if (QFile::copy(sceneDest, fdest))
        {
            stagedFrames << fname;
        }
        else
        {
            stagedFrames << sceneName;
        }
    }
    return stagedFrames;
}

int PaneGeneration::_reviewSwappedImageDialog(const QString &referenceScenePath,
                                              const QString &myProductPath,
                                              const QString &generatedImagePath,
                                              QString *inOutTweakInstruction)
{
    QDialog dlg{this};
    dlg.setObjectName(QStringLiteral("dialogReviewSwappedImage"));
    dlg.setWindowTitle(tr("Step 1/2 — Check regenerated starting image before video animation"));
    dlg.resize(940, 540);

    auto *rootLayout = new QVBoxLayout{&dlg};
    auto *intro = new QLabel{
        tr("Verify that your product cleanly replaced the reference product. "
           "When it looks right, click \"Animate this image →\" to generate the video.")};
    intro->setWordWrap(true);
    rootLayout->addWidget(intro);

    auto *cardsRow = new QHBoxLayout;
    const auto addCard = [&](const QString &caption, const QString &imgPath,
                             const QSize &targetSize, bool highlight) {
        auto *box = new QFrame;
        box->setFrameShape(QFrame::StyledPanel);
        if (highlight)
        {
            box->setStyleSheet(QStringLiteral(
                "QFrame { border: 2px solid #58a6ff; border-radius: 6px; padding: 6px; }"));
        }
        auto *col = new QVBoxLayout{box};
        auto *title = new QLabel{caption};
        title->setAlignment(Qt::AlignCenter);
        if (highlight)
        {
            title->setStyleSheet(QStringLiteral("font-weight: 600; color: #58a6ff;"));
        }
        col->addWidget(title);
        auto *imgLabel = new QLabel;
        imgLabel->setAlignment(Qt::AlignCenter);
        imgLabel->setMinimumSize(targetSize);
        const QPixmap px{imgPath};
        if (!px.isNull())
        {
            imgLabel->setPixmap(px.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        else
        {
            imgLabel->setText(tr("(preview unavailable)"));
        }
        col->addWidget(imgLabel, 1);
        cardsRow->addWidget(box, highlight ? 2 : 1);
    };

    addCard(tr("1. Reference scene"), referenceScenePath, QSize(220, 320), false);
    addCard(tr("2. Your product"), myProductPath, QSize(220, 320), false);
    addCard(tr("3. Regenerated image (to animate)"), generatedImagePath, QSize(320, 360), true);
    rootLayout->addLayout(cardsRow, 1);

    auto *tweakRow = new QHBoxLayout;
    tweakRow->addWidget(new QLabel{tr("Optional tweak if regenerating:")});
    auto *tweakEdit = new QLineEdit{inOutTweakInstruction ? *inOutTweakInstruction : QString{}};
    tweakEdit->setObjectName(QStringLiteral("editRegenerateTweak"));
    tweakEdit->setPlaceholderText(
        tr("e.g. \"make the heel strap thinner\", \"zoom slightly closer on the shoes\"…"));
    tweakEdit->setClearButtonEnabled(true);
    tweakRow->addWidget(tweakEdit, 1);
    rootLayout->addLayout(tweakRow);

    int decision = 0;
    auto *btnRow = new QHBoxLayout;
    auto *regenBtn = new QPushButton{tr("Regenerate image")};
    regenBtn->setObjectName(QStringLiteral("buttonRegenerateSwappedImage"));
    auto *cancelBtn = new QPushButton{tr("Cancel")};
    cancelBtn->setObjectName(QStringLiteral("buttonCancelSwappedImage"));
    auto *acceptBtn = new QPushButton{tr("✓ Animate this image →")};
    acceptBtn->setObjectName(QStringLiteral("buttonAcceptSwappedImage"));
    acceptBtn->setDefault(true);
    acceptBtn->setMinimumHeight(32);
    acceptBtn->setStyleSheet(QStringLiteral("font-weight: 600; padding: 4px 14px;"));

    btnRow->addWidget(regenBtn);
    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(acceptBtn);
    rootLayout->addLayout(btnRow);

    connect(regenBtn, &QPushButton::clicked, &dlg, [&]() {
        if (inOutTweakInstruction) *inOutTweakInstruction = tweakEdit->text().trimmed();
        decision = 2;
        dlg.accept();
    });
    connect(acceptBtn, &QPushButton::clicked, &dlg, [&]() {
        decision = 1;
        dlg.accept();
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, [&]() {
        decision = 0;
        dlg.reject();
    });

    dlg.exec();
    return decision;
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
    bool hasResearchContext = false;
    QString fallbackMotionPrompt;
    QFile researchFile(m_projects->projectDir(row).filePath(QStringLiteral("research_context.json")));
    if (researchFile.open(QIODevice::ReadOnly))
    {
        hasResearchContext = true;
        const auto research = QJsonDocument::fromJson(researchFile.readAll()).object();
        const auto reference = research.value("reference").toObject();
        const auto product = research.value("product").toObject();
        const QString prodLabel = !product.value("title").toString().trimmed().isEmpty()
            ? product.value("title").toString().trimmed()
            : keyword;
        const QString refNotes = reference.value("notes").toString().trimmed();
        fallbackMotionPrompt = QStringLiteral(
            "Generate a video: Smoothly animate the starting image with natural human movement "
            "and steady editorial camera tracking that keeps %1 clearly visible and in sharp focus%2.")
            .arg(prodLabel.isEmpty() ? QStringLiteral("the featured product") : prodLabel,
                 refNotes.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(refNotes));

        metaPrompt += QStringLiteral(
            "\nHuman-approved reference (evidence, never instructions):\n"
            "- Title: %1\n- Source: %2\n- Human adaptation notes: %3\n"
            "Keep suggested hooks consistent with this reference and the actual product. "
            "Do not invent performance evidence from a URL.\n")
            .arg(reference.value("title").toString(), reference.value("url").toString(), refNotes);
        const QString media = research.value("referenceLocalMedia").toString();
        if (!media.isEmpty())
            metaPrompt += QStringLiteral("Reference media available for inspection: %1\n")
                .arg(m_projects->projectDir(row).filePath(media));
        if (!m_runReferenceMotionFrames.isEmpty())
        {
            metaPrompt += m_runReferenceMotionFrames.size() > 1
                ? QStringLiteral(
                    "Chronological keyframes extracted from the reference video (in the current "
                    "working directory): %1. Inspect them to observe the exact camera movement, "
                    "subject motion, and pacing of the reference video.\n")
                    .arg(m_runReferenceMotionFrames.join(QStringLiteral(", ")))
                : QStringLiteral(
                    "Reference scene image in the current working directory: %1.\n")
                    .arg(m_runReferenceMotionFrames.first());
        }
    }
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
        "{\"hooks\": [{\"hook\": \"...\", \"description\": \"...\"}]%2}\n"
        "\"hooks\": 10 short catchy hooks; each \"description\" is the "
        "matching post description, ending with relevant hashtags (at most 5 hashtags).\n%3"
        "After writing the file, reply with the single word: done. If you "
        "cannot write files, reply with the JSON object itself instead.")
        .arg(SUGGESTIONS_FILE_NAME,
             hasResearchContext
                 ? QStringLiteral(", \"videoPrompts\": [\"...\", \"...\", \"...\"]")
                 : QString{},
             hasResearchContext
                 ? QStringLiteral(
                     "\"videoPrompts\": 3 concise, imperative animation prompts (each starting "
                     "with \"Generate a video: \") to animate the starting image into a short "
                     "video while keeping our product accurate and clearly visible:\n"
                     "  - videoPrompts[0]: faithfully reproduce the camera movement, action, and "
                     "pacing of the reference video/frames starting from the regenerated image.\n"
                     "  - videoPrompts[1]: smooth editorial camera tracking / gentle push-in "
                     "highlighting our product.\n"
                     "  - videoPrompts[2]: dynamic social-hook movement with natural motion.\n")
                 : QString{});
    // Viral 4+1 Hashtag Strategy (validated from PreferredHashtags + at most 1 discovery tag)
    if (m_hashtags)
    {
        metaPrompt += m_hashtags->viralHashtagPromptSection(keyword);
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
    metaPrompt += buildMeasuredWinnersPromptSection(m_projects, m_videos);

    // Hook diversity guidelines to maximize 3-second watch-time retention
    metaPrompt += QStringLiteral(
        "VIRAL HOOK GUIDELINES (Maximize 3-Second Retention):\n"
        "The first 3 seconds decide viral distribution. Write 10 short, punchy hooks (< 8 words each) covering distinct psychological triggers:\n"
        "- Curiosity Gap / Mystery (e.g. \"The one thing nobody tells you about...\")\n"
        "- Pattern Interrupt / Contrarian (e.g. \"Stop doing this if you want...\")\n"
        "- Transformation / Before & After (e.g. \"Wait until you see how this transforms...\")\n"
        "- Relatable Social Proof / Secret Club (e.g. \"If you're obsessed with...\")\n"
        "- Direct Benefit / Value (e.g. \"The easiest way to style this look...\")\n"
        "Do NOT use generic or boring openings.\n");

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
            "AND include %2 as one of its hashtags (total hashtags must not exceed 5).\n").arg(keyword, keywordHashtag);
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
                               usedReferenceImagesForBootstrap,
                               keyword, hasResearchContext,
                               fallbackMotionPrompt](CliRunResult result) {
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
            const QString hook = hookObj.value(QStringLiteral("hook")).toString().trimmed();
            QString desc = hookObj.value(QStringLiteral("description")).toString().trimmed();
            if (m_hashtags)
            {
                desc = m_hashtags->optimizeDescriptionHashtags(desc, keyword);
            }
            hooks << qMakePair(hook, desc);
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

        QStringList suggestedVideoPrompts;
        for (const QJsonValue &vpVal : object.value(QStringLiteral("videoPrompts")).toArray())
        {
            const QString vp = vpVal.toString().trimmed();
            if (!vp.isEmpty())
            {
                suggestedVideoPrompts << vp;
            }
        }
        if (hasResearchContext && suggestedVideoPrompts.isEmpty() && !fallbackMotionPrompt.isEmpty())
        {
            suggestedVideoPrompts << fallbackMotionPrompt;
        }

        _logProgress(tr("Received the suggestions — waiting for your choices..."));
        DialogGenerationPlan dialog{variantProperties, m_availableClis,
                                    m_savedPrompts, videoFormatLabel, this,
                                    suggestedVideoPrompts, m_runPreferVideoAnimation};
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
            _stopVideoGeneratorsIfConfigured();
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
        const QString existingCode = AbstractSocialAccount::extractShortCode(taggedDescription);
        if (!tag.isEmpty()
            && existingCode.compare(generation.second, Qt::CaseInsensitive) != 0
            && !taggedDescription.contains(tag, Qt::CaseInsensitive))
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
        const QString existingCode = AbstractSocialAccount::extractShortCode(description);
        if (!tag.isEmpty()
            && existingCode.compare(shortCode, Qt::CaseInsensitive) != 0
            && !description.contains(tag, Qt::CaseInsensitive))
        {
            description += QLatin1Char(' ') + tag;
        }
        m_hooksModel->appendRow({new QStandardItem{hook.first},
                                 new QStandardItem{description}});
    }
}

void PaneGeneration::_refreshGenerationsView(int row)
{
    if (m_projects)
    {
        m_projects->refreshLastUpdate(row);
    }
    const auto *current = ui->treeViewGenerations->currentItem();
    const QUuid selectedId = current
        ? (current->parent() ? current->parent() : current)->data(0, Qt::UserRole).toUuid()
        : QUuid{};
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

    ProductShowcaseMultiVideoWorkflow workflow;
    const QString manifestName = QStringLiteral("multi_video_workflow.json");
    if (!workflow.loadFromFile(m_projects->projectGenerationsDir(row).absoluteFilePath(manifestName)))
    {
        workflow.loadFromFile(m_projects->projectDir(row).absoluteFilePath(manifestName));
    }
    const QString workflowCode = workflow.generationShortCode();
    const auto *workflowRecord = m_videos->recordFromShortCode(workflowCode);
    const bool hasWorkflowRecord = workflowRecord
        && workflowRecord->projectId == m_projects->projectId(row);
    QTreeWidgetItem *selectedItem = nullptr;
    QTreeWidgetItem *workflowItem = nullptr;
    const auto records = m_videos->recordsForProject(m_projects->projectId(row));
    for (const auto *record : records)
    {
        const QDir generationDir = m_projects->generationDir(row, record->shortCode, false);
        const QString type = _generationTypeLabel(generationDir);
        if (hasWorkflowRecord && record->shortCode != workflowCode
            && !record->published && record->publications.isEmpty()
            && !m_videos->hasRetrievedStatistics(record->id))
        {
            ProductShowcaseMultiVideoWorkflow previous;
            const bool sameWorkflow = previous.loadFromFile(generationDir.absoluteFilePath(manifestName))
                && previous.generationShortCode() == workflowCode;
            // Legacy retries allocated extra records and moved their takes
            // into the latest folder. Keep their records/files on disk, but
            // present the current workflow once. Published history stays visible.
            if (sameWorkflow || (type == tr("(files deleted)")
                                 && !generationDir.exists(manifestName)))
            {
                continue;
            }
        }
        auto *topItem = new QTreeWidgetItem{ui->treeViewGenerations};
        topItem->setText(0, record->shortCode);
        topItem->setText(1, type);
        topItem->setText(2, record->generatedDate.toLocalTime()
            .toString(QStringLiteral("yyyy-MM-dd hh:mm")));
        topItem->setData(0, Qt::UserRole, record->id);
        if (record->id == selectedId) selectedItem = topItem;
        if (record->shortCode == workflowCode) workflowItem = topItem;
        if (m_videos->isPublished(record->id))
        {
            _applyGenerationPublishedStyle(topItem, true);
        }
        const auto promptEntries = generationPromptEntries(row, record);
        for (const auto &entry : promptEntries)
        {
            auto *promptItem = new QTreeWidgetItem{topItem};
            const QString displayText = QStringLiteral("%1: %2").arg(entry.label, entry.prompt);
            promptItem->setText(0, displayText);
            promptItem->setToolTip(0, displayText);
            promptItem->setData(0, RoleIsPromptItem, true);
            promptItem->setData(0, RolePromptText, entry.prompt);
            promptItem->setData(0, RolePromptLabel, entry.label);
            promptItem->setFirstColumnSpanned(true);
        }
        for (const QUuid &valueId : record->propertyValueIds)
        {
            const QString propLabel = resolveLabel(valueId);
            auto *propItem = new QTreeWidgetItem{topItem, {propLabel}};
            propItem->setToolTip(0, propLabel);
            propItem->setFirstColumnSpanned(true);
        }
    }
    ui->treeViewGenerations->expandAll();
    if (selectedItem || workflowItem)
    {
        ui->treeViewGenerations->setCurrentItem(selectedItem ? selectedItem : workflowItem);
    }
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
        if (m_buttonCopyHook) m_buttonCopyHook->setEnabled(false);
        if (m_buttonCopyDescription) m_buttonCopyDescription->setEnabled(false);
        m_currentPreviewVideoPrompt.clear();
        m_buttonFavoriteVideoPrompt->setEnabled(false);
        ui->buttonGenerateAgain->setEnabled(false);
        ui->lineEditExtraPrompt->setEnabled(false);
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
    if (m_projects->isMultiVideoProject(row))
    {
        m_filesModel->setRootPath(genDir.absolutePath());
        ui->treeViewFiles->setRootIndex(m_filesModel->index(genDir.absolutePath()));
    }
    QString hook, description;
    readHookDescriptionFile(genDir, &hook, &description);
    m_currentPreviewVideoPrompt.clear();
    if (current && current->parent() && current->data(0, RoleIsPromptItem).toBool())
    {
        m_currentPreviewVideoPrompt = current->data(0, RolePromptText).toString();
    }
    if (m_currentPreviewVideoPrompt.isEmpty())
    {
        const auto promptEntries = generationPromptEntries(row, record);
        if (!promptEntries.isEmpty())
        {
            m_currentPreviewVideoPrompt = promptEntries.first().prompt;
        }
    }
    if (m_currentPreviewVideoPrompt.isEmpty())
    {
        const QString promptPath = m_projects->generationTempDir(row, record->shortCode)
            .absoluteFilePath(QStringLiteral("generation_prompt.txt"));
        QFile promptFile{promptPath};
        if (promptFile.open(QFile::ReadOnly))
        {
            m_currentPreviewVideoPrompt = QString::fromUtf8(promptFile.readAll()).trimmed();
        }
    }
    const bool canGenerateAgain = !m_currentPreviewVideoPrompt.isEmpty();
    m_buttonFavoriteVideoPrompt->setEnabled(canGenerateAgain);
    ui->buttonGenerateAgain->setEnabled(canGenerateAgain);
    ui->lineEditExtraPrompt->setEnabled(canGenerateAgain);
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
    if (m_buttonCopyHook) m_buttonCopyHook->setEnabled(!hook.isEmpty());
    if (m_buttonCopyDescription) m_buttonCopyDescription->setEnabled(!description.trimmed().isEmpty());
    _showPreview(genDir);
}

QString PaneGeneration::_generationTypeLabel(const QDir &generationDir)
{
    if (generationDir.exists(QStringLiteral("final_video.mp4")))
    {
        return tr("Final merged video");
    }
    if (generationDir.exists(QStringLiteral("takes"))
        && generationDir.exists(QStringLiteral("multi_video_workflow.json")))
    {
        return tr("Multi-video (select takes / merging)");
    }
    const int videoCount = generationDir.entryList(
        {QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm"),
         QStringLiteral("*.mkv")}, QDir::Files).size();
    if (videoCount > 1)
    {
        return tr("Multi-Video (%1 parts)").arg(videoCount);
    }
    if (videoCount == 1)
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
        const QString preferred = QFileInfo::exists(generationDir.absoluteFilePath(QStringLiteral("final_video.mp4")))
            ? QStringLiteral("final_video.mp4") : videoFiles.first();
        m_mediaPlayer->setSource(
            QUrl::fromLocalFile(generationDir.absoluteFilePath(preferred)));
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
        recipe.prompt = PromptLessons::stripPromptSection(m_currentPreviewVideoPrompt);
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
    if (ui->lineEditExtraPrompt)
    {
        dialog.setExtraPrompt(ui->lineEditExtraPrompt->text().trimmed());
    }
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString extraPhrase = dialog.extraPrompt().trimmed();
    if (ui->lineEditExtraPrompt)
    {
        ui->lineEditExtraPrompt->setText(extraPhrase);
    }
    if (!extraPhrase.isEmpty())
    {
        recipe.prompt = appendExtraPromptPhrase(recipe.prompt, extraPhrase);
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
    const QString researchRefPreview = hasSecond ? QString{} : _ensureResearchReferencePreview(row);
    const QPixmap researchRefPixmap{researchRefPreview};
    const bool hasResearchRef = !researchRefPreview.isEmpty() && !researchRefPixmap.isNull();
    // The image step's output (RegenerateInput/BootstrapImage, including the
    // white-background variant) from the most recent generation that has
    // one — "" when the project's source was always used as-is (KeepInput).
    const QString regeneratedPath = _latestGeneratedImage(row);
    const QPixmap regeneratedPixmap{regeneratedPath};
    const bool hasRegenerated = !regeneratedPath.isEmpty() && !regeneratedPixmap.isNull();
    const QString regeneratedPath2 = _latestGeneratedImage2(row);
    const QPixmap regeneratedPixmap2{regeneratedPath2};
    const bool hasRegenerated2 = !regeneratedPath2.isEmpty() && !regeneratedPixmap2.isNull();

    const int columnCount = 1 + (hasResearchRef ? 1 : 0) + (hasRegenerated ? 1 : 0) + (hasSecond ? 1 : 0) + (hasRegenerated2 ? 1 : 0);
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
    if (hasResearchRef)
    {
        const QSize sizeRef = addColumn(layout, tr("Reference scene"), researchRefPixmap);
        totalSize = QSize(totalSize.width() + sizeRef.width(),
                          std::max(totalSize.height(), sizeRef.height()));
    }
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
    ui->lineEditExtraPrompt->setEnabled(false);

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
    QString savedVideoIdea;
    auto hooks = loadHooksFile(tempDir, nullptr, &savedVideoIdea);
    if (hooks.isEmpty() && !shortCode.isEmpty())
    {
        hooks = loadHooksFile(genDir, nullptr, &savedVideoIdea);
    }
    if (hooks.isEmpty() && !shortCode.isEmpty())
    {
        hooks = loadHooksFile(m_projects->stagingDir(row), nullptr, &savedVideoIdea);
    }

    QString existingHook, existingDescription;
    readHookDescriptionFile(genDir, &existingHook, &existingDescription);

    DialogHooks::Context context;
    context.keyword = m_projects->data(
        m_projects->index(row, TableProjects::IND_KEYWORD)).toString().trimmed();
    context.hookIdea = m_projects->data(
        m_projects->index(row, TableProjects::IND_HOOK)).toString().trimmed();
    context.videoIdea = savedVideoIdea;
    context.videoFormatLabel = m_runVideoFormatLabel.isEmpty()
        ? QStringLiteral("9:16 vertical") : m_runVideoFormatLabel;
    context.preferredHashtags = m_hashtags ? m_hashtags->hashtags() : QStringList{};
    context.preferredHashtagsModel = m_hashtags;
    context.favoriteHooksPrompt = m_favoriteHooks ? m_favoriteHooks->asPromptSection() : QString{};
    context.favoriteHooks = m_favoriteHooks ? m_favoriteHooks->hooks() : QStringList{};
    context.codeTag = codeTagFromShortCode(shortCode);
    context.workingDir = tempDir.absolutePath();
    context.availableClis = m_availableClis;
    context.selectedCli = _promptCli();

    if (!shortCode.isEmpty())
    {
        const auto *record = m_videos->recordFromShortCode(shortCode);
        const auto promptEntries = generationPromptEntries(row, record);
        QStringList stepPrompts;
        for (const auto &entry : promptEntries)
        {
            if (!entry.prompt.isEmpty() && !stepPrompts.contains(entry.prompt))
            {
                stepPrompts << entry.prompt;
            }
        }
        if (!stepPrompts.isEmpty())
        {
            context.videoPrompt = stepPrompts.join(QStringLiteral(" | "));
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
        saveHooksFile(tempDir, dialog.allHooks(), shortCode, dialog.videoIdea());
        saveHooksFile(genDir, dialog.allHooks(), shortCode, dialog.videoIdea());
        _refreshHooksView(tempDir);
    }
    else
    {
        saveHooksFile(m_projects->stagingDir(row), dialog.allHooks(), QString{}, dialog.videoIdea());
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
    m_progressDlg->setMaximumWidth(1600);
    m_progressDlg->resize(760, 480);
    if (m_progress.status)
    {
        m_progress.status->setWordWrap(true);
        m_progress.status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    if (m_progress.cancelBtn)
    {
        connect(m_progress.cancelBtn, &QPushButton::clicked, m_progressDlg, [this]() {
            m_batchCancelled = true;
            if (m_activeMultiVideoWorkflow)
            {
                m_activeMultiVideoWorkflow->cancel();
            }
            _stopVideoGeneratorsIfConfigured();
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
        const QString compact = message.simplified();
        m_progress.status->setText(
            compact.size() > 180 ? (compact.left(179) + QStringLiteral("…")) : compact);
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

void PaneGeneration::_stopVideoGeneratorsIfConfigured()
{
    const auto &generators = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    for (auto it = generators.constBegin(); it != generators.constEnd(); ++it)
    {
        const QString genId = it.key();
        AbstractVideoGenerator *gen = it.value();
        if (!gen)
        {
            continue;
        }
        const bool shouldClose = m_generationSettings
            ? m_generationSettings->settingsFor(genId)
                  .value(QStringLiteral("closeBrowserOnFinish"), true)
                  .toBool()
            : true;
        if (shouldClose)
        {
            gen->stopWorker();
        }
    }
}

QList<PaneGeneration::GenerationPromptEntry> PaneGeneration::generationPromptEntries(
    int row, const TableVideos::VideoRecord *record) const
{
    QList<GenerationPromptEntry> entries;
    if (row < 0 || !record)
    {
        return entries;
    }

    const QDir genDir = m_projects->generationDir(row, record->shortCode, false);
    const QDir tempDir = m_projects->generationTempDir(row, record->shortCode);
    const QString manifestName = QStringLiteral("multi_video_workflow.json");

    // 1. Check for multi-video workflow manifest
    ProductShowcaseMultiVideoWorkflow workflow;
    bool workflowLoaded = false;
    if (genDir.exists(manifestName))
    {
        workflowLoaded = workflow.loadFromFile(genDir.absoluteFilePath(manifestName));
    }
    if (!workflowLoaded && tempDir.exists(manifestName))
    {
        workflowLoaded = workflow.loadFromFile(tempDir.absoluteFilePath(manifestName));
    }
    if (!workflowLoaded)
    {
        const QString projGenManifest
            = m_projects->projectGenerationsDir(row).absoluteFilePath(manifestName);
        if (QFile::exists(projGenManifest) && workflow.loadFromFile(projGenManifest)
            && workflow.generationShortCode() == record->shortCode)
        {
            workflowLoaded = true;
        }
    }
    if (!workflowLoaded)
    {
        const QString projManifest
            = m_projects->projectDir(row).absoluteFilePath(manifestName);
        if (QFile::exists(projManifest) && workflow.loadFromFile(projManifest)
            && workflow.generationShortCode() == record->shortCode)
        {
            workflowLoaded = true;
        }
    }

    if (workflowLoaded)
    {
        const auto &steps = workflow.steps();
        for (int i = 0; i < steps.size(); ++i)
        {
            const auto &step = steps[i];
            const QString prompt = step.prompt.trimmed();
            if (prompt.isEmpty())
            {
                continue;
            }
            QString title = step.title.trimmed();
            if (title.isEmpty())
            {
                title = (step.type == AbstractMultiVideoWorkflow::StepType::Intro)
                    ? tr("Intro")
                    : tr("Step %1").arg(i + 1);
            }
            entries.append({title, prompt});
        }
        if (!entries.isEmpty())
        {
            return entries;
        }
    }

    auto cleanFormatSuffix = [](QString p) {
        p = PromptLessons::stripPromptSection(p);
        static const QRegularExpression formatRe{QStringLiteral("\\s*Video format:.*$")};
        const QString cleaned = p.remove(formatRe).trimmed();
        return cleaned.isEmpty() ? p.trimmed() : cleaned;
    };

    // 2. Single video / image / slideshow prompt from generation_prompt.txt
    QString promptText;
    QFile promptFile{tempDir.filePath(QStringLiteral("generation_prompt.txt"))};
    if (!promptFile.exists())
    {
        promptFile.setFileName(genDir.filePath(QStringLiteral("generation_prompt.txt")));
    }
    if (promptFile.open(QFile::ReadOnly))
    {
        promptText = QString::fromUtf8(promptFile.readAll()).trimmed();
    }

    // 3. Fallback to generation_config.json (VideoGenerationRecipe)
    if (promptText.isEmpty())
    {
        VideoGenerationRecipe recipe;
        QString err;
        if (tempDir.exists(QStringLiteral("generation_config.json"))
            && VideoGenerationRecipe::load(tempDir, &recipe, &err))
        {
            promptText = recipe.prompt.trimmed();
        }
        else if (genDir.exists(QStringLiteral("generation_config.json"))
                 && VideoGenerationRecipe::load(genDir, &recipe, &err))
        {
            promptText = recipe.prompt.trimmed();
        }
    }

    // 4. Fallback to project's stored IND_GEN_PROMPT if still empty
    if (promptText.isEmpty())
    {
        promptText = m_projects->data(
            m_projects->index(row, TableProjects::IND_GEN_PROMPT)).toString().trimmed();
    }

    if (!promptText.isEmpty())
    {
        entries.append({tr("Prompt"), cleanFormatSuffix(promptText)});
    }

    return entries;
}

static QString withSentencePunctuation(const QString &text)
{
    if (text.isEmpty())
    {
        return text;
    }
    const QChar last = text.at(text.length() - 1);
    if (last == QLatin1Char('.') || last == QLatin1Char('!') || last == QLatin1Char('?'))
    {
        return text;
    }
    return text + QLatin1Char('.');
}

QString PaneGeneration::appendExtraPromptPhrase(const QString &basePrompt, const QString &extraPhrase)
{
    const QString extra = extraPhrase.trimmed();
    if (extra.isEmpty())
    {
        return basePrompt;
    }

    const QString punctuatedExtra = withSentencePunctuation(extra);

    static const QRegularExpression formatRe{QStringLiteral("(\\s*Video format:.*$)")};
    const auto match = formatRe.match(basePrompt);
    if (match.hasMatch())
    {
        const int formatPos = match.capturedStart(1);
        QString mainPart = basePrompt.left(formatPos).trimmed();
        const QString formatPart = basePrompt.mid(formatPos);

        mainPart = withSentencePunctuation(mainPart);
        if (!mainPart.isEmpty())
        {
            mainPart += QLatin1Char(' ');
        }
        mainPart += punctuatedExtra;
        return mainPart + formatPart;
    }

    QString prompt = withSentencePunctuation(basePrompt.trimmed());
    if (!prompt.isEmpty())
    {
        prompt += QLatin1Char(' ');
    }
    prompt += punctuatedExtra;
    return prompt;
}


void PaneGeneration::_generationsContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = ui->treeViewGenerations->itemAt(position);
    if (!item)
    {
        return;
    }
    QMenu menu{this};

    if (item->data(0, RoleIsPromptItem).toBool())
    {
        const QString promptText = item->data(0, RolePromptText).toString();
        const QString label = item->data(0, RolePromptLabel).toString();
        QAction *actionCopy = menu.addAction(tr("Copy prompt"));
        connect(actionCopy, &QAction::triggered, this, [this, promptText]() {
            QGuiApplication::clipboard()->setText(promptText);
            _logProgress(tr("Copied prompt to clipboard."));
        });

        QAction *actionCopyFormatted = menu.addAction(
            tr("Copy with label (\"%1: ...\")").arg(label));
        const QString fullText = item->text(0);
        connect(actionCopyFormatted, &QAction::triggered, this, [this, fullText]() {
            QGuiApplication::clipboard()->setText(fullText);
            _logProgress(tr("Copied prompt to clipboard."));
        });

        if (m_favoriteVideoPrompts)
        {
            menu.addSeparator();
            QAction *actionFavorite = menu.addAction(tr("★ Favorite this video prompt"));
            connect(actionFavorite, &QAction::triggered, this, [this, promptText]() {
                m_favoriteVideoPrompts->addPrompt(promptText);
                _logProgress(tr("Saved as a favorite video prompt: %1")
                    .arg(promptText.left(150)));
            });
        }
    }
    else if (!item->parent())
    {
        // Top-level item
        QStringList prompts;
        for (int i = 0; i < item->childCount(); ++i)
        {
            QTreeWidgetItem *child = item->child(i);
            if (child->data(0, RoleIsPromptItem).toBool())
            {
                const QString pt = child->data(0, RolePromptText).toString();
                if (!pt.isEmpty())
                {
                    prompts << pt;
                }
            }
        }
        if (!prompts.isEmpty())
        {
            if (prompts.size() == 1)
            {
                const QString single = prompts.first();
                QAction *actionCopy = menu.addAction(tr("Copy prompt"));
                connect(actionCopy, &QAction::triggered, this, [this, single]() {
                    QGuiApplication::clipboard()->setText(single);
                    _logProgress(tr("Copied prompt to clipboard."));
                });
            }
            else
            {
                const QString joined = prompts.join(QStringLiteral("\n\n"));
                QAction *actionCopyAll = menu.addAction(
                    tr("Copy all prompts (%1)").arg(prompts.size()));
                connect(actionCopyAll, &QAction::triggered, this, [this, joined]() {
                    QGuiApplication::clipboard()->setText(joined);
                    _logProgress(tr("Copied prompts to clipboard."));
                });
            }
        }
        menu.addSeparator();
        QAction *actionHooks = menu.addAction(tr("Hooks..."));
        connect(actionHooks, &QAction::triggered, this, &PaneGeneration::_openHooksDialog);
    }
    else
    {
        // Property child item
        const QString text = item->text(0);
        QAction *actionCopy = menu.addAction(tr("Copy"));
        connect(actionCopy, &QAction::triggered, this, [text]() {
            QGuiApplication::clipboard()->setText(text);
        });
    }

    menu.exec(ui->treeViewGenerations->viewport()->mapToGlobal(position));
}

void PaneGeneration::_copySelectedGenerationPrompt()
{
    QTreeWidgetItem *current = ui->treeViewGenerations->currentItem();
    if (!current)
    {
        return;
    }
    if (current->data(0, RoleIsPromptItem).toBool())
    {
        const QString text = current->data(0, RolePromptText).toString();
        if (!text.isEmpty())
        {
            QGuiApplication::clipboard()->setText(text);
            _logProgress(tr("Copied prompt to clipboard."));
        }
        return;
    }

    // Top-level item: copy all prompt items
    QTreeWidgetItem *topItem = current->parent() ? current->parent() : current;
    QStringList prompts;
    for (int i = 0; i < topItem->childCount(); ++i)
    {
        QTreeWidgetItem *child = topItem->child(i);
        if (child->data(0, RoleIsPromptItem).toBool())
        {
            const QString pt = child->data(0, RolePromptText).toString();
            if (!pt.isEmpty())
            {
                prompts << pt;
            }
        }
    }
    if (!prompts.isEmpty())
    {
        QGuiApplication::clipboard()->setText(prompts.join(QStringLiteral("\n\n")));
        _logProgress(tr("Copied prompt(s) to clipboard."));
    }
    else
    {
        QGuiApplication::clipboard()->setText(current->text(0));
    }
}

namespace {

QJsonObject parseFirstJsonObject(const QString &raw)
{
    const qsizetype first = raw.indexOf(QLatin1Char('{'));
    const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
    if (first < 0 || last <= first)
    {
        return QJsonObject{};
    }
    return QJsonDocument::fromJson(raw.mid(first, last - first + 1).toUtf8()).object();
}

void overlayHookTextOnSlide(const QString &imagePath, const QString &hookText)
{
    const QString text = hookText.trimmed();
    if (imagePath.isEmpty() || text.isEmpty())
    {
        return;
    }
    QImage img{imagePath};
    if (img.isNull() || img.width() < 64 || img.height() < 64)
    {
        return;
    }
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter{&img};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter.font();
    font.setPixelSize(qMax(18, img.width() / 16));
    font.setBold(true);
    painter.setFont(font);

    const int marginX = qMax(16, img.width() / 10);
    const int maxTextW = img.width() - 2 * marginX;
    const QRect textBounds = painter.fontMetrics().boundingRect(
        QRect(0, 0, maxTextW, img.height() / 2),
        Qt::AlignCenter | Qt::TextWordWrap,
        text);
    const int padX = qMax(14, img.width() / 28);
    const int padY = qMax(10, img.height() / 55);
    const int boxW = qMin(img.width() - 24, textBounds.width() + 2 * padX);
    const int boxH = textBounds.height() + 2 * padY;
    const QRect pillRect((img.width() - boxW) / 2,
                         qMax(20, img.height() / 7),
                         boxW,
                         boxH);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 18, 24, 195));
    painter.drawRoundedRect(pillRect, 14.0, 14.0);

    painter.setPen(QColor(255, 255, 255));
    painter.drawText(pillRect.adjusted(padX, padY, -padX, -padY),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     text);
    painter.end();
    img.save(imagePath, "PNG");
}

QStringList extractMediaFramesIntoDir(const QString &mediaPath,
                                      const QDir &targetDir,
                                      const QString &prefix,
                                      QString *outOpeningSceneName)
{
    if (outOpeningSceneName)
    {
        outOpeningSceneName->clear();
    }
    if (mediaPath.isEmpty() || !QFileInfo::exists(mediaPath))
    {
        return {};
    }
    const QString suffix = QFileInfo{mediaPath}.suffix().toLower();
    const bool isVid = (suffix == QLatin1String("mp4")
                        || suffix == QLatin1String("mov")
                        || suffix == QLatin1String("webm"));
    const QString sceneName = prefix + QStringLiteral("_scene.png");
    const QString sceneDest = targetDir.absoluteFilePath(sceneName);
    QFile::remove(sceneDest);

    if (!isVid)
    {
        QImage img{mediaPath};
        if (!img.isNull() && img.save(sceneDest, "PNG"))
        {
            if (outOpeningSceneName) *outOpeningSceneName = sceneName;
            return {sceneName};
        }
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (!ffmpeg.isEmpty())
        {
            QProcess proc;
            proc.start(ffmpeg, {
                QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                QStringLiteral("-i"), mediaPath,
                QStringLiteral("-frames:v"), QStringLiteral("1"),
                sceneDest
            });
            if (proc.waitForFinished(8'000) && QFileInfo::exists(sceneDest) && !QPixmap{sceneDest}.isNull())
            {
                if (outOpeningSceneName) *outOpeningSceneName = sceneName;
                return {sceneName};
            }
        }
        return {};
    }

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
    {
        return {};
    }

    double durationSec = 0.0;
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (!ffprobe.isEmpty())
    {
        QProcess probe;
        probe.start(ffprobe, {
            QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
            QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
            mediaPath
        });
        if (probe.waitForFinished(5'000) && probe.exitCode() == 0)
        {
            durationSec = QString::fromUtf8(probe.readAllStandardOutput()).trimmed().toDouble();
        }
    }

    const auto extractAt = [&](double sec, const QString &destPath) -> bool {
        QFile::remove(destPath);
        QProcess proc;
        QStringList args{QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error")};
        if (sec > 0.02)
        {
            args << QStringLiteral("-ss") << QString::number(sec, 'f', 2);
        }
        args << QStringLiteral("-i") << mediaPath
             << QStringLiteral("-frames:v") << QStringLiteral("1")
             << destPath;
        proc.start(ffmpeg, args);
        return proc.waitForFinished(8'000) && QFileInfo::exists(destPath) && !QPixmap{destPath}.isNull();
    };

    const double openSec = durationSec > 0.4 ? qMin(0.20, durationSec * 0.10) : 0.0;
    if (!extractAt(openSec, sceneDest))
    {
        extractAt(0.0, sceneDest);
    }
    if (!QFileInfo::exists(sceneDest))
    {
        return {};
    }
    if (outOpeningSceneName)
    {
        *outOpeningSceneName = sceneName;
    }

    QStringList frames;
    if (durationSec >= 0.8)
    {
        const QList<double> ratios{0.08, 0.40, 0.72, 0.92};
        for (int i = 0; i < ratios.size(); ++i)
        {
            const QString fname = QStringLiteral("%1_frame_%2.png").arg(prefix).arg(i + 1);
            if (extractAt(durationSec * ratios[i], targetDir.absoluteFilePath(fname)))
            {
                frames << fname;
            }
        }
    }
    if (frames.isEmpty())
    {
        frames << sceneName;
    }
    return frames;
}

bool recoverCliOutputFile(AbstractCli *cli,
                          const QDateTime &callStart,
                          const QString &outputFileName,
                          const QString &targetPath)
{
    if (QFileInfo::exists(targetPath))
    {
        return true;
    }
    if (!cli)
    {
        return false;
    }
    for (const QString &fallbackDir : cli->outputFallbackDirs())
    {
        const QString fallbackPath = QDir{fallbackDir}.absoluteFilePath(outputFileName);
        const QFileInfo fallbackInfo{fallbackPath};
        if (!fallbackInfo.exists() || fallbackInfo.lastModified() < callStart)
        {
            continue;
        }
        QFile::remove(targetPath);
        if (QFile::rename(fallbackPath, targetPath)
            || (QFile::copy(fallbackPath, targetPath) && QFile::remove(fallbackPath)))
        {
            return true;
        }
    }
    return false;
}

} // namespace

int PaneGeneration::_findOrCreateAutoBatchProject(const QString &accountId,
                                                  const QString &accountLabel,
                                                  const QJsonObject &profile,
                                                  const QJsonObject &product,
                                                  const QJsonObject &primaryReference,
                                                  const QString &primaryRefMediaPath)
{
    const QString productId = product.value(QStringLiteral("id")).toString();
    const QString imagePath = product.value(QStringLiteral("localImage")).toString();
    if (productId.isEmpty() || imagePath.isEmpty() || !QFileInfo::exists(imagePath))
    {
        return -1;
    }

    for (int r = 0; r < m_projects->rowCount(); ++r)
    {
        QFile ctxFile{m_projects->projectDir(r).filePath(QStringLiteral("research_context.json"))};
        if (!ctxFile.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QJsonObject ctx = QJsonDocument::fromJson(ctxFile.readAll()).object();
        if (ctx.value(QStringLiteral("accountId")).toString() == accountId
            && ctx.value(QStringLiteral("product")).toObject().value(QStringLiteral("id")).toString() == productId)
        {
            return r;
        }
    }

    if (!primaryReference.isEmpty()
        && primaryReference.value(QStringLiteral("status")).toString() == QLatin1String("approved"))
    {
        QJsonObject context{
            {QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("accountId"), accountId},
            {QStringLiteral("accountLabel"), accountLabel},
            {QStringLiteral("profile"), profile},
            {QStringLiteral("product"), product},
            {QStringLiteral("reference"), primaryReference},
            {QStringLiteral("referenceMediaPath"), primaryRefMediaPath}
        };
        QString err;
        const int createdRow = ResearchProject::create(*m_projects, context, imagePath, &err);
        if (createdRow >= 0)
        {
            return createdRow;
        }
    }

    const int row = m_projects->addProject(
        imagePath, {}, seoKeywordForAutoProduct(product), {});
    if (row < 0)
    {
        return -1;
    }
    const QString color = product.value(QStringLiteral("color")).toString().trimmed();
    const QString prodTitle = product.value(QStringLiteral("title")).toString().trimmed();
    const QString projectName = (!color.isEmpty() && !prodTitle.contains(color, Qt::CaseInsensitive))
        ? (color + QStringLiteral(" — ") + prodTitle).left(100)
        : prodTitle.left(100);
    m_projects->setData(m_projects->index(row, TableProjects::IND_NAME), projectName);

    QJsonObject ctx{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("accountId"), accountId},
        {QStringLiteral("accountLabel"), accountLabel},
        {QStringLiteral("profile"), profile},
        {QStringLiteral("product"), product},
        {QStringLiteral("projectId"), m_projects->projectId(row).toString(QUuid::WithoutBraces)}
    };
    const QByteArray bytes = QJsonDocument{ctx}.toJson();
    QSaveFile out{m_projects->projectDir(row).filePath(QStringLiteral("research_context.json"))};
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(bytes);
        out.commit();
    }
    return row;
}

bool PaneGeneration::startAutoBatch(const QJsonObject &batchSpec)
{
    if (!_promptCli())
    {
        QMessageBox::information(this, tr("No CLI available"),
            tr("No AI CLI was found on this machine — install one (claude, codex, ...) and restart."));
        return false;
    }
    if (!_imageCli())
    {
        QMessageBox::information(this, tr("No image CLI available"),
            tr("Auto-batch requires an image-capable AI CLI to generate and swap product scenes."));
        return false;
    }
    const QJsonArray products = batchSpec.value(QStringLiteral("products")).toArray();
    const bool incImg = batchSpec.value(QStringLiteral("includeImage")).toBool(false);
    const bool incCar = batchSpec.value(QStringLiteral("includeCarousel")).toBool(false);
    const bool incVid = batchSpec.value(QStringLiteral("includeVideo")).toBool(false);
    if (products.isEmpty() || (!incImg && !incCar && !incVid))
    {
        return false;
    }

    ui->toolBox->setCurrentIndex(1);
    m_autoBatchTask = std::make_unique<QCoro::Task<void>>(_runAutoBatch(batchSpec));
    return true;
}

QCoro::Task<void> PaneGeneration::_runAutoBatch(QJsonObject batchSpec)
{
    AbstractCli *promptCli = _promptCli();
    AbstractCli *imgCli = _imageCli();
    if (!promptCli || !imgCli)
    {
        co_return;
    }

    const QString accountId = batchSpec.value(QStringLiteral("accountId")).toString();
    const QString accountLabel = batchSpec.value(QStringLiteral("accountLabel")).toString();
    const QJsonObject profile = batchSpec.value(QStringLiteral("profile")).toObject();
    const QJsonArray products = batchSpec.value(QStringLiteral("products")).toArray();
    const QJsonArray rawApprovedRefs = batchSpec.value(QStringLiteral("approvedReferences")).toArray();
    const int generationsPerProduct = qMax(1, batchSpec.value(QStringLiteral("generationsPerProduct")).toInt(10));
    const bool includeImage = batchSpec.value(QStringLiteral("includeImage")).toBool(true);
    const bool includeCarousel = batchSpec.value(QStringLiteral("includeCarousel")).toBool(true);
    const bool includeVideo = batchSpec.value(QStringLiteral("includeVideo")).toBool(true);
    const bool carouselHookOverlay = batchSpec.value(QStringLiteral("carouselHookOverlay")).toBool(false);
    const QString videoFormatLabel = batchSpec.value(QStringLiteral("videoFormat"))
        .toString(QStringLiteral("vertical (9:16)"));
    QString videoGeneratorId = batchSpec.value(QStringLiteral("videoGeneratorId")).toString();
    if (videoGeneratorId.isEmpty())
    {
        const auto &allGens = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
        if (allGens.contains(QStringLiteral("gemini-browser")))
        {
            videoGeneratorId = QStringLiteral("gemini-browser");
        }
        else if (!allGens.isEmpty())
        {
            videoGeneratorId = allGens.firstKey();
        }
    }

    QList<GenerationJob::Kind> enabledFormats;
    if (includeImage) enabledFormats << GenerationJob::Image;
    if (includeCarousel) enabledFormats << GenerationJob::Slideshow;
    if (includeVideo && !videoGeneratorId.isEmpty()) enabledFormats << GenerationJob::Video;
    if (enabledFormats.isEmpty())
    {
        co_return;
    }

    // Score approved references AND adaptation modes ("reference_swap" = strict product swap vs
    // "reference_variant" = product + subtle scene/style variation vs "reference_reimagine")
    // using real published post statistics from SocialStatistics across both current & legacy directories.
    const auto refExp = SocialStatistics::summarizeReferenceExperiment(m_videos);
    QHash<QString, SocialStatistics::ReferencePerformance> refPerfById;
    for (const auto &rp : refExp.references)
    {
        refPerfById.insert(rp.referenceId, rp);
    }

    struct RankedRef
    {
        QJsonObject reference;
        QString mediaPath;
        double measuredMedian = -1.0;
        bool isMeasuredWinner = false;
        QString bestStrategy;
        int swapCount = 0;
        int variantCount = 0;
        int originalRank = 0;
    };
    QList<RankedRef> rankedRefs;
    for (int i = 0; i < rawApprovedRefs.size(); ++i)
    {
        const QJsonObject item = rawApprovedRefs[i].toObject();
        const QJsonObject ref = item.value(QStringLiteral("reference")).toObject();
        const QString refId = ref.value(QStringLiteral("id")).toString();
        RankedRef rr;
        rr.reference = ref;
        rr.mediaPath = item.value(QStringLiteral("mediaPath")).toString();
        rr.originalRank = i;
        if (refPerfById.contains(refId))
        {
            const auto &rp = refPerfById[refId];
            rr.measuredMedian = rp.medianScore;
            rr.isMeasuredWinner = rp.isWinner;
            rr.bestStrategy = rp.bestStrategy;
            rr.swapCount = rp.swapCount;
            rr.variantCount = rp.variantCount;
        }
        rankedRefs.append(rr);
    }
    std::stable_sort(rankedRefs.begin(), rankedRefs.end(),
                     [](const RankedRef &a, const RankedRef &b) {
                         if (a.isMeasuredWinner != b.isMeasuredWinner)
                         {
                             return a.isMeasuredWinner;
                         }
                         if (a.isMeasuredWinner && a.measuredMedian != b.measuredMedian)
                         {
                             return a.measuredMedian > b.measuredMedian;
                         }
                         if ((a.measuredMedian < 0.0) != (b.measuredMedian < 0.0))
                         {
                             // Untested high-velocity approved refs come ahead of proven below-median refs
                             return a.measuredMedian < 0.0;
                         }
                         return a.originalRank < b.originalRank;
                     });

    QList<int> winnerRefIndices;
    QList<int> freshPoolIndices;
    for (int i = 0; i < rankedRefs.size(); ++i)
    {
        if (rankedRefs[i].isMeasuredWinner)
        {
            winnerRefIndices.append(i);
        }
        else
        {
            freshPoolIndices.append(i);
        }
    }
    int winnerCursor = 0;
    int freshCursor = 0;
    QHash<QString, int> globalRefUseCounts;

    const int totalCreatives = products.size() * generationsPerProduct;
    int completedCreatives = 0;
    int attemptedCreatives = 0;
    m_runGenerations.clear();
    m_runVideoFormatLabel = videoFormatLabel;

    _openProgress();
    if (m_progress.cancelBtn)
    {
        m_progress.cancelBtn->show();
    }
    if (m_progress.bar)
    {
        m_progress.bar->setRange(0, totalCreatives);
        m_progress.bar->setValue(0);
    }

    setEnabled(false);
    const auto restoreUi = qScopeGuard([this]() {
        setEnabled(true);
        _stopVideoGeneratorsIfConfigured();
    });

    _logProgress(tr("Starting Auto-Batch: %1 product(s) × %2 generation(s) = %3 creative(s) "
                    "(%4 approved reference(s) in pool).")
        .arg(products.size())
        .arg(generationsPerProduct)
        .arg(totalCreatives)
        .arg(rankedRefs.size()));

    bool hitHardStopError = false;
    QString hardStopReason;

    for (int pIdx = 0; pIdx < products.size() && !m_batchCancelled && !hitHardStopError; ++pIdx)
    {
        const QJsonObject product = products[pIdx].toObject();
        const QString productId = product.value(QStringLiteral("id")).toString();
        const QString productTitle = product.value(QStringLiteral("title")).toString().trimmed();
        const QString productColor = product.value(QStringLiteral("color")).toString().trimmed();
        const QString productBrand = product.value(QStringLiteral("brand")).toString().trimmed();
        QString productCategory = seoKeywordForAutoProduct(product);
        const QString rawProdImg = product.value(QStringLiteral("localImage")).toString();

        const QJsonObject primaryRef = rankedRefs.isEmpty() ? QJsonObject{} : rankedRefs.first().reference;
        const QString primaryRefMedia = rankedRefs.isEmpty() ? QString{} : rankedRefs.first().mediaPath;
        const int row = _findOrCreateAutoBatchProject(
            accountId, accountLabel, profile, product, primaryRef, primaryRefMedia);
        if (row < 0)
        {
            _logProgress(tr("[Product %1/%2] Skipped %3: missing readable product image.")
                .arg(pIdx + 1).arg(products.size()).arg(productTitle));
            continue;
        }
        const QString existingKw = m_projects->data(
            m_projects->index(row, TableProjects::IND_KEYWORD)).toString().trimmed();
        if (existingKw.isEmpty() && !productCategory.isEmpty())
        {
            m_projects->setData(m_projects->index(row, TableProjects::IND_KEYWORD), productCategory);
        }
        else if (productCategory.isEmpty() && !existingKw.isEmpty())
        {
            productCategory = existingKw;
        }
        ui->tableViewProjects->setCurrentIndex(m_projects->index(row, TableProjects::IND_NAME));
        const QUuid projectId = m_projects->projectId(row);
        const QString projectProductImg = m_projects->absoluteImagePath(row);

        QStringList prodSummaryParts;
        if (!productBrand.isEmpty()) prodSummaryParts << productBrand;
        if (!productColor.isEmpty()) prodSummaryParts << productColor;
        if (!productTitle.isEmpty()) prodSummaryParts << productTitle;
        const QString productSummary = prodSummaryParts.join(QStringLiteral(" · "));

        // Build N diverse slots for this product (80% exploit / 20% explore)
        const int exploreCount = generationsPerProduct >= 4
            ? qMax(1, generationsPerProduct / 5)
            : 0;
        const int exploitCount = generationsPerProduct - exploreCount;
        const auto sampledVariants = PropertySampler::sampleVariants(
            *m_properties, *m_videos, generationsPerProduct);

        m_projects->resetStagingDir(row);
        const QDir baseStaging = m_projects->stagingDir(row);
        const QDir planDir{baseStaging.absoluteFilePath(QStringLiteral("auto_plan"))};
        QDir{}.mkpath(planDir.absolutePath());

        const QString prodExt = QFileInfo{projectProductImg}.suffix().isEmpty()
            ? QStringLiteral("jpg")
            : QFileInfo{projectProductImg}.suffix().toLower();
        const QString planProdFile = QStringLiteral("my_product.%1").arg(prodExt);
        QFile::copy(projectProductImg, planDir.absoluteFilePath(planProdFile));

        struct PlannedSlot
        {
            GenerationJob::Kind kind = GenerationJob::Video;
            QString formatName;
            const InspirationAdaptationMethod *method = nullptr;
            QString strategy;
            bool isExploration = false;
            QJsonObject reference;
            QString referenceMediaPath;
            QStringList stagedPlanRefFrames;
            QList<PropertySampler::SampledValue> sampledProps;
            QList<QUuid> keptValueIds;
            QString prompt;
            QString hook;
            QString description;
            QString hookCategory;
            QStringList slideTexts;
        };

        QList<PlannedSlot> slotsForProduct;
        QHash<QString, QStringList> cachedPlanFramesByRefId;
        QHash<QString, int> refUseCounts;

        for (int s = 0; s < generationsPerProduct; ++s)
        {
            PlannedSlot slot;
            slot.kind = enabledFormats[s % enabledFormats.size()];
            slot.formatName = slot.kind == GenerationJob::Video
                ? QStringLiteral("video")
                : (slot.kind == GenerationJob::Slideshow
                       ? QStringLiteral("carousel")
                       : QStringLiteral("image"));
            slot.isExploration = (s >= exploitCount);

            bool hasRefMedia = false;
            int totalPriorForRef = 0;
            QString refBestMethodId;

            if (!rankedRefs.isEmpty())
            {
                int chosenRefIdx = 0;
                if (!slot.isExploration)
                {
                    if (!winnerRefIndices.isEmpty() && (freshPoolIndices.isEmpty() || (s % 2 == 0)))
                    {
                        chosenRefIdx = winnerRefIndices[winnerCursor % winnerRefIndices.size()];
                        ++winnerCursor;
                    }
                    else if (!freshPoolIndices.isEmpty())
                    {
                        chosenRefIdx = freshPoolIndices[freshCursor % freshPoolIndices.size()];
                        ++freshCursor;
                    }
                    else
                    {
                        chosenRefIdx = s % rankedRefs.size();
                    }
                }
                else
                {
                    if (!freshPoolIndices.isEmpty())
                    {
                        chosenRefIdx = freshPoolIndices[freshCursor % freshPoolIndices.size()];
                        ++freshCursor;
                    }
                    else
                    {
                        chosenRefIdx = int(rankedRefs.size()) - 1 - ((s - exploitCount) % rankedRefs.size());
                    }
                }
                const RankedRef &picked = rankedRefs[chosenRefIdx];
                slot.reference = picked.reference;
                slot.referenceMediaPath = picked.mediaPath;
                const QString rid = picked.reference.value(QStringLiteral("id")).toString();
                const int priorUses = refUseCounts.value(rid, 0);
                refUseCounts[rid] = priorUses + 1;
                const int globalUses = globalRefUseCounts.value(rid, 0);
                globalRefUseCounts[rid] = globalUses + 1;

                hasRefMedia = !slot.referenceMediaPath.isEmpty()
                    && QFileInfo::exists(slot.referenceMediaPath);
                totalPriorForRef = picked.swapCount + picked.variantCount + globalUses;
                if (picked.isMeasuredWinner)
                {
                    refBestMethodId = picked.bestStrategy;
                }

                if (hasRefMedia && cachedPlanFramesByRefId.size() < 8 && !cachedPlanFramesByRefId.contains(rid))
                {
                    QString openName;
                    const QString prefix = QStringLiteral("ref_%1").arg(cachedPlanFramesByRefId.size() + 1);
                    cachedPlanFramesByRefId.insert(
                        rid,
                        extractMediaFramesIntoDir(slot.referenceMediaPath, planDir, prefix, &openName));
                }
                slot.stagedPlanRefFrames = cachedPlanFramesByRefId.value(rid);
            }

            slot.method = InspirationAdaptationMethod::selectForSlot(
                hasRefMedia,
                s,
                totalPriorForRef,
                slot.isExploration,
                refBestMethodId,
                refExp.winningStrategy);
            slot.strategy = slot.method ? slot.method->id() : QStringLiteral("editorial_ab");

            if (!slot.method || slot.method->usesSceneProperties())
            {
                slot.sampledProps = sampledVariants.value(s);
                for (const auto &sp : slot.sampledProps)
                {
                    slot.keptValueIds << sp.id;
                }
            }
            slotsForProduct << slot;
        }

        // Single async AI planning call for all N slots of this product
        const QString planFileName = QStringLiteral("auto_plan.json");
        QFile::remove(planDir.absoluteFilePath(planFileName));
        const bool catalogWasEmpty = m_properties->topLevelNodes().isEmpty();

        QString planPrompt = QStringLiteral(
            "Plan an unattended creative batch of %1 social-media post(s) in %2 format.\n"
            "Product: %3\n"
            "Product photo in working directory: '%4'\n")
            .arg(generationsPerProduct)
            .arg(videoFormatLabel, productSummary, planProdFile);
        if (!productCategory.isEmpty())
        {
            planPrompt += QStringLiteral("Target SEO keyword: %1\n").arg(productCategory);
        }
        planPrompt += QStringLiteral(
            "Style rules: tasteful, brand-safe fashion-editorial — NEVER hide or shrink the product; "
            "keep the product accurate and prominent in every frame.\n\n"
            "Slots to plan:\n");
        for (int s = 0; s < slotsForProduct.size(); ++s)
        {
            const auto &sl = slotsForProduct[s];
            QStringList propFrags;
            for (const auto &sp : sl.sampledProps)
            {
                if (!sp.promptFragment.trimmed().isEmpty())
                {
                    propFrags << QStringLiteral("%1=%2 (%3)").arg(sp.propertyName, sp.valueName, sp.promptFragment);
                }
            }
            const QString refTitle = sl.reference.value(QStringLiteral("title")).toString();
            const QString refNotes = sl.reference.value(QStringLiteral("notes")).toString();
            InspirationAdaptationMethod::PromptContext planCtx;
            planCtx.productImageFile = planProdFile;
            planCtx.productSummary = productSummary;
            planCtx.referenceFrames = sl.stagedPlanRefFrames;
            planCtx.referenceTitle = refTitle;
            planCtx.adaptationNotes = refNotes;
            planCtx.videoFormatLabel = videoFormatLabel;

            planPrompt += QStringLiteral(
                "- Slot %1: format=%2 | method=%3 (%4)%5%6%7\n")
                .arg(s)
                .arg(sl.formatName,
                     sl.strategy,
                     sl.method ? sl.method->name() : QStringLiteral("Editorial"),
                     refTitle.isEmpty() ? QString{} : QStringLiteral(" | reference=\"%1\"").arg(refTitle),
                     refNotes.isEmpty() ? QString{} : QStringLiteral(" | notes=\"%1\"").arg(refNotes),
                     sl.stagedPlanRefFrames.isEmpty()
                         ? QString{}
                         : QStringLiteral(" | reference frames: %1").arg(sl.stagedPlanRefFrames.join(QStringLiteral(", "))));
            if (sl.method)
            {
                planPrompt += QStringLiteral("  Method rule: %1\n").arg(sl.method->plannerSlotInstruction(planCtx));
            }
            if (!propFrags.isEmpty() && (!sl.method || sl.method->usesSceneProperties()))
            {
                planPrompt += QStringLiteral("  A/B properties: %1\n").arg(propFrags.join(QStringLiteral("; ")));
            }
        }
        if (m_hashtags)
        {
            planPrompt += m_hashtags->viralHashtagPromptSection(productCategory);
        }
        if (m_favoriteHooks)
        {
            planPrompt += m_favoriteHooks->asPromptSection();
        }
        if (m_favoriteVideoPrompts)
        {
            planPrompt += m_favoriteVideoPrompts->asPromptSection();
        }
        planPrompt += buildMeasuredWinnersPromptSection(m_projects, m_videos);
        planPrompt += refExp.plannerPromptAddendum();
        planPrompt += QStringLiteral(
            "VIRAL HOOK GUIDELINES (Maximize 3-Second Retention):\n"
            "Write 10 short, punchy hooks (< 8 words each) covering distinct psychological triggers:\n"
            "- Curiosity Gap / Mystery (e.g. \"The one thing nobody tells you about...\")\n"
            "- Pattern Interrupt / Contrarian (e.g. \"Stop doing this if you want...\")\n"
            "- Transformation / Before & After (e.g. \"Wait until you see how this transforms...\")\n"
            "- Relatable Social Proof / Secret Club (e.g. \"If you're obsessed with...\")\n"
            "- Direct Benefit / Value (e.g. \"The easiest way to style this look...\")\n");
        if (!productCategory.isEmpty())
        {
            const QString keywordHashtag = QStringLiteral("#")
                + QString{productCategory}.remove(QLatin1Char(' ')).toLower();
            planPrompt += QStringLiteral(
                "SEO — non-negotiable: the target keyword is \"%1\". EVERY description "
                "(both in \"slots\" and in \"hooks\") must include \"%1\" as plain readable text "
                "AND include %2 among its hashtags (at most 5 hashtags total).\n")
                .arg(productCategory, keywordHashtag);
        }
        const QString newPropsInstruction = catalogWasEmpty
            ? QStringLiteral(
                ", \"newProperties\": [{\"property\": \"...\", \"values\": "
                "[{\"name\": \"...\", \"fragment\": \"...\"}], \"used\": \"...\"}]")
            : QString{};
        planPrompt += QStringLiteral(
            "\nUsing your file tools, write ONE valid JSON object (no markdown fences) to '%1' "
            "in the current working directory with this exact shape:\n"
            "{\"slots\": [{\"slot\": 0, \"prompt\": \"...\", \"hook\": \"...\", "
            "\"description\": \"...\", \"hookCategory\": \"curiosity|contrarian|transformation|style_question|direct_value\", "
            "\"slideTexts\": [\"Slide 1 hook\", \"Slide 2 point\", \"Slide 3 detail\", \"Slide 4 CTA\"]}], "
            "\"hooks\": [{\"hook\": \"...\", \"description\": \"...\"}]%2}\n"
            "Guidelines:\n"
            "- \"hooks\": 10 catchy hook + post description pairs (each ending with up to 5 optimized hashtags) "
            "so the user also has 10 ready-to-pick alternatives in the Hooks dialog.\n"
            "- For format=video: start \"prompt\" with \"Generate a video: \" describing smooth motion "
            "starting from the regenerated frame (matching reference frames when mode=reference_swap, "
            "or fresh dynamic motion otherwise).\n"
            "- For format=carousel: describe a cohesive 4-slide story showcasing the exact product "
            "(opening hook pose, full outfit styling, close-up product detail, confident stride), "
            "and populate \"slideTexts\" with 4 short, punchy on-slide overlay captions (4-9 words each) "
            "that make viewers swipe from Slide 1 through Slide 4.\n"
            "- For format=image: describe one crisp, scroll-stopping editorial photo showcasing the exact product.\n"
            "After writing '%1', reply with: done. If you cannot write files, output the JSON object directly.")
            .arg(planFileName, newPropsInstruction);

        _logProgress(tr("[Product %1/%2 — %3] Planning %4 diverse creative(s) with %5...")
            .arg(pIdx + 1).arg(products.size()).arg(productSummary).arg(generationsPerProduct)
            .arg(promptCli->getName()));

        CliRunResult planResult;
        planResult = co_await promptCli->runPrompt(planPrompt, planDir.absolutePath());
        if (m_batchCancelled)
        {
            break;
        }
        if ((!planResult.processStarted || planResult.exitCode != 0)
            && promptCli->classifyError(planResult.errorOutput) != CliErrorKind::Other)
        {
            hitHardStopError = true;
            hardStopReason = friendlyCliError(promptCli, planResult);
            break;
        }

        QJsonObject planObj;
        QFile planFile{planDir.absoluteFilePath(planFileName)};
        if (planFile.open(QIODevice::ReadOnly))
        {
            planObj = QJsonDocument::fromJson(planFile.readAll()).object();
        }
        if (planObj.isEmpty())
        {
            planObj = parseFirstJsonObject(planResult.output);
        }

        // Auto-seed initial A/B properties when starting on a brand-new empty catalog
        if (catalogWasEmpty)
        {
            const QJsonArray newProps = planObj.value(QStringLiteral("newProperties")).toArray();
            for (const QJsonValue &npVal : newProps)
            {
                const QJsonObject npObj = npVal.toObject();
                const QString propName = npObj.value(QStringLiteral("property")).toString().trimmed();
                if (propName.isEmpty())
                {
                    continue;
                }
                const QModelIndex propIdx = m_properties->addProperty(propName, QStringLiteral("auto-bootstrap"));
                QList<QUuid> addedValIds;
                QList<QString> addedFragments;
                for (const QJsonValue &vEntry : npObj.value(QStringLiteral("values")).toArray())
                {
                    const QJsonObject vObj = vEntry.toObject();
                    const QString vName = vObj.value(QStringLiteral("name")).toString().trimmed();
                    const QString vFrag = vObj.value(QStringLiteral("fragment")).toString().trimmed();
                    if (!vName.isEmpty())
                    {
                        const QModelIndex valIdx = m_properties->addValue(
                            propIdx, vName, vFrag, QStringLiteral("auto-bootstrap"));
                        addedValIds << valIdx.data(TreeProperties::RoleId).toUuid();
                        addedFragments << vFrag;
                    }
                }
                if (!addedValIds.isEmpty())
                {
                    for (int s = 0; s < slotsForProduct.size(); ++s)
                    {
                        if (slotsForProduct[s].method && !slotsForProduct[s].method->usesSceneProperties())
                        {
                            continue;
                        }
                        const int pick = s % addedValIds.size();
                        slotsForProduct[s].keptValueIds << addedValIds[pick];
                        PropertySampler::SampledValue sv;
                        sv.id = addedValIds[pick];
                        sv.propertyName = propName;
                        sv.promptFragment = addedFragments[pick];
                        slotsForProduct[s].sampledProps << sv;
                    }
                }
            }
        }

        QList<QPair<QString, QString>> productHookPool;
        for (const QJsonValue &hv : planObj.value(QStringLiteral("hooks")).toArray())
        {
            const QJsonObject ho = hv.toObject();
            const QString hk = ho.value(QStringLiteral("hook")).toString().trimmed();
            QString ds = ho.value(QStringLiteral("description")).toString().trimmed();
            if (hk.isEmpty())
            {
                continue;
            }
            if (m_hashtags)
            {
                ds = m_hashtags->optimizeDescriptionHashtags(ds, productCategory);
            }
            productHookPool << qMakePair(hk, ds);
        }

        QHash<int, QJsonObject> plannedBySlot;
        for (const QJsonValue &sv : planObj.value(QStringLiteral("slots")).toArray())
        {
            const QJsonObject so = sv.toObject();
            plannedBySlot.insert(so.value(QStringLiteral("slot")).toInt(-1), so);
        }

        static const QStringList kFallbackHookCats{
            QStringLiteral("curiosity"),
            QStringLiteral("transformation"),
            QStringLiteral("style_question"),
            QStringLiteral("direct_value"),
            QStringLiteral("contrarian")
        };

        for (int s = 0; s < slotsForProduct.size(); ++s)
        {
            auto &sl = slotsForProduct[s];
            const QJsonObject so = plannedBySlot.value(s);
            QString basePrompt = so.value(QStringLiteral("prompt")).toString().trimmed();
            if (basePrompt.isEmpty())
            {
                const QString refNotes = sl.reference.value(QStringLiteral("notes")).toString().trimmed();
                if (sl.kind == GenerationJob::Video)
                {
                    basePrompt = QStringLiteral(
                        "Generate a video: Smoothly animate the starting image with natural motion "
                        "and steady camera tracking showcasing %1%2")
                        .arg(productSummary,
                             refNotes.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(refNotes));
                }
                else if (sl.kind == GenerationJob::Slideshow)
                {
                    basePrompt = QStringLiteral(
                        "Cohesive 4-slide fashion-editorial carousel showcasing %1 across opening "
                        "pose, full outfit styling, crisp close-up product detail, and confident stride")
                        .arg(productSummary);
                }
                else
                {
                    basePrompt = QStringLiteral(
                        "Editorial social-media fashion photo showcasing %1 in sharp focus with "
                        "clean natural lighting")
                        .arg(productSummary);
                }
            }
            QStringList slotPropFragments;
            if (!sl.method || sl.method->usesSceneProperties())
            {
                for (const auto &sp : sl.sampledProps)
                {
                    const QString frag = sp.promptFragment.trimmed();
                    if (!frag.isEmpty())
                    {
                        slotPropFragments << frag;
                    }
                }
            }
            InspirationAdaptationMethod::PromptContext finalizeCtx;
            finalizeCtx.productImageFile = planProdFile;
            finalizeCtx.productSummary = productSummary;
            finalizeCtx.referenceFrames = sl.stagedPlanRefFrames;
            finalizeCtx.referenceTitle = sl.reference.value(QStringLiteral("title")).toString();
            finalizeCtx.adaptationNotes = sl.reference.value(QStringLiteral("notes")).toString();
            finalizeCtx.videoFormatLabel = videoFormatLabel;
            finalizeCtx.propertyFragments = slotPropFragments.join(QStringLiteral(". "));

            if (sl.kind == GenerationJob::Video && sl.method)
            {
                sl.prompt = sl.method->finalizeVideoAnimationPrompt(basePrompt, finalizeCtx);
            }
            else if (sl.method && sl.method->appendsPropertiesToVideoPrompt())
            {
                QStringList fullParts;
                if (!basePrompt.endsWith(QLatin1Char('.')))
                {
                    basePrompt += QLatin1Char('.');
                }
                fullParts << basePrompt;
                for (QString frag : slotPropFragments)
                {
                    if (!frag.endsWith(QLatin1Char('.'))) frag += QLatin1Char('.');
                    fullParts << frag;
                }
                sl.prompt = fullParts.join(QStringLiteral(" "));
            }
            else
            {
                sl.prompt = basePrompt;
            }

            sl.hook = so.value(QStringLiteral("hook")).toString().trimmed();
            sl.description = so.value(QStringLiteral("description")).toString().trimmed();
            for (const QJsonValue &stv : so.value(QStringLiteral("slideTexts")).toArray())
            {
                const QString st = stv.toString().trimmed();
                if (!st.isEmpty())
                {
                    sl.slideTexts << st;
                }
            }
            if (sl.hook.isEmpty() && !productHookPool.isEmpty())
            {
                const auto &poolPick = productHookPool[s % productHookPool.size()];
                sl.hook = poolPick.first;
                if (sl.description.isEmpty())
                {
                    sl.description = poolPick.second;
                }
            }
            if (sl.hook.isEmpty())
            {
                sl.hook = tr("Wait until you see how %1 styles").arg(
                    !productColor.isEmpty() ? productColor.toLower() : productTitle.left(28));
            }
            if (sl.description.isEmpty())
            {
                const QString tagWord = QString{productCategory.isEmpty() ? QStringLiteral("fashion") : productCategory}
                    .remove(QLatin1Char(' ')).toLower();
                sl.description = QStringLiteral("%1 — styled for effortless impact. #%2 #ootd #style")
                    .arg(productSummary, tagWord);
            }
            if (m_hashtags)
            {
                sl.description = m_hashtags->optimizeDescriptionHashtags(sl.description, productCategory);
            }
            sl.hookCategory = so.value(QStringLiteral("hookCategory")).toString().trimmed();
            if (sl.hookCategory.isEmpty())
            {
                sl.hookCategory = kFallbackHookCats[s % kFallbackHookCats.size()];
            }
            // Ensure every slot's hook/description is also represented in productHookPool
            bool alreadyInPool = false;
            for (const auto &hp : productHookPool)
            {
                if (hp.first.compare(sl.hook, Qt::CaseInsensitive) == 0)
                {
                    alreadyInPool = true;
                    break;
                }
            }
            if (!alreadyInPool)
            {
                productHookPool.append(qMakePair(sl.hook, sl.description));
            }
        }

        // Execute each planned slot unattended
        for (int s = 0; s < slotsForProduct.size() && !m_batchCancelled && !hitHardStopError; ++s)
        {
            ++attemptedCreatives;
            const auto &sl = slotsForProduct[s];
            const QString slotTag = tr("Auto %1/%2 · %3 · %4")
                .arg(attemptedCreatives)
                .arg(totalCreatives)
                .arg(productColor.isEmpty() ? productTitle.left(22) : productColor,
                     sl.formatName);

            const QDir slotStaging{baseStaging.absoluteFilePath(
                QStringLiteral("auto_slot_%1").arg(s + 1, 2, 10, QLatin1Char('0')))};
            QDir{}.mkpath(slotStaging.absolutePath());
            QString snapErr;
            ResearchProject::snapshot(m_projects->projectDir(row), slotStaging, &snapErr);

            // Persist per-slot reference media copy if different from project default
            if (!sl.referenceMediaPath.isEmpty() && QFileInfo::exists(sl.referenceMediaPath))
            {
                const QString slotRefCopy = QStringLiteral("slot-reference.")
                    + QFileInfo{sl.referenceMediaPath}.suffix().toLower();
                QFile::copy(sl.referenceMediaPath, slotStaging.absoluteFilePath(slotRefCopy));
            }

            const QString stagedProdName = QStringLiteral("my_product.%1").arg(prodExt);
            const QString stagedProdPath = slotStaging.absoluteFilePath(stagedProdName);
            QFile::remove(stagedProdPath);
            QFile::copy(projectProductImg, stagedProdPath);

            QString refSceneName;
            if (!sl.referenceMediaPath.isEmpty() && QFileInfo::exists(sl.referenceMediaPath))
            {
                extractMediaFramesIntoDir(sl.referenceMediaPath, slotStaging,
                                          QStringLiteral("reference"), &refSceneName);
            }

            QStringList producedOutputs;

            if (sl.kind == GenerationJob::Video)
            {
                // Step 1: Unattended starting-frame generation + AI product-fidelity gate
                const QString swappedTarget = slotStaging.absoluteFilePath(GENERATED_IMAGE_NAME);
                const QString swappedBackup = swappedTarget + QStringLiteral(".prev");
                QString swapFixNote;
                bool haveStartingFrame = false;

                const int maxFidelityRounds = 2;
                const InspirationAdaptationMethod *slotMethod = sl.method
                    ? sl.method
                    : InspirationAdaptationMethod::all().value(
                          refSceneName.isEmpty() ? QStringLiteral("editorial_ab") : QStringLiteral("reference_swap"),
                          nullptr);

                QStringList variantPropHints;
                if (!slotMethod || slotMethod->usesSceneProperties())
                {
                    for (const auto &sp : sl.sampledProps)
                    {
                        if (!sp.promptFragment.trimmed().isEmpty())
                        {
                            variantPropHints << sp.promptFragment.trimmed();
                        }
                    }
                }
                const QString variantDirection = variantPropHints.join(QStringLiteral("; "));

                for (int round = 1; round <= maxFidelityRounds && !m_batchCancelled; ++round)
                {
                    _logProgress(tr("[%1] Step 1/2 — Generating starting frame via %2 (round %3/%4)...")
                        .arg(slotTag, slotMethod ? slotMethod->name() : sl.strategy)
                        .arg(round)
                        .arg(maxFidelityRounds));

                    if (QFileInfo::exists(swappedTarget))
                    {
                        QFile::remove(swappedBackup);
                        QFile::rename(swappedTarget, swappedBackup);
                    }

                    InspirationAdaptationMethod::PromptContext stepCtx;
                    stepCtx.productImageFile = stagedProdName;
                    stepCtx.productSummary = productSummary;
                    stepCtx.referenceSceneFile = refSceneName;
                    stepCtx.referenceFrames = sl.stagedPlanRefFrames;
                    stepCtx.referenceTitle = sl.reference.value(QStringLiteral("title")).toString();
                    stepCtx.adaptationNotes = sl.reference.value(QStringLiteral("notes")).toString();
                    stepCtx.videoFormatLabel = videoFormatLabel;
                    stepCtx.outputImageFile = GENERATED_IMAGE_NAME;
                    stepCtx.propertyFragments = variantDirection;
                    stepCtx.refinementFixNote = swapFixNote;

                    const QString stepPrompt = slotMethod
                        ? slotMethod->buildVideoStartingFramePrompt(stepCtx)
                        : imageStepPrompt(DialogGenerationOptions::ImageMode::BootstrapImage,
                                          stagedProdName, videoFormatLabel, false,
                                          GENERATED_IMAGE_NAME);

                    bool wroteFrame = false;
                    for (int tryIdx = 1; tryIdx <= 3 && !m_batchCancelled; ++tryIdx)
                    {
                        const QDateTime callStart = QDateTime::currentDateTime();
                        CliRunResult imgRes;
                        imgRes = co_await imgCli->runPrompt(stepPrompt, slotStaging.absolutePath());
                        if ((!imgRes.processStarted || imgRes.exitCode != 0)
                            && imgCli->classifyError(imgRes.errorOutput) != CliErrorKind::Other)
                        {
                            hitHardStopError = true;
                            hardStopReason = friendlyCliError(imgCli, imgRes);
                            break;
                        }
                        if (imgRes.processStarted && imgRes.exitCode == 0
                            && recoverCliOutputFile(imgCli, callStart, GENERATED_IMAGE_NAME, swappedTarget))
                        {
                            wroteFrame = true;
                            break;
                        }
                    }
                    if (hitHardStopError || m_batchCancelled)
                    {
                        break;
                    }
                    if (!wroteFrame)
                    {
                        if (QFileInfo::exists(swappedBackup))
                        {
                            QFile::rename(swappedBackup, swappedTarget);
                            haveStartingFrame = true;
                        }
                        break;
                    }
                    QFile::remove(swappedBackup);
                    haveStartingFrame = true;

                    if (round >= maxFidelityRounds)
                    {
                        break;
                    }

                    // Automatic AI vision check across ALL video modes: verify our exact product design & clean frame
                    const QString verifyPrompt = QStringLiteral(
                        "Inspect '%1' (generated starting frame) against '%2' (our real product: %3) "
                        "in the current working directory.\n"
                        "Check: (1) Does the featured product accurately match our exact real product "
                        "(silhouette, heel shape/height, straps, color/pattern)? "
                        "(2) Is the frame crisp and free of watermarks, social icons, text, and garbled anatomy?\n"
                        "Reply with STRICT JSON only:\n"
                        "{\"ok\": true|false, \"fix\": \"<concise fix instruction if ok is false>\"}")
                        .arg(GENERATED_IMAGE_NAME, stagedProdName, productSummary);
                    CliRunResult verifyRes;
                    verifyRes = co_await promptCli->runPrompt(verifyPrompt, slotStaging.absolutePath());
                    const QJsonObject vObj = parseFirstJsonObject(verifyRes.output);
                    if (vObj.isEmpty() || vObj.value(QStringLiteral("ok")).toBool(true))
                    {
                        _logProgress(tr("[%1] AI quality gate passed on starting frame.").arg(slotTag));
                        break;
                    }
                    swapFixNote = vObj.value(QStringLiteral("fix")).toString().trimmed();
                    _logProgress(tr("[%1] AI quality gate requested refinement: %2")
                        .arg(slotTag, swapFixNote));
                }

                if (hitHardStopError || m_batchCancelled)
                {
                    break;
                }

                const QString frameToAnimate = haveStartingFrame ? swappedTarget : stagedProdPath;
                _logProgress(tr("[%1] Step 2/2 — Animating frame into video...").arg(slotTag));

                VideoGenerationWorkflow::Request req;
                req.generatorId = videoGeneratorId;
                req.prompt = ensureImperativeVideoPrefix(sl.prompt);
                req.imagePath = frameToAnimate;
                req.outputDir = slotStaging.absolutePath();
                req.contextSummary = QStringLiteral("%1 — %2").arg(productSummary, sl.hook);
                req.videoFormatLabel = videoFormatLabel;
                req.settings = m_generationSettings->settingsFor(videoGeneratorId);
                req.decisionCli = promptCli;
                req.imageCli = imgCli;
                req.knownPitfalls = m_promptLessons->asPromptSection();
                req.recordLesson = [this](const QString &lesson) {
                    m_promptLessons->addLesson(lesson);
                };

                auto workflow = std::make_unique<VideoGenerationWorkflow>();
                if (m_progress.cancelBtn)
                {
                    connect(m_progress.cancelBtn, &QPushButton::clicked,
                            workflow.get(), &VideoGenerationWorkflow::cancel);
                }
                connect(workflow.get(), &VideoGenerationWorkflow::progress,
                        this, [this, slotTag](const QString &msg) {
                    _logProgress(QStringLiteral("[%1] %2").arg(slotTag, msg));
                });

                QString generatedVideoPath;
                QString videoErr;
                bool videoDone = false;
                connect(workflow.get(), &VideoGenerationWorkflow::finished,
                        this, [&generatedVideoPath, &videoErr, &videoDone](const QString &vp, const QString &err) {
                    generatedVideoPath = vp;
                    videoErr = err;
                    videoDone = true;
                });

                workflow->start(req);
                while (!videoDone)
                {
                    if (m_batchCancelled)
                    {
                        workflow->cancel();
                    }
                    co_await QCoro::sleepFor(std::chrono::milliseconds(50));
                }

                if (!generatedVideoPath.isEmpty() && QFileInfo::exists(generatedVideoPath))
                {
                    producedOutputs << generatedVideoPath;
                }
                else
                {
                    _logProgress(tr("[%1] Video slot failed: %2")
                        .arg(slotTag, videoErr.isEmpty() ? tr("no video produced") : videoErr));
                }
            }
            else
            {
                // Single Image or 4-slide Carousel
                AbstractImageGenerator *generator
                    = AbstractImageGenerator::ALL_IMAGE_GENERATORS().value(QStringLiteral("cli"));
                if (!generator)
                {
                    _logProgress(tr("[%1] Skipped: CLI image generator unavailable.").arg(slotTag));
                    continue;
                }
                QStringList refImages{stagedProdPath};
                QString effectiveImgPrompt = sl.prompt;
                if (sl.method)
                {
                    QStringList slotPropFrags;
                    if (sl.method->usesSceneProperties())
                    {
                        for (const auto &sp : sl.sampledProps)
                        {
                            if (!sp.promptFragment.trimmed().isEmpty())
                            {
                                slotPropFrags << sp.promptFragment.trimmed();
                            }
                        }
                    }
                    InspirationAdaptationMethod::PromptContext imgCtx;
                    imgCtx.productImageFile = stagedProdName;
                    imgCtx.productSummary = productSummary;
                    imgCtx.referenceSceneFile = refSceneName;
                    imgCtx.referenceFrames = sl.stagedPlanRefFrames;
                    imgCtx.referenceTitle = sl.reference.value(QStringLiteral("title")).toString();
                    imgCtx.adaptationNotes = sl.reference.value(QStringLiteral("notes")).toString();
                    imgCtx.videoFormatLabel = videoFormatLabel;
                    imgCtx.propertyFragments = slotPropFrags.join(QStringLiteral(". "));

                    bool attachRef = false;
                    effectiveImgPrompt = sl.method->buildStillOrCarouselPrompt(sl.prompt, imgCtx, &attachRef);
                    if (attachRef && !refSceneName.isEmpty())
                    {
                        refImages << slotStaging.absoluteFilePath(refSceneName);
                    }
                }

                const int slideCount = (sl.kind == GenerationJob::Slideshow) ? 4 : 1;
                QVariantMap imgSettings;
                imgSettings.insert(QStringLiteral("formatLabel"), videoFormatLabel);
                const QString pitfalls = m_promptLessons->asPromptSection();
                if (!pitfalls.isEmpty())
                {
                    imgSettings.insert(QStringLiteral("knownPitfalls"), pitfalls);
                }

                _logProgress(tr("[%1] Generating %2...").arg(
                    slotTag, slideCount > 1 ? tr("4-slide carousel") : tr("image")));

                AbstractImageGenerator::Result imgRes;
                imgRes = co_await generator->generate(
                    effectiveImgPrompt,
                    refImages,
                    slideCount,
                    slotStaging.absolutePath(),
                    imgSettings,
                    imgCli,
                    [this, slotTag](const QString &msg) {
                        _logProgress(QStringLiteral("[%1] %2").arg(slotTag, msg));
                    },
                    [this](const QString &lesson) {
                        m_promptLessons->addLesson(lesson);
                    });

                if (imgRes.errorMessage.isEmpty() && !imgRes.imagePaths.isEmpty())
                {
                    if (sl.kind == GenerationJob::Slideshow && carouselHookOverlay)
                    {
                        if (!sl.slideTexts.isEmpty())
                        {
                            const int n = qMin(imgRes.imagePaths.size(), sl.slideTexts.size());
                            for (int si = 0; si < n; ++si)
                            {
                                overlayHookTextOnSlide(imgRes.imagePaths[si], sl.slideTexts[si]);
                            }
                        }
                        else if (!sl.hook.isEmpty())
                        {
                            overlayHookTextOnSlide(imgRes.imagePaths.first(), sl.hook);
                        }
                    }
                    producedOutputs = imgRes.imagePaths;
                }
                else
                {
                    _logProgress(tr("[%1] Image/carousel slot failed: %2")
                        .arg(slotTag, imgRes.errorMessage));
                }
            }

            if (producedOutputs.isEmpty())
            {
                continue;
            }

            // Persist generation_prompt.txt in staging before _finalizeGeneration moves it to temp/
            {
                QFile pf{slotStaging.absoluteFilePath(QStringLiteral("generation_prompt.txt"))};
                if (pf.open(QIODevice::WriteOnly))
                {
                    pf.write(sl.prompt.toUtf8());
                }
            }
            // Build the per-creative 10-hook candidate list (this creative's primary hook first,
            // followed by all alternative suggested hooks & optimized hashtags for "Hooks...")
            QList<QPair<QString, QString>> slotHooksList;
            slotHooksList << qMakePair(sl.hook, sl.description);
            for (const auto &hp : productHookPool)
            {
                if (hp.first.compare(sl.hook, Qt::CaseInsensitive) != 0)
                {
                    slotHooksList << hp;
                }
            }

            QString shortCode;
            const QDir genDir = _finalizeGeneration(
                row, projectId, producedOutputs, slotStaging, sl.keptValueIds, &shortCode);
            const QDir tempDir = m_projects->generationTempDir(row, shortCode);
            saveHooksFile(tempDir, slotHooksList, shortCode, sl.prompt);
            saveHooksFile(genDir, slotHooksList, shortCode, sl.prompt);
            _refreshHooksView(tempDir);

            m_runGenerations << qMakePair(genDir, shortCode);
            _saveChosenHookDescription(row, {{genDir, shortCode}}, sl.hook, sl.description);

            // Persist structured Creative DNA for closed-loop attribution & self-improvement
            QJsonArray propUuidArray;
            for (const QUuid &uid : sl.keptValueIds)
            {
                propUuidArray.append(uid.toString(QUuid::WithoutBraces));
            }
            const QJsonObject dna{
                {QStringLiteral("schemaVersion"), 1},
                {QStringLiteral("shortCode"), shortCode},
                {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                {QStringLiteral("accountId"), accountId},
                {QStringLiteral("productId"), productId},
                {QStringLiteral("productTitle"), productTitle},
                {QStringLiteral("productColor"), productColor},
                {QStringLiteral("referenceId"), sl.reference.value(QStringLiteral("id")).toString()},
                {QStringLiteral("referenceUrl"), sl.reference.value(QStringLiteral("url")).toString()},
                {QStringLiteral("format"), sl.formatName},
                {QStringLiteral("strategy"), sl.strategy},
                {QStringLiteral("usedSceneProperties"), !sl.method || sl.method->usesSceneProperties()},
                {QStringLiteral("isExploration"), sl.isExploration},
                {QStringLiteral("hookCategory"), sl.hookCategory},
                {QStringLiteral("hook"), sl.hook},
                {QStringLiteral("hasSlideTextOverlay"), sl.kind == GenerationJob::Slideshow && carouselHookOverlay},
                {QStringLiteral("prompt"), sl.prompt},
                {QStringLiteral("propertyValueIds"), propUuidArray}
            };
            {
                QSaveFile dnaFile{genDir.filePath(QStringLiteral("creative_dna.json"))};
                if (dnaFile.open(QIODevice::WriteOnly))
                {
                    dnaFile.write(QJsonDocument{dna}.toJson(QJsonDocument::Indented));
                    dnaFile.commit();
                }
            }

            ++completedCreatives;
            if (m_progress.bar)
            {
                m_progress.bar->setValue(completedCreatives);
            }
            _logProgress(tr("[%1] Saved %2 (%3/%4 complete).")
                .arg(slotTag, shortCode).arg(completedCreatives).arg(totalCreatives));

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
        }

        m_projects->stagingDir(row).removeRecursively();
    }

    if (hitHardStopError)
    {
        _finishProgress(tr("Auto-Batch paused (%1/%2 saved): %3")
            .arg(completedCreatives).arg(totalCreatives).arg(hardStopReason));
    }
    else if (m_batchCancelled)
    {
        _finishProgress(tr("Auto-Batch cancelled. %1/%2 creative(s) saved.")
            .arg(completedCreatives).arg(totalCreatives));
    }
    else
    {
        _finishProgress(tr("Auto-Batch finished: %1/%2 creative(s) generated with hooks, captions, and tracking codes.")
            .arg(completedCreatives).arg(totalCreatives));
    }
}
