#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemModel>
#include <QImage>
#include <QMediaPlayer>
#include <QProcess>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QCoro/QCoroTimer>
#include <chrono>
#include <stdexcept>
#include <iostream>

#include "gui/DialogGenerateAgain.h"
#include "AbstractCli.h"
#include "gui/DialogGenerationPlan.h"
#include "gui/GenerationPlanSection.h"
#include "gui/panes/PaneGeneration.h"
#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/TableProjects.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/AbstractVideoGenerator.h"
#include "model/videogen/TableGenerationSettings.h"
#include "model/videogen/VideoGenerationRecipe.h"
#include "model/videogen/VideoClipProcessing.h"
#include "model/videogen/VideoGeneratorGemini.h"
#include "model/videogen/AbstractMultiVideoWorkflow.h"
#include "model/videogen/ProductShowcaseMultiVideoWorkflow.h"
#include "gui/MultiVideoStepWidget.h"
#include "gui/DialogMultiVideoGeneration.h"
#include "../common/workingdirectory/WorkingDirectoryManager.h"

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "open fixture file");
    require(file.write(data) == data.size(), "write fixture file");
}

static QByteArray readFile(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture file");
    return file.readAll();
}

class FakeGenerator : public AbstractVideoGenerator
{
public:
    mutable QList<VideoGenerationRecipe> calls;
    mutable QStringList outputs;
    mutable bool fail = false;
    mutable int browserFailures = 0;
    mutable std::function<void()> onCall;
    QString getId() const override { return QStringLiteral("test-backend"); }
    QString getName() const override { return QStringLiteral("Test backend"); }
    QCoro::Task<Result> generate(const QString &prompt, const QStringList &images,
                                const QString &output, const QVariantMap &settings) const override
    {
        VideoGenerationRecipe recipe{getId(), prompt, images, settings};
        calls << recipe;
        outputs << output;
        if (onCall) onCall();
        co_await QCoro::sleepFor(std::chrono::milliseconds(20));
        Result result;
        if (browserFailures > 0)
        {
            --browserFailures;
            result.errorMessage = QStringLiteral("Browser connection lost");
            result.browserLost = true;
            co_return result;
        }
        if (fail)
        {
            result.errorMessage = QStringLiteral("Test failure");
            co_return result;
        }
        require(recipe.save(QDir(output), &result.errorMessage), "save fake recipe");
        writeFile(QDir(output).filePath("generation_prompt.txt"), prompt.toUtf8());
        result.videoPath = QDir(output).filePath("take.mp4");
        writeFile(result.videoPath, "fake video; never contact a real provider");
        co_return result;
    }
};

static void checkRecipes(const QDir &root)
{
    const QString source = root.filePath("source.png");
    writeFile(source, "original source");
    root.mkpath("saved");
    VideoGenerationRecipe recipe{"test-backend", "Initial prompt", {source}, {{"quality", 7}}};
    QString error;
    require(recipe.save(QDir(root.filePath("saved")), &error), "save recipe");
    writeFile(root.filePath("saved/generation_prompt.txt"), "Actual successful prompt");
    require(QDir().rename(root.filePath("saved"), root.filePath("moved")), "move generation");
    VideoGenerationRecipe loaded;
    require(VideoGenerationRecipe::load(QDir(root.filePath("moved")), &loaded, &error), "load moved recipe");
    require(loaded.prompt == "Actual successful prompt", "prefer actual successful prompt");
    require(loaded.settings == recipe.settings, "preserve settings");
    require(readFile(loaded.imagePaths.first()) == "original source", "portable source copy");
    QFile::remove(loaded.imagePaths.first());
    require(!VideoGenerationRecipe::load(QDir(root.filePath("moved")), &loaded, &error), "reject missing image");
    writeFile(root.filePath("moved/generation_config.json"), "broken JSON");
    require(!VideoGenerationRecipe::load(QDir(root.filePath("moved")), &loaded, &error), "reject broken recipe");
}

static void checkPlan(SavedPrompts &saved)
{
    DialogGenerationPlan noCli{{}, {}, &saved, "vertical (9:16)"};
    const auto clis = AbstractCli::ALL_CLIS(); // capabilities only; no CLI is executed
    DialogGenerationPlan dialog{{}, clis, &saved, "vertical (9:16)"};
    for (auto *group : dialog.findChildren<QGroupBox *>())
    {
        auto *section = dynamic_cast<GenerationPlanSection *>(group);
        if (!section) continue;
        const bool selected = !section->title().contains("Gemini");
        section->setChecked(selected);
        section->setGenerationCount(section->title().startsWith("One") ? 3
            : section->title().startsWith("Several") ? 2 : 4);
        for (auto *editor : section->findChildren<QPlainTextEdit *>()) editor->setPlainText("Test prompt");
        require(section->findChild<QSpinBox *>("spinCount")->isEnabled() == selected,
                "count follows content checkbox");
    }
    auto plan = dialog.plan();
    require(plan.oneImage && plan.imageCount == 3, "image count");
    require(plan.slideshow && plan.slideshowCount == 2, "slideshow count");
    require(plan.videos.size() == 1 && plan.videos.first().count == 4, "checked video count");
    dialog.accept();
    DialogGenerationPlan restored{{}, clis, &saved, "vertical (9:16)"};
    require(restored.plan().imageCount == 3 && restored.plan().slideshowCount == 2
            && restored.plan().videos.first().count == 4, "remember generation counts");
}

static void waitForBatch(PaneGeneration &pane)
{
    QElapsedTimer timer;
    timer.start();
    while (!pane.isEnabled() && timer.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(2);
    }
    require(pane.isEnabled(), "batch completed within timeout");
    QApplication::processEvents();
}

static void answerRepeatDialog(int count, bool accept = true)
{
    QTimer::singleShot(0, [count, accept]() {
        bool found = false;
        for (auto *widget : QApplication::topLevelWidgets())
        {
            if (auto *dialog = dynamic_cast<DialogGenerateAgain *>(widget))
            {
                dialog->findChild<QSpinBox *>("spinCount")->setValue(count);
                if (accept) dialog->accept(); else dialog->reject();
                found = true;
            }
        }
        require(found, "repeat dialog opened");
    });
}

static void checkMultiVideo(const QDir &root)
{
    // 1. Workflow setup and step structure
    ProductShowcaseMultiVideoWorkflow workflow;
    const QString introPrompt = QStringLiteral("Bold opening zoom on product line");
    const QString afterIntroPrompt = QStringLiteral("Sleek 360 rotation with details");

    // Up to 4 images per step
    QList<QList<AbstractMultiVideoWorkflow::ImageInput>> stepImages;
    // Step 0 (Intro): 3 images
    stepImages.append({
        {root.filePath("img_intro_1.png"), AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal, ""},
        {root.filePath("img_intro_2.png"), AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate, ""},
        {root.filePath("img_intro_3.png"), AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite, ""}
    });
    // Step 1 (Product 1): 4 images
    stepImages.append({
        {root.filePath("prod1_1.png"), AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal, ""},
        {root.filePath("prod1_2.png"), AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate, ""},
        {root.filePath("prod1_3.png"), AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite, ""},
        {root.filePath("prod1_4.png"), AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal, ""}
    });
    // Step 2 (Product 2): 4 images
    stepImages.append({
        {root.filePath("prod2_1.png"), AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal, ""},
        {root.filePath("prod2_2.png"), AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal, ""},
        {root.filePath("prod2_3.png"), AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate, ""},
        {root.filePath("prod2_4.png"), AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite, ""}
    });

    // 1 intro step + 2 product steps = 3 steps total
    // Intro: 2 takes, Product steps: 3 takes each -> Total: 2 + 3 + 3 = 8 takes
    workflow.configure(3, introPrompt, 2, afterIntroPrompt, 3, stepImages);

    require(workflow.steps().size() == 3, "3 steps configured");
    require(workflow.steps()[0].type == AbstractMultiVideoWorkflow::StepType::Intro, "step 0 is intro");
    require(workflow.steps()[1].type == AbstractMultiVideoWorkflow::StepType::AfterIntro, "step 1 is after-intro");
    require(workflow.steps()[2].type == AbstractMultiVideoWorkflow::StepType::AfterIntro, "step 2 is after-intro");
    require(workflow.steps()[0].images.size() == 3, "intro has 3 images");
    require(workflow.steps()[1].images.size() == 4, "step 1 has 4 images");
    require(workflow.steps()[2].images.size() == 4, "step 2 has 4 images");
    require(workflow.totalVideoCount() == 8, "total 8 videos expected (2 intro + 2*3 after-intro)");
    require(workflow.completedVideoCount() == 0, "0 completed initially");

    // 2. File naming conventions for Dropbox upload / sorting
    require(workflow.videoFileName(0, 0) == QStringLiteral("01_step1_intro_take01.mp4"), "take 0 name");
    require(workflow.videoFileName(0, 1) == QStringLiteral("01_step1_intro_take02.mp4"), "take 1 name");
    require(workflow.videoFileName(1, 0) == QStringLiteral("02_step2_product1_take01.mp4"), "step 2 take 1 name");
    require(workflow.videoFileName(1, 2) == QStringLiteral("02_step2_product1_take03.mp4"), "step 2 take 3 name");
    require(workflow.videoFileName(2, 0) == QStringLiteral("03_step3_product2_take01.mp4"), "step 3 take 1 name");

    // 3. Serialization and state persistence (manifest)
    const QString manifestFile = root.filePath("multi_video_workflow.json");
    require(workflow.saveToFile(manifestFile), "save multi-video manifest");

    ProductShowcaseMultiVideoWorkflow restored;
    require(restored.loadFromFile(manifestFile), "load multi-video manifest");
    require(restored.totalVideoCount() == 8, "restored total video count");
    require(restored.steps().size() == 3, "restored steps size");
    require(restored.steps()[0].prompt == introPrompt, "restored intro prompt");
    require(restored.steps()[1].prompt == afterIntroPrompt, "restored after-intro prompt");

    // 4. Partial completion & Resume check:
    // Suppose 4 out of 8 videos were generated and blocked by quota limit
    restored.setConversationUrl(QStringLiteral("https://gemini.google.com/app/test_conv_1234"));
    for (int i = 0; i < 4; ++i)
    {
        restored.takes()[i].completed = true;
        restored.takes()[i].filePath = root.filePath(restored.takes()[i].expectedFileName);
    }
    require(restored.completedVideoCount() == 4, "4 of 8 completed");
    require(restored.saveToFile(manifestFile), "save partially completed manifest");

    ProductShowcaseMultiVideoWorkflow resumed;
    require(resumed.loadFromFile(manifestFile), "load partially completed manifest");
    require(resumed.completedVideoCount() == 4, "resumed sees 4 completed");
    require(resumed.totalVideoCount() == 8, "resumed sees total 8");
    require(!resumed.takes()[4].completed, "take 4 is ready to be resumed");
    require(resumed.conversationUrl() == QStringLiteral("https://gemini.google.com/app/test_conv_1234"),
            "conversationUrl persisted across resume");

    // 5. TableProjects multi-video support
    TableProjects projects{root.path()};
    const int singleRow = projects.addProject(root.filePath("source.png"), "", "Single Bag", "Best bag");
    require(!projects.isMultiVideoProject(singleRow), "standard project is not multi-video");
    require(projects.projectType(singleRow) == QStringLiteral("single"), "standard project type is single");

    const int multiRow = projects.addMultiVideoProject("Multi Bag Showcase", root.filePath("source.png"));
    require(projects.isMultiVideoProject(multiRow), "multi-video project detected");
    require(projects.projectType(multiRow) == QStringLiteral("multi_video"), "multi-video project type");

    // 6. Settings for CLI validation frame count (5 to 10)
    TableGenerationSettings genSettings{root.path()};
    const auto geminiSettings = genSettings.settingsFor(QStringLiteral("gemini-browser"));
    require(geminiSettings.contains(QStringLiteral("validationFramesCount")),
            "validationFramesCount setting present");
    const int frameCount = geminiSettings.value(QStringLiteral("validationFramesCount")).toInt();
    require(frameCount >= 5 && frameCount <= 10, "validationFramesCount in range 5-10");

    // 7. MultiVideoStepWidget unit check
    MultiVideoStepWidget stepWidget{0, true};
    require(stepWidget.isIntro(), "step widget is intro");
    stepWidget.setImage(0, root.filePath("source.png"), AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate);
    require(stepWidget.imageInputs().size() == 1, "1 image set on widget");
    require(stepWidget.imageInputs()[0].regenOption == AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate,
            "regen option correctly stored");
}


static QByteArray mediaCommand(const QString &program, const QStringList &args)
{
    QProcess process;
    process.start(program, args);
    require(process.waitForFinished(30000), "media fixture command finished");
    if (process.exitCode() != 0) std::cerr << process.readAllStandardError().constData();
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "media fixture command succeeded");
    return process.readAllStandardOutput();
}

template<typename T>
static T awaitMedia(QCoro::Task<T> task)
{
    T result;
    bool done = false;
    auto collect = [&]() -> QCoro::Task<void> { result = co_await task; done = true; };
    auto running = collect();
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 30000)
    {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    require(done, "media coroutine completed");
    return result;
}

static double videoDuration(const QString &path)
{
    return mediaCommand("ffprobe", {"-v", "error", "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1", path}).trimmed().toDouble();
}

static QColor videoPixel(const QString &path, double seconds)
{
    const QByteArray png = mediaCommand("ffmpeg", {"-v", "error", "-ss", QString::number(seconds),
        "-i", path, "-frames:v", "1", "-f", "image2pipe", "-c:v", "png", "-"});
    const auto image = QImage::fromData(png);
    require(!image.isNull(), "decode sample frame");
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

static void waitForMerge(ProductShowcaseMultiVideoWorkflow &workflow)
{
    QElapsedTimer timer;
    timer.start();
    while (workflow.isMerging() && timer.elapsed() < 30000)
    {
        QApplication::processEvents();
        QThread::msleep(10);
    }
    require(!workflow.isMerging(), "automatic merge completed");
    require(QFile::exists(workflow.mergedVideoPath()), "final video exists");
}

static void checkVideoClips(const QDir &root)
{
    VideoGeneratorGemini gemini;
    require(gemini.continuationDurationSeconds({}) == 10.0, "Gemini default segment length");
    require(gemini.continuationDurationSeconds({{"continuationDurationSec", 8.0}}) == 8.0, "provider duration configurable");
    FakeGenerator other;
    require(other.continuationDurationSeconds({}) == 0, "other providers preserve their downloads");

    const QString red = root.filePath("red.mp4");
    const QString green = root.filePath("green.mp4");
    const QString combined = root.filePath("combined.mp4");
    mediaCommand("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "color=red:s=160x120:d=1:r=30",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=1", "-c:v", "libx264", "-c:a", "aac", "-shortest", red});
    mediaCommand("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "color=green:s=120x160:d=1:r=24",
        "-c:v", "libx264", green});
    mediaCommand("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "color=red:s=160x120:d=1:r=30",
        "-f", "lavfi", "-i", "color=blue:s=160x120:d=1:r=30", "-filter_complex",
        "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-c:v", "libx264", combined});
    const auto clip = awaitMedia(VideoClipProcessing::trimTail(combined, 1.0));
    require(clip.error.isEmpty() && clip.path != combined, "trim cumulative video");
    require(qAbs(videoDuration(clip.path) - 1.0) < 0.06, "tail duration is one second");
    require(videoPixel(clip.path, 0.1).blue() > 200 && videoPixel(clip.path, 0.8).blue() > 200,
        "tail contains only new blue segment, not red prefix");
    require(awaitMedia(VideoClipProcessing::trimTail(red, 1.0)).path == red, "equal length unchanged");
    require(awaitMedia(VideoClipProcessing::trimTail(red, 2.0)).path == red, "short clip unchanged");
    require(awaitMedia(VideoClipProcessing::trimTail(combined, 0)).path == combined, "disabled trim unchanged");

    ProductShowcaseMultiVideoWorkflow workflow;
    workflow.configure(2, "Intro", 1, "Continuation", 2);
    auto json = workflow.toJson();
    json["state"] = static_cast<int>(AbstractMultiVideoWorkflow::State::Completed);
    json["outputDir"] = root.path();
    workflow.fromJson(json);
    const QStringList paths{red, clip.path, green};
    for (int i = 0; i < paths.size(); ++i)
    {
        workflow.takes()[i].filePath = paths[i];
        workflow.takes()[i].completed = true;
    }
    workflow.updateMergedVideo();
    require(!workflow.isMerging(), "no merge before all selections");
    workflow.selectTake(1, 0);
    waitForMerge(workflow);
    require(qAbs(videoDuration(workflow.mergedVideoPath()) - 2.0) < 0.1, "merged clip duration");
    require(videoPixel(workflow.mergedVideoPath(), 0.2).red() > 200, "intro first in merge");
    require(videoPixel(workflow.mergedVideoPath(), 1.2).blue() > 200, "selected continuation second");
    require(mediaCommand("ffprobe", {"-v", "error", "-select_streams", "a:0", "-show_entries", "stream=codec_type",
        "-of", "default=noprint_wrappers=1:nokey=1", workflow.mergedVideoPath()}).trimmed() == "audio", "audio retained with silent clips");

    workflow.selectTake(1, 1);
    // Change again while ffmpeg is active, then choose green as the final selection.
    QTimer::singleShot(30, &workflow, [&] { workflow.selectTake(1, 0); });
    QTimer::singleShot(60, &workflow, [&] { workflow.selectTake(1, 1); });
    waitForMerge(workflow);
    const QColor updated = videoPixel(workflow.mergedVideoPath(), 1.2);
    require(updated.green() > 90 && updated.blue() < 50, "latest selection wins while merge runs");
    require(qAbs(videoDuration(workflow.mergedVideoPath()) - 2.0) < 0.1, "mixed sizes and frame rates merge");

    ProductShowcaseMultiVideoWorkflow restored;
    require(restored.loadFromFile(root.filePath("multi_video_workflow.json")), "reload merge selection manifest");
    restored.selectTake(1, 0);
    waitForMerge(restored);
    require(videoPixel(restored.mergedVideoPath(), 1.2).blue() > 200, "selection after reload rebuilds final");
    const auto previous = readFile(restored.mergedVideoPath());
    const auto invalid = awaitMedia(VideoClipProcessing::merge({root.filePath("missing.mp4")}, restored.mergedVideoPath()));
    require(!invalid.error.isEmpty() && readFile(restored.mergedVideoPath()) == previous,
        "failed merge preserves existing final video");
}

static void checkMultiVideoIdentity(PaneGeneration &pane, TableVideos &videos, const QDir &root)
{
    auto *table = pane.findChild<QTableView *>("tableViewProjects");
    auto *projects = qobject_cast<TableProjects *>(table->model());
    require(projects != nullptr, "project model available");
    const int row = projects->addMultiVideoProject("Completed multi-video", {});
    const QUuid projectId = projects->projectId(row);
    QStringList codes;
    for (int i = 0; i < 4; ++i)
    {
        const QUuid id = videos.addVideo({}, {}, "original", projectId);
        codes << videos.recordFromId(id)->shortCode;
    }
    const QString code = codes.last();
    const QDir outputDir = projects->projectGenerationsDir(row);
    const QDir finalDir = projects->generationDir(row, code);
    ProductShowcaseMultiVideoWorkflow workflow;
    workflow.configure(2, "Intro", 1, "Product", 1);
    auto json = workflow.toJson();
    json["outputDir"] = outputDir.absolutePath();
    json["projectDir"] = projects->projectDir(row).absolutePath();
    json["state"] = static_cast<int>(AbstractMultiVideoWorkflow::State::Completed);
    workflow.fromJson(json);
    for (auto &take : workflow.takes())
    {
        take.filePath = finalDir.filePath(take.expectedFileName);
        take.completed = true;
        require(QFile::copy(root.filePath("green.mp4"), take.filePath), "copy legacy take");
    }
    require(QFile::copy(root.filePath("combined.mp4"), finalDir.filePath("final_video.mp4")), "copy legacy final");
    const QDir legacyInputs(outputDir.filePath("step_1_inputs"));
    require(legacyInputs.mkpath("."), "create old step inputs");
    const QString processed = legacyInputs.filePath("processed.png");
    writeFile(processed, "processed input fixture");
    workflow.steps()[0].images.append({root.filePath("source.png"),
        AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate, processed});
    const QDir legacyWork(outputDir.filePath("working_step1_take1"));
    require(legacyWork.mkpath("replay_inputs"), "create old working directory");
    writeFile(legacyWork.filePath("replay_inputs/image_1.png"), "saved replay image");
    writeFile(legacyWork.filePath("generation_config.json"), R"({"images":["replay_inputs/image_1.png"]})");
    require(workflow.saveToFile(outputDir.filePath("multi_video_workflow.json")), "save legacy root manifest");
    for (int i = 1; i < 4; ++i)
    {
        require(workflow.saveToFile(projects->generationDir(row, codes[i]).filePath("multi_video_workflow.json")),
            "save legacy duplicate manifest");
    }
    require(QFile::copy(root.filePath("red.mp4"), projects->generationDir(row, codes[1]).filePath("old_take.mp4")),
        "legacy folder retains a candidate");
    table->setCurrentIndex(table->model()->index(row, 0));
    auto *tree = pane.findChild<QTreeWidget *>("treeViewGenerations");
    require(tree->topLevelItemCount() == 1, "legacy retries display as one workflow");
    require(tree->currentItem() && tree->currentItem()->text(0) == code, "select latest workflow result automatically");
    require(tree->currentItem()->text(1) == "Final merged video", "label final instead of counting candidates");
    require(pane.findChild<QFileSystemModel *>()->rootPath() == finalDir.absolutePath(),
        "file browser opens the selected result instead of four legacy folders");
    require(pane.findChild<QMediaPlayer *>()->source().toLocalFile() == finalDir.filePath("final_video.mp4"),
        "preview merged result");

    require(QMetaObject::invokeMethod(&pane, "_resumeMultiVideoGeneration", Qt::DirectConnection), "review existing workflow");
    ProductShowcaseMultiVideoWorkflow *active = nullptr;
    for (auto *candidate : pane.findChildren<ProductShowcaseMultiVideoWorkflow *>())
    {
        if (candidate->generationShortCode() == code && candidate->isMerging()) active = candidate;
    }
    require(active != nullptr, "migration schedules final rebuild");
    waitForMerge(*active);
    require(videos.recordsForProject(projectId).size() == 4, "review reuses code without adding records");
    require(finalDir.entryList({"*.mp4"}, QDir::Files) == QStringList{"final_video.mp4"}, "only final video at generation top level");
    for (const auto &take : active->takes())
    {
        require(QFileInfo(take.filePath).dir().dirName() == "takes" && QFile::exists(take.filePath), "candidates remain reviewable in takes folder");
    }
    require(QMetaObject::invokeMethod(&pane, "_resumeMultiVideoGeneration", Qt::DirectConnection), "review a second time");
    require(videos.recordsForProject(projectId).size() == 4, "second review keeps same identity");

    ProductShowcaseMultiVideoWorkflow restored;
    require(restored.loadFromFile(finalDir.filePath("multi_video_workflow.json")), "reload stable workflow identity");
    require(!outputDir.exists("multi_video_workflow.json"), "no redundant manifest beside final generation");
    require(!outputDir.exists("step_1_inputs") && !outputDir.exists("working_step1_take1"),
        "working folders moved out of generation root");
    require(restored.steps()[0].images[0].processedPath == finalDir.filePath("work/step_1_inputs/processed.png"),
        "saved processed image path follows moved inputs");
    require(readFile(restored.steps()[0].images[0].processedPath) == "processed input fixture", "processed image retained");
    require(readFile(finalDir.filePath("work/working_step1_take1/replay_inputs/image_1.png")) == "saved replay image",
        "replay inputs preserved after moving work folder");
    require(!projects->generationDir(row, codes[0], false).exists(), "view never recreates a removed generation folder");
    require(restored.generationShortCode() == code, "short code persisted");
    restored.regenerateStep(1, 1, false);
    auto &newTake = restored.takes().last();
    newTake.filePath = outputDir.filePath(newTake.expectedFileName);
    require(QFile::copy(root.filePath("red.mp4"), newTake.filePath), "new candidate fixture");
    newTake.completed = true;
    auto regenerated = restored.toJson();
    regenerated["state"] = static_cast<int>(AbstractMultiVideoWorkflow::State::Completed);
    restored.fromJson(regenerated);
    QString error;
    require(restored.prepareGeneration(restored.generationShortCode(), &error), "save regenerated candidate into same generation");
    restored.selectTake(1, 1);
    waitForMerge(restored);
    require(restored.generationShortCode() == code, "regeneration preserves short code");
    require(restored.mergedVideoPath() == finalDir.filePath("final_video.mp4"), "regeneration replaces same final video");
    require(videoPixel(restored.mergedVideoPath(), 1.2).red() > 200, "newly selected take appears in final");

    // Published history is not hidden with superseded, unpublished retries.
    videos.setPublished(videos.recordFromShortCode(codes[1])->id, true);
    table->setCurrentIndex(table->model()->index(0, 0));
    table->setCurrentIndex(table->model()->index(row, 0));
    require(tree->topLevelItemCount() == 2, "published older result stays visible");
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary working directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QApplication::setOrganizationName("QuantumSocialChecks");
    QApplication::setApplicationName("GenerationChecks");
    try
    {
        const QDir root(temporary.path());
        WorkingDirectoryManager::instance()->setWorkingDir(root);
        FakeGenerator generator;
        AbstractVideoGenerator::Recorder registration(&generator);
        checkRecipes(root);
        TreeProperties properties(root.filePath("properties.json"));
        TreeProperties archive(root.filePath("archive.json"));
        TableVideos videos(root.path());
        TableGenerationSettings settings(root.path());
        PreferredHashtags hashtags(root.path());
        PromptLessons lessons(root.path());
        FavoriteHooks hooks(root.path());
        FavoriteVideoPrompts prompts(root.path());
        SavedPrompts saved(root.path());
        checkPlan(saved);

        TableProjects projects(root.path());
        const int row = projects.addProject({}, {}, "Test project", {});
        const QUuid projectId = projects.projectId(row);
        const QList<QUuid> propertyIds{QUuid::createUuid()};
        const QUuid seedId = videos.addVideo(propertyIds, {}, "original", projectId);
        const QString seedCode = videos.recordFromId(seedId)->shortCode;
        const QDir seedDir = projects.generationDir(row, seedCode);
        const QDir seedTemp = projects.generationTempDir(row, seedCode);
        writeFile(seedDir.filePath("take.mp4"), "fixture");
        writeFile(seedTemp.filePath("generation_prompt.txt"), "Exact prompt. Video format: vertical (9:16).");
        writeFile(root.filePath("second.png"), "second image");
        VideoGenerationRecipe seed{"test-backend", "Exact prompt. Video format: vertical (9:16).",
            {root.filePath("source.png"), root.filePath("second.png")}, {{"quality", 7}}};
        QString error;
        require(seed.save(seedTemp, &error), "save initial configuration");

        QWidget window;
        PaneGeneration pane(&properties, &archive, &videos, &settings, &hashtags,
                            &lessons, &hooks, &prompts, &saved, &window);
        auto *table = pane.findChild<QTableView *>("tableViewProjects");
        table->setCurrentIndex(table->model()->index(row, 0));
        auto *tree = pane.findChild<QTreeWidget *>("treeViewGenerations");
        require(tree->topLevelItemCount() == 1, "seed generation listed");
        tree->setCurrentItem(tree->topLevelItem(0));
        auto *again = pane.findChild<QPushButton *>("buttonGenerateAgain");
        require(again->isEnabled(), "repeat enabled for saved video");

        answerRepeatDialog(3, false);
        again->click();
        require(generator.calls.isEmpty(), "cancel dialog creates no jobs");

        answerRepeatDialog(3);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 3, "exactly three backend calls");
        require(videos.recordsForProject(projectId).size() == 4, "three separate records");
        require(QSet<QString>(generator.outputs.begin(), generator.outputs.end()).size() == 3,
                "isolated job directories");
        for (const auto &call : generator.calls)
        {
            require(call.prompt == seed.prompt, "unchanged prompt");
            require(call.imagePaths.size() == 2, "both source images included");
            require(call.settings.value("quality").toInt() == 7, "saved backend setting");
            require(call.settings.value("preservePrompt").toBool(), "disable prompt rewriting");
        }
        for (const auto *record : videos.recordsForProject(projectId))
        {
            require(record->propertyValueIds == propertyIds, "same A/B configuration");
            VideoGenerationRecipe replay;
            require(VideoGenerationRecipe::load(projects.generationTempDir(row, record->shortCode),
                                                 &replay, &error), "each output remains repeatable");
            require(readFile(replay.imagePaths[0]) == "original source", "first image preserved");
            require(readFile(replay.imagePaths[1]) == "second image", "second image preserved");
        }

        generator.calls.clear();
        generator.fail = true;
        answerRepeatDialog(3);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "failure stops remaining jobs");
        require(videos.recordsForProject(projectId).size() == 4, "failure creates no record");
        generator.fail = false;

        generator.calls.clear();
        generator.onCall = [&]() {
            QTimer::singleShot(0, [&]() {
                for (auto *button : window.findChildren<QPushButton *>())
                {
                    if (button->text().contains("Cancel") && button->isVisible()) button->click();
                }
            });
        };
        answerRepeatDialog(3);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "cancel stops queued jobs");
        require(videos.recordsForProject(projectId).size() == 5, "cancel preserves completed take");
        generator.onCall = {};
        generator.calls.clear();
        generator.browserFailures = 1;
        answerRepeatDialog(2);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 3, "lost browser retries current job, then runs next job");
        require(videos.recordsForProject(projectId).size() == 7, "browser failure still yields two requested videos");
        require(generator.calls[0].prompt == generator.calls[1].prompt
                && generator.calls[0].imagePaths == generator.calls[1].imagePaths
                && generator.calls[0].settings == generator.calls[1].settings,
                "browser recovery preserves all inputs");

        generator.calls.clear();
        generator.browserFailures = 5;
        answerRepeatDialog(2);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 3, "browser recovery is bounded to two retries");
        require(videos.recordsForProject(projectId).size() == 7, "exhausted recovery creates no record");

        generator.calls.clear();
        generator.browserFailures = 1;
        generator.onCall = [&]() {
            QTimer::singleShot(0, [&]() {
                for (auto *button : window.findChildren<QPushButton *>())
                {
                    if (button->text().contains("Cancel") && button->isVisible()) button->click();
                }
            });
        };
        answerRepeatDialog(2);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "cancel prevents browser recovery and further jobs");
        require(readFile(seedTemp.filePath("generation_prompt.txt")) == seed.prompt.toUtf8(),
                "original generation untouched");
        checkMultiVideo(root);
        checkVideoClips(root);
        checkMultiVideoIdentity(pane, videos, root);

        std::cout << "Generation checks passed: counts, persistence, replay, isolation, failure and cancellation.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
