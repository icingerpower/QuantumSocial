#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
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
        std::cout << "Generation checks passed: counts, persistence, replay, isolation, failure and cancellation.\n";
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
