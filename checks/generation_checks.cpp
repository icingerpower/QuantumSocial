#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QFile>
#include <QFileSystemModel>
#include <QImage>
#include <QJsonArray>
#include <QLineEdit>
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

#include <QListWidget>
#include "gui/DialogGenerateAgain.h"
#include "gui/DialogPickIntroVideo.h"
#include "gui/DialogHooks.h"
#include "AbstractCli.h"
#include "gui/DialogGenerationPlan.h"
#include "gui/GenerationPlanSection.h"
#include "gui/panes/PaneGeneration.h"
#include "gui/panes/PaneInspiration.h"
#include "gui/panes/PaneProperties.h"
#include "gui/panes/PaneStatistics.h"
#include "AvailableCliTable.h"
#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/TableProjects.h"
#include "model/TableAccounts.h"
#include "model/accounts/AbstractSocialAccount.h"
#include "model/properties/TreeProperties.h"
#include "model/videos/SocialStatistics.h"
#include "model/videos/TableVideoStatistics.h"
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
    mutable int rejectionFailures = 0;
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
        if (rejectionFailures > 0)
        {
            --rejectionFailures;
            result.errorMessage = QStringLiteral("Gemini returned text without a video");
            result.rejected = true;
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
    writeFile(root.filePath("saved/generation_prompt.txt"),
              "Known pitfalls from past generations — do NOT repeat these:\n"
              "- Keep shoe straps consistent across frames.\n\n"
              "Actual successful prompt");
    require(QDir().rename(root.filePath("saved"), root.filePath("moved")), "move generation");
    VideoGenerationRecipe loaded;
    require(VideoGenerationRecipe::load(QDir(root.filePath("moved")), &loaded, &error), "load moved recipe");
    require(loaded.prompt == "Actual successful prompt", "prefer actual successful prompt and strip legacy pitfalls");
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

static void checkMultiVideoPromptManagement(SavedPrompts &saved)
{
    const auto clis = AbstractCli::ALL_CLIS();
    saved.savePrompt(QStringLiteral("Fashion Hook 1"), QStringLiteral("Dynamic zoom on high heel shoes"));
    saved.savePrompt(QStringLiteral("Product Spin"), QStringLiteral("360 degree turntable rotation in studio lighting"));

    DialogMultiVideoGeneration dialog{clis, &saved, nullptr};
    require(dialog.savedIntroPromptCombo() != nullptr, "intro prompt combo exists");
    require(dialog.savedAfterIntroPromptCombo() != nullptr, "after-intro prompt combo exists");
    require(dialog.savedIntroPromptCombo()->count() >= 2, "saved prompts loaded into intro combo");
    require(dialog.savedAfterIntroPromptCombo()->count() >= 2, "saved prompts loaded into after-intro combo");

    // Load a prompt into intro
    dialog.savedIntroPromptCombo()->setCurrentText(QStringLiteral("Fashion Hook 1"));
    dialog.loadIntroPromptButton()->click();
    require(dialog.introPrompt() == QStringLiteral("Dynamic zoom on high heel shoes"), "load intro prompt from saved");

    // Load another prompt into after-intro
    dialog.savedAfterIntroPromptCombo()->setCurrentText(QStringLiteral("Product Spin"));
    dialog.loadAfterIntroPromptButton()->click();
    require(dialog.afterIntroPrompt() == QStringLiteral("360 degree turntable rotation in studio lighting"), "load after-intro prompt from saved");

    // Test setting prompts
    dialog.setIntroPrompt(QStringLiteral("Custom unique hook prompt"));
    require(dialog.introPrompt() == QStringLiteral("Custom unique hook prompt"), "setIntroPrompt works");

    dialog.setAfterIntroPrompt(QStringLiteral("Custom unique showcase prompt"));
    require(dialog.afterIntroPrompt() == QStringLiteral("Custom unique showcase prompt"), "setAfterIntroPrompt works");
}

static void checkMultiVideoPropertySelection(TreeProperties &properties, TableVideos &videos, SavedPrompts &saved)
{
    const auto clis = AbstractCli::ALL_CLIS();

    // Add properties and values to properties catalog
    const QModelIndex prop1 = properties.addProperty(QStringLiteral("MultiVideo Background"));
    const QModelIndex val1_1 = properties.addValue(prop1, QStringLiteral("Warm modern living room"),
                                                   QStringLiteral("in a warm modern living room"));
    const QModelIndex val1_2 = properties.addValue(prop1, QStringLiteral("Sunny Street"),
                                                   QStringLiteral("on a sunny city street"));

    const QModelIndex prop2 = properties.addProperty(QStringLiteral("MultiVideo Lighting"));
    const QModelIndex val2_1 = properties.addValue(prop2, QStringLiteral("Studio High-Key"),
                                                   QStringLiteral("with dramatic high-key studio lighting"));

    DialogMultiVideoGeneration dialog{clis, &saved, &properties, &videos, nullptr};

    require(!dialog.introPropertyRows().isEmpty(), "intro property rows populated from TreeProperties");
    require(dialog.introPropertyRows().size() >= 2, "at least 2 property rows present");
    require(dialog.introFinalPreviewLabel() != nullptr, "final preview label exists");

    // Base prompt
    dialog.setIntroPrompt(QStringLiteral("Dynamic zoom on high heel shoes"));
    dialog.setAfterIntroPrompt(QStringLiteral("360 degree turntable rotation"));

    // Both properties checked by default
    require(dialog.introPropertyRows()[0].checkBox->isChecked(), "property 1 checked by default");
    require(dialog.introPropertyRows()[1].checkBox->isChecked(), "property 2 checked by default");

    QString introPrompt = dialog.introPrompt();
    require(introPrompt.contains(QStringLiteral("Dynamic zoom on high heel shoes.")), "introPrompt starts with base prompt");
    require(introPrompt.contains(dialog.introPropertyRows()[0].valueCombo->currentData(Qt::UserRole + 1).toString()), "introPrompt contains property 1 fragment");
    require(introPrompt.contains(dialog.introPropertyRows()[1].valueCombo->currentData(Qt::UserRole + 1).toString()), "introPrompt contains property 2 fragment");

    // Live preview label should match introPrompt()
    require(dialog.introFinalPreviewLabel()->text() == introPrompt, "live preview label matches computed intro prompt");

    // After-intro prompt should NOT contain any property fragments
    require(dialog.afterIntroPrompt() == QStringLiteral("360 degree turntable rotation"), "after-intro prompt has no property fragments");

    // Raw prompt should return base text only
    require(dialog.rawIntroPrompt() == QStringLiteral("Dynamic zoom on high heel shoes"), "rawIntroPrompt returns base text");

    // Uncheck property 2: prompt should no longer contain property 2 fragment
    dialog.introPropertyRows()[1].checkBox->setChecked(false);
    require(!dialog.introPropertyRows()[1].valueCombo->isEnabled(), "unchecking disables combo box");
    require(!dialog.introPrompt().contains(dialog.introPropertyRows()[1].valueCombo->currentData(Qt::UserRole + 1).toString()), "introPrompt excludes unchecked property fragment");
    require(dialog.introFinalPreviewLabel()->text() == dialog.introPrompt(), "live preview label updates when property unchecked");

    // Change property 1 value in combo
    auto &row0 = dialog.introPropertyRows()[0];
    const int newIndex = (row0.valueCombo->currentIndex() == 0) ? 1 : 0;
    row0.valueCombo->setCurrentIndex(newIndex);
    const QString newFragment = row0.valueCombo->itemData(newIndex, Qt::UserRole + 1).toString();
    require(dialog.introPrompt().contains(newFragment), "introPrompt contains newly selected property fragment");

    // introPropertyValueIds() should contain checked property value UUID
    const QList<QUuid> valueIds = dialog.introPropertyValueIds();
    require(valueIds.contains(row0.valueCombo->currentData(Qt::UserRole).toUuid()), "introPropertyValueIds contains selected property 1 value UUID");
    require(!valueIds.contains(dialog.introPropertyRows()[1].valueCombo->currentData(Qt::UserRole).toUuid()), "introPropertyValueIds does NOT contain unchecked property 2 value UUID");

    // Uncheck ALL properties: introPrompt() returns exact base prompt
    dialog.introPropertyRows()[0].checkBox->setChecked(false);
    require(dialog.introPrompt() == QStringLiteral("Dynamic zoom on high heel shoes"), "all properties unchecked returns exact base prompt");
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

static void answerRepeatDialog(int count, bool accept = true, const QString &extraPrompt = QString())
{
    QTimer::singleShot(0, [count, accept, extraPrompt]() {
        bool found = false;
        for (auto *widget : QApplication::topLevelWidgets())
        {
            if (auto *dialog = dynamic_cast<DialogGenerateAgain *>(widget))
            {
                dialog->findChild<QSpinBox *>("spinCount")->setValue(count);
                if (!extraPrompt.isNull())
                {
                    dialog->setExtraPrompt(extraPrompt);
                }
                if (accept) dialog->accept(); else dialog->reject();
                found = true;
            }
        }
        require(found, "repeat dialog opened");
    });
}

static void checkExtraPromptPhrase()
{
    // Empty or whitespace
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy"), QString())
            == QStringLiteral("A cute puppy"), "empty extra prompt returns base");
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy"), QStringLiteral("   "))
            == QStringLiteral("A cute puppy"), "whitespace extra prompt returns base");
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy. Video format: vertical (9:16)."), QStringLiteral("  "))
            == QStringLiteral("A cute puppy. Video format: vertical (9:16)."), "whitespace with format returns base");

    // Plain prompt without format suffix
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy"), QStringLiteral("wearing sunglasses"))
            == QStringLiteral("A cute puppy. wearing sunglasses."), "appends with periods");
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy."), QStringLiteral("wearing sunglasses."))
            == QStringLiteral("A cute puppy. wearing sunglasses."), "avoids double periods");
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy!"), QStringLiteral("wearing sunglasses"))
            == QStringLiteral("A cute puppy! wearing sunglasses."), "preserves existing terminal exclamation");

    // Prompt with format suffix
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy. Video format: vertical (9:16)."), QStringLiteral("wearing sunglasses"))
            == QStringLiteral("A cute puppy. wearing sunglasses. Video format: vertical (9:16)."), "inserts before format suffix");
    require(PaneGeneration::appendExtraPromptPhrase(QStringLiteral("A cute puppy Video format: vertical (9:16)."), QStringLiteral("wearing sunglasses."))
            == QStringLiteral("A cute puppy. wearing sunglasses. Video format: vertical (9:16)."), "inserts cleanly when base lacks period before format suffix");

    // DialogGenerateAgain extra prompt getter/setter
    DialogGenerateAgain repeatDlg(QStringLiteral("Test configuration"));
    require(repeatDlg.extraPrompt().isEmpty(), "DialogGenerateAgain default extraPrompt is empty");
    repeatDlg.setExtraPrompt(QStringLiteral("warm sunset lighting"));
    require(repeatDlg.extraPrompt() == QStringLiteral("warm sunset lighting"), "DialogGenerateAgain setExtraPrompt");

    // DialogPickIntroVideo extra prompt
    AbstractMultiVideoWorkflow::TakeInfo sampleTake;
    sampleTake.stepIndex = 0;
    sampleTake.takeIndex = 0;
    sampleTake.expectedFileName = QStringLiteral("take_1.mp4");
    sampleTake.completed = true;
    DialogPickIntroVideo pickDlg(0, QStringLiteral("Intro"), {sampleTake}, 0, nullptr);
    require(pickDlg.extraPrompt().isEmpty(), "DialogPickIntroVideo default extraPrompt is empty");
    require(pickDlg.extraPromptEdit() != nullptr, "DialogPickIntroVideo extraPromptEdit is accessible");
    pickDlg.extraPromptEdit()->setText(QStringLiteral("cinematic slow motion"));
    emit pickDlg.extraPromptEdit()->returnPressed();
    require(pickDlg.isRegenerateRequested(), "returnPressed on lineEditExtraPrompt triggers regenerate");
    require(pickDlg.extraPrompt() == QStringLiteral("cinematic slow motion"), "DialogPickIntroVideo captured extra prompt on returnPressed");
}

static void checkDialogHooksPickQuote(const QDir &root)
{
    FavoriteHooks hooks(root.path());
    hooks.setHooks({QStringLiteral("The heel that owns the room."), QStringLiteral("Step into power.")});

    DialogHooks::Context context;
    context.favoriteHooks = hooks.hooks();
    context.preferredHashtags = {QStringLiteral("#fallbackTag")};
    context.codeTag = QStringLiteral("#qs0001");

    QList<QPair<QString, QString>> existingHooks;
    existingHooks.append(qMakePair(
        QStringLiteral("First Hook"),
        QStringLiteral("Stunning heels for your night out! #shoes #fashion #qs0001")));

    DialogHooks dlg(existingHooks, context);

    auto *btnPick = dlg.findChild<QPushButton *>("buttonPickQuote");
    require(btnPick != nullptr, "buttonPickQuote exists in DialogHooks");
    require(btnPick->text() == QStringLiteral("Pick quote"), "buttonPickQuote text is 'Pick quote'");

    // 1. Simulate clicking "Pick quote" and accepting DialogPickQuote with first item
    QTimer::singleShot(50, []() {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *picker = qobject_cast<QDialog *>(widget))
            {
                if (picker->windowTitle() == QStringLiteral("Pick Quote"))
                {
                    auto *list = picker->findChild<QListWidget *>();
                    if (list && list->count() > 0)
                    {
                        list->setCurrentRow(0); // "The heel that owns the room."
                        picker->accept();
                        return;
                    }
                }
            }
        }
    });

    btnPick->click();

    require(dlg.allHooks().size() == 2, "hook row added after picking quote");
    require(dlg.allHooks()[1].first == QStringLiteral("The heel that owns the room."),
            "new row hook column matches picked quote");
    require(dlg.allHooks()[1].second == QStringLiteral("The heel that owns the room. #shoes #fashion #qs0001"),
            "new row description column has quote + hashtags of first line");
    require(dlg.selectedHook() == QStringLiteral("The heel that owns the room."),
            "selectedHook reflects picked quote");
    require(dlg.selectedDescription() == QStringLiteral("The heel that owns the room. #shoes #fashion #qs0001"),
            "selectedDescription reflects picked quote with first line hashtags");

    // 2. Test empty initial table: uses fallback hashtags and code tag
    DialogHooks dlgEmpty({}, context);
    auto *btnPickEmpty = dlgEmpty.findChild<QPushButton *>("buttonPickQuote");
    require(btnPickEmpty != nullptr, "buttonPickQuote exists on empty dialog");

    QTimer::singleShot(50, []() {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *picker = qobject_cast<QDialog *>(widget))
            {
                if (picker->windowTitle() == QStringLiteral("Pick Quote"))
                {
                    auto *list = picker->findChild<QListWidget *>();
                    if (list && list->count() > 1)
                    {
                        list->setCurrentRow(1); // "Step into power."
                        picker->accept();
                        return;
                    }
                }
            }
        }
    });

    btnPickEmpty->click();

    require(dlgEmpty.allHooks().size() == 1, "hook row added to empty dialog");
    require(dlgEmpty.allHooks()[0].first == QStringLiteral("Step into power."),
            "hook column matches second picked quote");
    require(dlgEmpty.allHooks()[0].second == QStringLiteral("Step into power. #fallbackTag #qs0001"),
            "description column has quote + fallback hashtags + code tag");

    // 3. Test search filter in DialogPickQuote
    DialogHooks dlgFilter({}, context);
    auto *btnPickFilter = dlgFilter.findChild<QPushButton *>("buttonPickQuote");
    QTimer::singleShot(50, []() {
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *picker = qobject_cast<QDialog *>(widget))
            {
                if (picker->windowTitle() == QStringLiteral("Pick Quote"))
                {
                    auto *edit = picker->findChild<QLineEdit *>();
                    auto *list = picker->findChild<QListWidget *>();
                    if (edit && list)
                    {
                        edit->setText(QStringLiteral("power"));
                        require(list->item(0)->isHidden(), "item 0 filtered out");
                        require(!list->item(1)->isHidden(), "item 1 matches filter");
                        picker->accept();
                        return;
                    }
                }
            }
        }
    });

    btnPickFilter->click();

    require(dlgFilter.allHooks().size() == 1, "hook row added after filter");
    require(dlgFilter.allHooks()[0].first == QStringLiteral("Step into power."),
            "filtered quote was picked");

    // 4. Test new code tag format without '#' (e.g. "qs0001") and max 5 hashtags
    {
        DialogHooks::Context contextNoHash = context;
        contextNoHash.codeTag = QStringLiteral("qs0001");

        QList<QPair<QString, QString>> legacyHooks;
        // First description has 6 hashtags + legacy #qs0001
        legacyHooks.append(qMakePair(
            QStringLiteral("First Hook"),
            QStringLiteral("Awesome outfit! #tag1 #tag2 #tag3 #tag4 #tag5 #tag6 #qs0001")));

        DialogHooks dlgNoHash(legacyHooks, contextNoHash);
        auto *btnPickNoHash = dlgNoHash.findChild<QPushButton *>("buttonPickQuote");
        require(btnPickNoHash != nullptr, "buttonPickQuote exists");

        QTimer::singleShot(50, []() {
            for (QWidget *widget : QApplication::topLevelWidgets())
            {
                if (auto *picker = qobject_cast<QDialog *>(widget))
                {
                    if (picker->windowTitle() == QStringLiteral("Pick Quote"))
                    {
                        auto *list = picker->findChild<QListWidget *>();
                        if (list && list->count() > 0)
                        {
                            list->setCurrentRow(0); // "The heel that owns the room."
                            picker->accept();
                            return;
                        }
                    }
                }
            }
        });

        btnPickNoHash->click();

        require(dlgNoHash.allHooks().size() == 2, "hook row added");
        // Verify: capped to 5 hashtags (#tag1..#tag5), #qs0001 stripped as code tag, qs0001 appended without '#'
        require(dlgNoHash.allHooks()[1].second == QStringLiteral("The heel that owns the room. #tag1 #tag2 #tag3 #tag4 #tag5 qs0001"),
                "new row description capped to 5 hashtags and uses qs0001 without '#'");
    }
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
    require(projects.data(projects.index(singleRow, TableProjects::IND_TYPE), Qt::DisplayRole).toString()
            == QStringLiteral("Single"), "standard project displays 'Single'");
    require(projects.data(projects.index(multiRow, TableProjects::IND_TYPE), Qt::DisplayRole).toString()
            == QStringLiteral("Multi-video"), "multi-video project displays 'Multi-video'");
    require(TableProjects::formatTypeLabel(QStringLiteral("custom_kind")) == QStringLiteral("Custom-kind"),
            "future project kind formatted properly");

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

static void checkRegeneratedStepOrder(const QDir &root)
{
    for (const bool replace : {false, true})
    {
        ProductShowcaseMultiVideoWorkflow workflow;
        workflow.configure(3, "Intro", 1, "Show attached products", 1);
        workflow.takes()[0].completed = true;
        workflow.takes()[1].completed = true;
        workflow.setSelectedTakeIndex(0, 0);
        workflow.regenerateStep(1, 2, replace);
        const auto &takes = workflow.takes();
        require(takes.last().stepIndex == 2, "step 2 regeneration precedes unfinished step 3");
        require(takes[1].stepIndex == 1 && takes[1].takeIndex == 0, "step 2 take IDs preserved");
        require(takes[2].stepIndex == 1 && takes[2].takeIndex == 1 && !takes[2].completed,
                "new step 2 take ready before step 3");
        require(takes[1].completed == !replace, "append preserves old take; replacement resets it");

        // Reproduce the old on-disk order: regenerated step 2 takes appended
        // after step 3. Loading must repair execution order without renaming.
        auto legacy = workflow.toJson();
        auto legacyTakes = legacy["takes"].toArray();
        legacyTakes.insert(1, legacyTakes.takeAt(legacyTakes.size() - 1));
        legacy["takes"] = legacyTakes;
        ProductShowcaseMultiVideoWorkflow restored;
        require(restored.fromJson(legacy), "load legacy regeneration queue");
        require(restored.takes().last().stepIndex == 2, "legacy queue restored in step order");
        require(restored.takes()[2].expectedFileName == workflow.videoFileName(1, 1),
                "legacy take filenames preserved");
        const QString manifest = root.filePath("regeneration_order.json");
        require(restored.saveToFile(manifest), "save repaired regeneration queue");
        ProductShowcaseMultiVideoWorkflow reloaded;
        require(reloaded.loadFromFile(manifest), "reload repaired regeneration queue");
        require(reloaded.takes().last().stepIndex == 2, "step order survives save and reload");

        int startedStep = -1;
        QObject::connect(&reloaded, &AbstractMultiVideoWorkflow::takeStarted, &reloaded,
                         [&](int stepIndex, int, const QString &) {
            startedStep = stepIndex;
            reloaded.cancel(); // Check scheduling without contacting a provider.
        });
        QTemporaryDir output;
        require(output.isValid(), "temporary regeneration output");
        reloaded.start(output.path(), output.path(), "test-backend", {}, nullptr, nullptr, {});
        QElapsedTimer timer;
        timer.start();
        while (reloaded.state() == AbstractMultiVideoWorkflow::State::Running && timer.elapsed() < 2000)
        {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        require(startedStep == 1, "resume actually starts regenerated step 2 before step 3");
        require(reloaded.state() == AbstractMultiVideoWorkflow::State::Cancelled,
                "scheduling check stops before generation");
    }
}

static void checkStepSelectionGates()
{
    for (const int count : {1, 2})
    {
        QTemporaryDir output;
        require(output.isValid(), "temporary step selection output");
        ProductShowcaseMultiVideoWorkflow workflow;
        workflow.configure(3, "Intro", 1, "Products", count);
        for (auto &take : workflow.takes())
        {
            take.completed = take.stepIndex < 2;
            take.conversationUrl = QStringLiteral("https://gemini.google.com/app/step%1_take%2")
                .arg(take.stepIndex).arg(take.takeIndex);
        }

        int requestedStep = -1;
        int startedStep = -1;
        QObject::connect(&workflow, &AbstractMultiVideoWorkflow::takeStarted, &workflow,
                         [&](int step, int, const QString &) { startedStep = step; });
        QObject::connect(&workflow, &AbstractMultiVideoWorkflow::introSelectionRequested, &workflow,
                         [&](const auto &takes) {
            require(takes.size() == 1, "single intro still requires approval");
            requestedStep = 0;
        });
        QObject::connect(&workflow, &AbstractMultiVideoWorkflow::takeSelectionRequested, &workflow,
                         [&](int step, const auto &takes) {
            require(takes.size() == count, "picker contains current step takes");
            requestedStep = step;
        });
        workflow.start(output.path(), output.path(), "test-backend", {}, nullptr, nullptr, {});
        QApplication::processEvents();
        require(workflow.state() == AbstractMultiVideoWorkflow::State::Paused && requestedStep == 0,
                "resume gates an unselected intro even with later takes already saved");
        require(startedStep == -1, "no next step starts before intro approval");

        workflow.selectIntroTake(0);
        QApplication::processEvents();
        require(workflow.state() == AbstractMultiVideoWorkflow::State::Paused && requestedStep == 1,
                "step 2 must be approved before step 3, for single and multiple takes");
        require(startedStep == -1, "step 3 stays blocked while step 2 picker is open");
        requestedStep = -1;
        workflow.resume(); // Equivalent to dismissing the picker and resuming later.
        QApplication::processEvents();
        require(requestedStep == 1 && startedStep == -1, "resume cannot bypass step 2 selection");
        workflow.selectTake(1, 999);
        require(workflow.state() == AbstractMultiVideoWorkflow::State::Paused,
                "invalid take cannot release the selection gate");

        ProductShowcaseMultiVideoWorkflow restored;
        require(restored.loadFromFile(QDir(output.path()).filePath("multi_video_workflow.json")),
                "reload project waiting for step 2 selection");
        requestedStep = -1;
        QObject::connect(&restored, &AbstractMultiVideoWorkflow::takeSelectionRequested, &restored,
                         [&](int step, const auto &) { requestedStep = step; });
        QObject::connect(&restored, &AbstractMultiVideoWorkflow::takeStarted, &restored,
                         [&](int step, int, const QString &) {
            startedStep = step;
            restored.cancel(); // Observe scheduling without contacting a provider.
        });
        restored.start(output.path(), output.path(), "test-backend", {}, nullptr, nullptr, {});
        QApplication::processEvents();
        require(requestedStep == 1 && startedStep == -1, "saved selection gate survives restart");
        restored.selectTake(1, count - 1);
        QApplication::processEvents();
        require(startedStep == 2, "choosing step 2 releases step 3 generation");
        require(restored.selectedTakeIndex(1) == count - 1, "chosen step 2 take is persisted");
        require(restored.conversationUrl().endsWith(QStringLiteral("step1_take%1").arg(count - 1)),
                "continuation uses the selected step conversation");

        restored.regenerateStep(1, 1);
        startedStep = -1;
        restored.resume();
        QApplication::processEvents();
        require(startedStep == 1, "regeneration stays in step 2 before advancing");
        // Simulate the downloaded replacement so the gate can be checked
        // without a real generator, browser session, or quota consumption.
        for (auto &take : restored.takes())
        {
            if (take.stepIndex == 1) take.completed = true;
        }
        startedStep = -1;
        requestedStep = -1;
        restored.resume();
        QApplication::processEvents();
        require(requestedStep == 1 && startedStep == -1,
                "regenerated step must be selected again before advancing");
    }

    // Older projects may already contain every later video when an earlier
    // step is regenerated. Selecting that step must still finish the workflow.
    QTemporaryDir output;
    ProductShowcaseMultiVideoWorkflow completed;
    completed.configure(3, "Intro", 1, "Products", 1);
    for (auto &take : completed.takes()) take.completed = true;
    completed.setSelectedTakeIndex(0, 0);
    int completions = 0;
    QObject::connect(&completed, &AbstractMultiVideoWorkflow::allCompleted, &completed,
                     [&](const auto &) { ++completions; });
    completed.start(output.path(), output.path(), "test-backend", {}, nullptr, nullptr, {});
    require(completed.state() == AbstractMultiVideoWorkflow::State::Paused,
            "legacy completed videos still require missing intermediate selection");
    completed.selectTake(1, 0);
    require(completed.state() == AbstractMultiVideoWorkflow::State::Completed && completions == 1,
            "selection finishes the workflow even when no more takes need generation");
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
    require(!table->isColumnHidden(TableProjects::IND_NAME), "project name column is visible");
    require(!table->isColumnHidden(TableProjects::IND_TYPE), "project type column is visible");
    require(table->isColumnHidden(TableProjects::IND_CREATED), "project created column is hidden");
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
    require(tree->currentItem()->childCount() >= 2, "multi-video workflow displays step prompt items");
    auto *step1Child = tree->currentItem()->child(0);
    require(step1Child->data(0, PaneGeneration::RoleIsPromptItem).toBool(), "step 1 child is prompt item");
    require(step1Child->data(0, PaneGeneration::RolePromptText).toString() == QStringLiteral("Intro"), "step 1 prompt matches");
    require(step1Child->isFirstColumnSpanned(), "step 1 prompt spans all columns");
    auto *step2Child = tree->currentItem()->child(1);
    require(step2Child->data(0, PaneGeneration::RoleIsPromptItem).toBool(), "step 2 child is prompt item");
    require(step2Child->data(0, PaneGeneration::RolePromptText).toString() == QStringLiteral("Product"), "step 2 prompt matches");
    tree->setCurrentItem(step1Child);
    require(QMetaObject::invokeMethod(&pane, "_copySelectedGenerationPrompt", Qt::DirectConnection), "copy prompt shortcut invoked");
    require(QGuiApplication::clipboard()->text() == QStringLiteral("Intro"), "copied step prompt to clipboard");
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

static void checkInspirationModelsAndPane(const QDir &parentRoot)
{
    QTemporaryDir tempInspiration;
    require(tempInspiration.isValid(), "temporary inspiration working directory");
    const QDir root(tempInspiration.path());
    WorkingDirectoryManager::instance()->setWorkingDir(root);

    // 1. Test PreferredHashtags table model, addRow, addHashtag, setData, removeRows, and persistence
    {
        PreferredHashtags hashtags(root.path());
        hashtags.setHashtags({});
        require(hashtags.rowCount() == 0, "hashtags starts empty");
        require(hashtags.columnCount() == 2, "hashtags has 2 columns (tag, source)");

        hashtags.addHashtag("#firstTag", PreferredHashtags::SOURCE_HUMAN);
        hashtags.addHashtag("#secondTag", PreferredHashtags::SOURCE_AI);
        require(hashtags.rowCount() == 2, "two hashtags added");
        require(hashtags.data(hashtags.index(0, PreferredHashtags::IND_TAG)).toString() == "#firstTag", "tag 0 correct");
        require(hashtags.data(hashtags.index(0, PreferredHashtags::IND_SOURCE)).toString() == PreferredHashtags::SOURCE_HUMAN, "source 0 is Human");
        require(hashtags.data(hashtags.index(1, PreferredHashtags::IND_TAG)).toString() == "#secondTag", "tag 1 correct");
        require(hashtags.data(hashtags.index(1, PreferredHashtags::IND_SOURCE)).toString() == PreferredHashtags::SOURCE_AI, "source 1 is AI");

        // Verify onlyHuman filtering
        require(hashtags.hashtags(false).size() == 2, "both hashtags returned when onlyHuman is false");
        require(hashtags.hashtags(true).size() == 1, "only 1 hashtag returned when onlyHuman is true");
        require(hashtags.hashtags(true).first() == "#firstTag", "human hashtag is #firstTag");
        require(hashtags.asPromptSection(true).contains("#firstTag"), "human prompt section contains #firstTag");
        require(!hashtags.asPromptSection(true).contains("#secondTag"), "human prompt section does not contain #secondTag");
        require(hashtags.asPromptSection(false).contains("#secondTag"), "both prompt section contains #secondTag");

        // Edit via setData
        hashtags.setData(hashtags.index(0, PreferredHashtags::IND_SOURCE), PreferredHashtags::SOURCE_AI, Qt::EditRole);
        require(hashtags.data(hashtags.index(0, PreferredHashtags::IND_SOURCE)).toString() == PreferredHashtags::SOURCE_AI, "source edited to AI");

        // addRow
        hashtags.addRow(PreferredHashtags::SOURCE_HUMAN);
        require(hashtags.rowCount() == 3, "addRow creates row");

        // Reload from disk to verify persistence
        PreferredHashtags reloaded(root.path());
        require(reloaded.rowCount() == 3, "reloaded has 3 items");
        require(reloaded.data(reloaded.index(0, PreferredHashtags::IND_SOURCE)).toString() == PreferredHashtags::SOURCE_AI, "reloaded item 0 has AI source");

        // removeRows
        hashtags.removeRow(1);
        require(hashtags.rowCount() == 2, "removed row 1");
    }

    // 2. Test FavoriteHooks table model, persistence, and asPromptSection
    {
        FavoriteHooks hooks(root.path());
        hooks.setHooks({});
        require(hooks.rowCount() == 0, "hooks starts empty");
        require(hooks.columnCount() == 2, "hooks has 2 columns");

        hooks.addHook("The heel that owns the room.", FavoriteHooks::SOURCE_HUMAN);
        hooks.addHook("Step into power.", FavoriteHooks::SOURCE_AI);
        require(hooks.rowCount() == 2, "two hooks added");
        require(hooks.data(hooks.index(0, FavoriteHooks::IND_HOOK)).toString() == "The heel that owns the room.", "hook 0 text");
        require(hooks.data(hooks.index(0, FavoriteHooks::IND_SOURCE)).toString() == FavoriteHooks::SOURCE_HUMAN, "hook 0 source");
        require(hooks.data(hooks.index(1, FavoriteHooks::IND_SOURCE)).toString() == FavoriteHooks::SOURCE_AI, "hook 1 source");

        // Verify clean hooks() list and onlyHuman filtering
        const QStringList hookList = hooks.hooks();
        require(hookList.size() == 2, "hookList size 2");
        require(hookList[0] == "The heel that owns the room.", "hook 0 in list");
        require(!hooks.asPromptSection().contains("[Human]"), "prompt section has no [Human] tag");
        require(hooks.hooks(true).size() == 1, "only 1 hook returned when onlyHuman is true");
        require(hooks.asPromptSection(true).contains("The heel that owns the room."), "human section contains human hook");
        require(!hooks.asPromptSection(true).contains("Step into power."), "human section does not contain AI hook");
        require(hooks.asPromptSection(false).contains("Step into power."), "both section contains AI hook");

        // Reload from disk to verify markdown persistence with [Human]/[AI]
        FavoriteHooks reloaded(root.path());
        require(reloaded.rowCount() == 2, "reloaded hooks size 2");
        require(reloaded.data(reloaded.index(1, FavoriteHooks::IND_SOURCE)).toString() == FavoriteHooks::SOURCE_AI, "reloaded hook 1 is AI");
    }

    // 3. Test FavoriteVideoPrompts table model and persistence
    {
        FavoriteVideoPrompts prompts(root.path());
        prompts.setPrompts({});
        require(prompts.rowCount() == 0, "prompts starts empty");
        require(prompts.columnCount() == 2, "prompts has 2 columns");

        prompts.addPrompt("Fast cut on the heels, vertical 9:16.", FavoriteVideoPrompts::SOURCE_HUMAN);
        prompts.addPrompt("Dynamic zoom and beat sync.", FavoriteVideoPrompts::SOURCE_AI);
        require(prompts.rowCount() == 2, "two prompts added");
        require(prompts.data(prompts.index(0, FavoriteVideoPrompts::IND_SOURCE)).toString() == FavoriteVideoPrompts::SOURCE_HUMAN, "prompt 0 source");
        require(prompts.data(prompts.index(1, FavoriteVideoPrompts::IND_SOURCE)).toString() == FavoriteVideoPrompts::SOURCE_AI, "prompt 1 source");

        // Verify onlyHuman filtering
        require(prompts.prompts(false).size() == 2, "both prompts returned");
        require(prompts.prompts(true).size() == 1, "only 1 prompt returned when onlyHuman is true");
        require(prompts.asPromptSection(true).contains("Fast cut on the heels, vertical 9:16."), "human section contains human prompt");
        require(!prompts.asPromptSection(true).contains("Dynamic zoom and beat sync."), "human section does not contain AI prompt");
        require(prompts.asPromptSection(false).contains("Dynamic zoom and beat sync."), "both section contains AI prompt");

        // Reload from disk
        FavoriteVideoPrompts reloaded(root.path());
        require(reloaded.rowCount() == 2, "reloaded prompts size 2");
        require(reloaded.data(reloaded.index(1, FavoriteVideoPrompts::IND_SOURCE)).toString() == FavoriteVideoPrompts::SOURCE_AI, "reloaded prompt 1 is AI");
    }

    // 4. Test PaneInspiration setup, spin boxes, only-human checkboxes, and settings persistence
    {
        PreferredHashtags hashtags(root.path());
        FavoriteHooks hooks(root.path());
        FavoriteVideoPrompts prompts(root.path());
        AvailableCliTable cliTable;
        {
            PaneInspiration pane(&hashtags, &hooks, &prompts, &cliTable);
            auto *tabWidget = pane.findChild<QTabWidget *>("tabWidgetInspiration");
            require(tabWidget != nullptr, "PaneInspiration has tabWidgetInspiration");
            require(tabWidget->count() == 3, "PaneInspiration has 3 tabs");
            require(pane.findChild<QTableView *>("tableViewHashtags") != nullptr, "has tableViewHashtags");
            require(pane.findChild<QTableView *>("tableViewHooks") != nullptr, "has tableViewHooks");
            require(pane.findChild<QTableView *>("tableViewPrompts") != nullptr, "has tableViewPrompts");
            require(pane.findChild<QComboBox *>("comboCliHashtag") != nullptr, "has comboCliHashtag");
            require(pane.findChild<QPlainTextEdit *>("editPromptHashtag") != nullptr, "has editPromptHashtag");
            require(!pane.findChild<QPlainTextEdit *>("editPromptHashtag")->toPlainText().isEmpty(), "editPromptHashtag prefilled with default prompt");

            // Verify spin boxes default to 10
            auto *spinHashtag = pane.findChild<QSpinBox *>("spinCountHashtag");
            require(spinHashtag != nullptr, "has spinCountHashtag");
            require(spinHashtag->value() == 10, "spinCountHashtag defaults to 10");

            auto *spinHook = pane.findChild<QSpinBox *>("spinCountHook");
            require(spinHook != nullptr, "has spinCountHook");
            require(spinHook->value() == 10, "spinCountHook defaults to 10");

            auto *spinPrompt = pane.findChild<QSpinBox *>("spinCountPrompt");
            require(spinPrompt != nullptr, "has spinCountPrompt");
            require(spinPrompt->value() == 10, "spinCountPrompt defaults to 10");

            // Verify onlyHuman checkboxes default to unchecked (both human and AI)
            auto *checkHashtag = pane.findChild<QCheckBox *>("checkOnlyHumanHashtag");
            require(checkHashtag != nullptr, "has checkOnlyHumanHashtag");
            require(!checkHashtag->isChecked(), "checkOnlyHumanHashtag defaults to unchecked");

            auto *checkHook = pane.findChild<QCheckBox *>("checkOnlyHumanHook");
            require(checkHook != nullptr, "has checkOnlyHumanHook");
            require(!checkHook->isChecked(), "checkOnlyHumanHook defaults to unchecked");

            auto *checkPrompt = pane.findChild<QCheckBox *>("checkOnlyHumanPrompt");
            require(checkPrompt != nullptr, "has checkOnlyHumanPrompt");
            require(!checkPrompt->isChecked(), "checkOnlyHumanPrompt defaults to unchecked");

            // Change spin box and checkbox values to test persistence to working directory settings
            spinHashtag->setValue(15);
            spinHook->setValue(7);
            spinPrompt->setValue(20);
            checkHashtag->setChecked(true);
            checkPrompt->setChecked(true);

            // Change prompt text to test persistence to working directory settings
            auto *editPromptHashtag = pane.findChild<QPlainTextEdit *>("editPromptHashtag");
            auto *editPromptHook = pane.findChild<QPlainTextEdit *>("editPromptHook");
            auto *editPromptPrompt = pane.findChild<QPlainTextEdit *>("editPromptPrompt");
            require(editPromptHashtag != nullptr, "has editPromptHashtag");
            require(editPromptHook != nullptr, "has editPromptHook");
            require(editPromptPrompt != nullptr, "has editPromptPrompt");

            editPromptHashtag->setPlainText("Custom hashtag prompt for testing");
            editPromptHook->setPlainText("Custom hook prompt for testing");
            editPromptPrompt->setPlainText("Custom video prompt for testing");
        }

        // Verify values were persisted in working directory settings
        auto settings = WorkingDirectoryManager::instance()->settings();
        require(settings->value("Inspiration/HashtagsCount").toInt() == 15, "HashtagsCount persisted as 15");
        require(settings->value("Inspiration/HooksCount").toInt() == 7, "HooksCount persisted as 7");
        require(settings->value("Inspiration/PromptsCount").toInt() == 20, "PromptsCount persisted as 20");
        require(settings->value("Inspiration/HashtagsOnlyHuman").toBool() == true, "HashtagsOnlyHuman persisted as true");
        require(settings->value("Inspiration/HooksOnlyHuman").toBool() == false, "HooksOnlyHuman persisted as false");
        require(settings->value("Inspiration/PromptsOnlyHuman").toBool() == true, "PromptsOnlyHuman persisted as true");
        require(settings->value("Inspiration/HashtagsPrompt").toString() == "Custom hashtag prompt for testing", "HashtagsPrompt persisted");
        require(settings->value("Inspiration/HooksPrompt").toString() == "Custom hook prompt for testing", "HooksPrompt persisted");
        require(settings->value("Inspiration/PromptsPrompt").toString() == "Custom video prompt for testing", "PromptsPrompt persisted");

        // Open a new PaneInspiration and verify restored values
        {
            PaneInspiration paneRestored(&hashtags, &hooks, &prompts, &cliTable);
            require(paneRestored.findChild<QSpinBox *>("spinCountHashtag")->value() == 15, "restored spinCountHashtag is 15");
            require(paneRestored.findChild<QSpinBox *>("spinCountHook")->value() == 7, "restored spinCountHook is 7");
            require(paneRestored.findChild<QSpinBox *>("spinCountPrompt")->value() == 20, "restored spinCountPrompt is 20");
            require(paneRestored.findChild<QCheckBox *>("checkOnlyHumanHashtag")->isChecked() == true, "restored checkOnlyHumanHashtag is checked");
            require(paneRestored.findChild<QCheckBox *>("checkOnlyHumanHook")->isChecked() == false, "restored checkOnlyHumanHook is unchecked");
            require(paneRestored.findChild<QCheckBox *>("checkOnlyHumanPrompt")->isChecked() == true, "restored checkOnlyHumanPrompt is checked");
            require(paneRestored.findChild<QPlainTextEdit *>("editPromptHashtag")->toPlainText() == "Custom hashtag prompt for testing", "restored editPromptHashtag");
            require(paneRestored.findChild<QPlainTextEdit *>("editPromptHook")->toPlainText() == "Custom hook prompt for testing", "restored editPromptHook");
            require(paneRestored.findChild<QPlainTextEdit *>("editPromptPrompt")->toPlainText() == "Custom video prompt for testing", "restored editPromptPrompt");
        }
    }

    // Restore working directory to parentRoot
    WorkingDirectoryManager::instance()->setWorkingDir(parentRoot);
}

class FakeSocialAccount : public AbstractSocialAccount
{
public:
    mutable AccountVideosResult nextAccountVideosResult;
    mutable VideoStatistics nextVideoResult;

    QString getId() const override { return QStringLiteral("test-social"); }
    QString getName() const override { return QStringLiteral("TestSocial"); }

    QCoro::Task<Statistics> fetchStatistics(const QString &) const override
    {
        Statistics r;
        r.followers = nextAccountVideosResult.followers;
        r.views = nextAccountVideosResult.views;
        r.likes = nextAccountVideosResult.likes;
        co_return r;
    }

    QSet<StatSource> supportedVideoStatSources() const override
    {
        return {StatSource::PublicView};
    }

    QCoro::Task<VideoStatistics> fetchVideoStatistics(const QString &, StatSource) const override
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(5));
        co_return nextVideoResult;
    }

    bool supportsAccountVideoStatistics() const override
    {
        return true;
    }

    mutable QStringList lastKnownShortCodes;

    QCoro::Task<AccountVideosResult> fetchAccountVideoStatistics(
        const QString &,
        const QStringList &knownShortCodes = {}) const override
    {
        lastKnownShortCodes = knownShortCodes;
        co_await QCoro::sleepFor(std::chrono::milliseconds(5));
        co_return nextAccountVideosResult;
    }
};

static void checkPropertyRetrieveStatistics(const QDir &root)
{
    // 1. Short code extraction
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Check out these heels! #fashion #qs0001 #style"))
            == QStringLiteral("QS-0001"), "extract #qs0001");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Caption with #QS0042 uppercase"))
            == QStringLiteral("QS-0042"), "extract #QS0042");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Direct QS-0007 code"))
            == QStringLiteral("QS-0007"), "extract QS-0007");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Check out these heels! #tag1 #tag2 #tag3 #tag4 #tag5 qs0001"))
            == QStringLiteral("QS-0001"), "extract qs0001 without #");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Caption with qs0042 lowercase"))
            == QStringLiteral("QS-0042"), "extract qs0042 without #");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Caption with QS0042 uppercase"))
            == QStringLiteral("QS-0042"), "extract QS0042 without #");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("Caption with qs-0007 lowercase hyphen"))
            == QStringLiteral("QS-0007"), "extract qs-0007");
    require(AbstractSocialAccount::extractShortCode(QStringLiteral("No short code here #fashion")).isEmpty(),
            "empty when no short code");

    // 2. Built-in accounts registered via Recorder: only TikTok supports account video statistics
    const auto &allSocial = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS();
    require(allSocial.contains(QStringLiteral("tiktok")), "tiktok registered");
    require(allSocial.contains(QStringLiteral("pinterest")), "pinterest registered");
    require(allSocial.contains(QStringLiteral("youtube")), "youtube registered");
    require(allSocial.contains(QStringLiteral("instagram")), "instagram registered");
    require(allSocial.value(QStringLiteral("tiktok"))->supportsAccountVideoStatistics(),
            "tiktok supports account video statistics");
    require(!allSocial.value(QStringLiteral("pinterest"))->supportsAccountVideoStatistics(),
            "pinterest does not support account video statistics yet");
    require(!allSocial.value(QStringLiteral("youtube"))->supportsAccountVideoStatistics(),
            "youtube does not support account video statistics yet");
    require(!allSocial.value(QStringLiteral("instagram"))->supportsAccountVideoStatistics(),
            "instagram does not support account video statistics yet");

    // 3. Test PaneProperties "Retrieve statistics" flow
    FakeSocialAccount fakeSocial;
    AbstractSocialAccount::Recorder socialReg(&fakeSocial);

    QTemporaryDir statDir;
    require(statDir.isValid(), "statDir valid");
    const QDir dir(statDir.path());

    TableAccounts accounts(dir.path());
    TableVideos videos(dir.path());
    TreeProperties props(dir.filePath("props_stats.json"));
    TreeProperties arch(dir.filePath("arch_stats.json"));
    props.setVideosModel(&videos);
    arch.setVideosModel(&videos);

    const QModelIndex propIdx = props.addProperty(QStringLiteral("Camera Angle"));
    const QModelIndex valIdx = props.addValue(propIdx, QStringLiteral("Low Angle"),
                                              QStringLiteral("shot from a dramatic low angle"));
    const QUuid valUuid = valIdx.data(TreeProperties::RoleId).toUuid();

    const QUuid videoId = videos.addVideo({valUuid}, {}, QStringLiteral("original"));
    const QString shortCode = videos.recordFromId(videoId)->shortCode; // e.g. "QS-0001"
    const QString codeHashtag = QStringLiteral("#qs") + shortCode.mid(3); // e.g. "#qs0001"
    const QString codeWithoutHash = QStringLiteral("qs") + shortCode.mid(3).toLower(); // e.g. "qs0001"

    require(videos.recordFromShortCode(codeHashtag) != nullptr, "recordFromShortCode matches legacy #qs0001");
    require(videos.recordFromShortCode(codeWithoutHash) != nullptr, "recordFromShortCode matches new qs0001 without #");
    require(videos.recordFromShortCode(shortCode) != nullptr, "recordFromShortCode matches QS-0001");

    // Register a supported account and an unsupported account (Instagram) in TableAccounts
    accounts.addAccount();
    accounts.setData(accounts.index(0, TableAccounts::IND_TYPE), QStringLiteral("TestSocial"));
    accounts.setData(accounts.index(0, TableAccounts::IND_URL), QStringLiteral("https://example.com/@brand"));
    accounts.addAccount();
    accounts.setData(accounts.index(1, TableAccounts::IND_TYPE), QStringLiteral("Instagram"));
    accounts.setData(accounts.index(1, TableAccounts::IND_URL), QStringLiteral("https://instagram.com/brand"));

    // Prepare fake social response
    fakeSocial.nextAccountVideosResult.followers = 1000;
    fakeSocial.nextAccountVideosResult.views = 500;
    fakeSocial.nextAccountVideosResult.likes = 50;
    AbstractSocialAccount::AccountPostStatistics post;
    post.postUrl = QStringLiteral("https://example.com/@brand/video/12345");
    post.caption = QStringLiteral("Walking with confidence ") + codeHashtag + QStringLiteral(" #heels");
    post.stats.views = 500;
    post.stats.likes = 50;
    post.stats.comments = 10;
    post.stats.shares = 20;
    post.publishDate = QDateTime::currentDateTimeUtc().addDays(-2);
    fakeSocial.nextAccountVideosResult.posts = {post};

    PaneProperties paneProps(&props, &arch, &videos, &accounts);
    auto *btnRetrieve = paneProps.findChild<QPushButton *>(QStringLiteral("buttonRetrieveStatistics"));
    require(btnRetrieve != nullptr, "buttonRetrieveStatistics exists in PaneProperties");
    require(btnRetrieve->text() == QStringLiteral("Retrieve statistics"), "button label is Retrieve statistics");

    bool sawNotSupportedDialog = false;
    QTimer dialogWatcher;
    QObject::connect(&dialogWatcher, &QTimer::timeout, [&]() {
        for (QWidget *w : QApplication::topLevelWidgets())
        {
            if (auto *dlg = qobject_cast<QDialog *>(w))
            {
                if (dlg->isVisible() && dlg->windowTitle() == QStringLiteral("Not supported yet"))
                {
                    sawNotSupportedDialog = true;
                    dlg->accept();
                }
            }
        }
    });
    dialogWatcher.start(10);

    btnRetrieve->click();
    QElapsedTimer timer;
    timer.start();
    while ((!paneProps.isEnabled() || !sawNotSupportedDialog) && timer.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    dialogWatcher.stop();
    require(paneProps.isEnabled(), "Retrieve statistics finished");
    require(sawNotSupportedDialog, "Not supported yet QDialog shown for non-TikTok social account");
    QApplication::processEvents();

    // Verify account stats updated
    require(accounts.data(accounts.index(0, TableAccounts::IND_FOLLOWERS)).toLongLong() == 1000,
            "account followers updated to 1000");
    require(accounts.data(accounts.index(0, TableAccounts::IND_VIEW)).toLongLong() == 500,
            "account views updated to 500");
    require(accounts.data(accounts.index(0, TableAccounts::IND_LIKES)).toLongLong() == 50,
            "account likes updated to 50");

    // Verify video matched and publication recorded
    const auto *videoRec = videos.recordFromId(videoId);
    require(videoRec != nullptr && videoRec->published, "video marked published");
    require(videoRec->publications.size() == 1, "1 publication attached to video");
    require(videoRec->publications[0].postUrl == post.postUrl, "publication URL matches");
    require(!videoRec->publications[0].snapshots.isEmpty(), "snapshot recorded on publication");

    // Verify post with code without '#' matches via extractShortCode and recordFromShortCode
    AbstractSocialAccount::AccountPostStatistics postNoHash;
    postNoHash.caption = QStringLiteral("Confidence without hash ") + codeWithoutHash + QStringLiteral(" #heels");
    require(AbstractSocialAccount::extractShortCode(postNoHash.caption) == shortCode,
            "extractShortCode matches caption with code without #");
    require(videos.recordFromShortCode(AbstractSocialAccount::extractShortCode(postNoHash.caption)) == videoRec,
            "recordFromShortCode maps extracted code without # to VideoRecord");

    // Verify TreeProperties columns
    const QModelIndex rowParent = propIdx;
    const int valRow = valIdx.row();
    require(props.data(props.index(valRow, TreeProperties::IND_VIDEO_COUNT, rowParent)).toInt() == 1,
            "IND_VIDEO_COUNT is 1");
    require(props.data(props.index(valRow, TreeProperties::IND_VIEWS, rowParent)).toLongLong() == 500,
            "IND_VIEWS is 500");
    require(props.data(props.index(valRow, TreeProperties::IND_LIKES, rowParent)).toLongLong() == 50,
            "IND_LIKES is 50");
    require(props.data(props.index(valRow, TreeProperties::IND_COMMENTS, rowParent)).toLongLong() == 10,
            "IND_COMMENTS is 10");
    require(props.data(props.index(valRow, TreeProperties::IND_SHARES, rowParent)).toLongLong() == 20,
            "IND_SHARES is 20");
    require(props.data(props.index(valRow, TreeProperties::IND_NORM_VIEWS, rowParent)).toString() == QStringLiteral("0.50"),
            "IND_NORM_VIEWS is 0.50 (500 / 1000)");
    require(props.data(props.index(valRow, TreeProperties::IND_ENGAGEMENT, rowParent)).toString() == QStringLiteral("16.0 %"),
            "IND_ENGAGEMENT is 16.0 % ((50+10+20)/500)");
    require(fakeSocial.lastKnownShortCodes.contains(shortCode),
            "knownShortCodes passed to fetchAccountVideoStatistics");
    require(QFileInfo::exists(root.filePath(QStringLiteral("logs/retrieve_statistics.log"))),
            "retrieve_statistics.log written in working directory logs/");
    require(!readFile(root.filePath(QStringLiteral("logs/retrieve_statistics.log"))).isEmpty(),
            "retrieve_statistics.log is non-empty");

    // 4. Test PaneStatistics ("Retrieve" button, TableVideoStatistics sorting by views,
    //    regenerated vs source image preview, 2-image horizontal merge, and shared stats columns)
    QTemporaryDir statsPaneDir;
    require(statsPaneDir.isValid(), "statsPaneDir valid");
    const QDir spDir(statsPaneDir.path());

    // Create solid-color test images
    auto saveSolidImage = [](const QString &path, const QColor &color, int w = 64, int h = 64) {
        QImage img(w, h, QImage::Format_ARGB32);
        img.fill(color);
        require(img.save(path), "save test image");
    };
    const QString srcImg1 = spDir.filePath(QStringLiteral("src1_red.png"));
    const QString regenImg1 = spDir.filePath(QStringLiteral("regen1_green.png"));
    const QString src2Left = spDir.filePath(QStringLiteral("src2_blue.png"));
    const QString src2Right = spDir.filePath(QStringLiteral("src2_yellow.png"));
    saveSolidImage(srcImg1, QColor(220, 20, 20));
    saveSolidImage(regenImg1, QColor(20, 200, 40));
    saveSolidImage(src2Left, QColor(20, 40, 220));
    saveSolidImage(src2Right, QColor(240, 220, 20));

    TableAccounts spAccounts(spDir.path());
    TableVideos spVideos(spDir.path());
    TableProjects spProjects(spDir.path());

    // Project 1: 1 source image + regenerated image in generation temp folder
    const int projRow1 = spProjects.addProject(srcImg1, QString{}, QStringLiteral("Single Source Project"), QString{});
    const QUuid projId1 = spProjects.projectId(projRow1);
    const QUuid vidId1 = spVideos.addVideo({}, {}, QStringLiteral("original"), projId1);
    const QString code1 = spVideos.recordFromId(vidId1)->shortCode;
    const QDir tempDir1 = spProjects.generationTempDir(projRow1, code1);
    QFile::copy(regenImg1, tempDir1.filePath(QStringLiteral("generation_source.png")));

    // Project 2: 2 source images (merged horizontally in preview)
    const int projRow2 = spProjects.addProject(src2Left, src2Right, QStringLiteral("Two Sources Project"), QString{});
    const QUuid projId2 = spProjects.projectId(projRow2);
    const QUuid vidId2 = spVideos.addVideo({}, {}, QStringLiteral("original"), projId2);
    const QString code2 = spVideos.recordFromId(vidId2)->shortCode;

    // Project 3: unpublished / unlocated video (must not appear in TableVideoStatistics)
    const int projRow3 = spProjects.addMultiVideoProject(QStringLiteral("Unlocated Multi Project"));
    const QUuid projId3 = spProjects.projectId(projRow3);
    spVideos.addVideo({}, {}, QStringLiteral("original"), projId3);

    spAccounts.addAccount();
    spAccounts.setData(spAccounts.index(0, TableAccounts::IND_TYPE), QStringLiteral("TestSocial"));
    spAccounts.setData(spAccounts.index(0, TableAccounts::IND_URL), QStringLiteral("https://example.com/@brand"));

    AbstractSocialAccount::AccountPostStatistics post1;
    post1.postUrl = QStringLiteral("https://example.com/@brand/video/v1");
    post1.shortCode = code1;
    post1.stats.views = 300;
    post1.stats.likes = 30;
    post1.stats.comments = 5;
    post1.stats.shares = 15;
    post1.publishDate = QDateTime::currentDateTimeUtc().addDays(-3);

    AbstractSocialAccount::AccountPostStatistics post2;
    post2.postUrl = QStringLiteral("https://example.com/@brand/video/v2");
    post2.shortCode = code2;
    post2.stats.views = 1200;
    post2.stats.likes = 180;
    post2.stats.comments = 20;
    post2.stats.shares = 40;
    post2.publishDate = QDateTime::currentDateTimeUtc().addDays(-1);

    fakeSocial.nextAccountVideosResult.followers = 1000;
    fakeSocial.nextAccountVideosResult.views = 1500;
    fakeSocial.nextAccountVideosResult.likes = 210;
    fakeSocial.nextAccountVideosResult.posts = {post1, post2};

    PaneStatistics paneStats(&spVideos, nullptr, &spAccounts, &spProjects);
    auto *btnStatsRetrieve = paneStats.findChild<QPushButton *>(QStringLiteral("buttonRetrieve"));
    auto *tableViewStats = paneStats.findChild<QTableView *>(QStringLiteral("tableViewStatistics"));
    require(btnStatsRetrieve != nullptr, "buttonRetrieve exists in PaneStatistics");
    require(btnStatsRetrieve->text() == QStringLiteral("Retrieve"), "top-left button text is Retrieve");
    require(tableViewStats != nullptr, "tableViewStatistics exists in PaneStatistics");
    TableVideoStatistics *statsModel = paneStats.statisticsModel();
    require(statsModel != nullptr && tableViewStats->model() == statsModel,
            "tableViewStatistics uses TableVideoStatistics model");
    require(statsModel->rowCount() == 0, "no rows before statistics are retrieved");

    // Verify shared statistics headers match between TreeProperties and TableVideoStatistics
    for (int sc = 0; sc < SocialStatistics::columnCount(); ++sc)
    {
        require(statsModel->headerData(TableVideoStatistics::IND_FIRST_STAT + sc, Qt::Horizontal).toString()
                == props.headerData(TreeProperties::IND_FIRST_STAT + sc, Qt::Horizontal).toString(),
                "statistics column headers match between PaneStatistics and PaneProperties");
    }

    btnStatsRetrieve->click();
    timer.restart();
    while ((statsModel->rowCount() < 2 || !paneStats.isEnabled()) && timer.elapsed() < 5000)
    {
        QApplication::processEvents();
        QThread::msleep(5);
    }
    require(paneStats.isEnabled(), "PaneStatistics Retrieve finished");
    require(statsModel->rowCount() == 2, "only the 2 published & located videos are listed");

    // Sorted by views descending: row 0 is vidId2 (1200 views), row 1 is vidId1 (300 views)
    require(statsModel->videoIdAt(0) == vidId2, "row 0 is highest-views video (1200 views)");
    require(statsModel->videoIdAt(1) == vidId1, "row 1 is second video (300 views)");

    // Check row 0 (2 source images merged horizontally)
    require(statsModel->data(statsModel->index(0, 0), TableVideoStatistics::RolePreviewImageCount).toInt() == 2,
            "row 0 uses 2 source images");
    const QPixmap preview0 = statsModel->previewPixmapAt(0);
    require(!preview0.isNull(), "row 0 preview pixmap is valid");
    require(preview0.width() == 64 + TableVideoStatistics::PREVIEW_MERGE_GAP + 64,
            "row 0 preview merges 2 images horizontally");
    require(preview0.height() == 64, "row 0 preview height is 64");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_NAME)).toString()
            == QStringLiteral("Two Sources Project (%1)").arg(code2),
            "row 0 Name column");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_TYPE)).toString()
            == QStringLiteral("Single"),
            "row 0 Video type column");
    require(!statsModel->data(statsModel->index(0, TableVideoStatistics::IND_CREATED)).toString().isEmpty(),
            "row 0 Created column");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_VIEWS)).toLongLong() == 1200,
            "row 0 Views == 1200");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_LIKES)).toLongLong() == 180,
            "row 0 Likes == 180");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_COMMENTS)).toLongLong() == 20,
            "row 0 Comments == 20");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_SHARES)).toLongLong() == 40,
            "row 0 Shares == 40");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_NORM_VIEWS)).toString()
            == QStringLiteral("1.20"),
            "row 0 Views/Follower == 1.20");
    require(statsModel->data(statsModel->index(0, TableVideoStatistics::IND_ENGAGEMENT)).toString()
            == QStringLiteral("20.0 %"),
            "row 0 Engagement == 20.0 %");

    // Check row 1 (regenerated image preferred over source image)
    require(statsModel->data(statsModel->index(1, 0), TableVideoStatistics::RolePreviewImageCount).toInt() == 1,
            "row 1 uses 1 image");
    const QStringList paths1 = statsModel->previewImagePathsAt(1);
    require(paths1.size() == 1 && paths1.first().endsWith(QStringLiteral("generation_source.png")),
            "row 1 prefers regenerated image over source image");
    const QPixmap preview1 = statsModel->previewPixmapAt(1);
    require(!preview1.isNull() && preview1.width() == 64 && preview1.height() == 64,
            "row 1 preview is single 64x64 image");
    const QColor centerColor = preview1.toImage().pixelColor(32, 32);
    require(centerColor.green() > 150 && centerColor.red() < 60,
            "row 1 preview pixel color comes from green regenerated image, not red source image");
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
        checkPropertyRetrieveStatistics(root);
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
        checkMultiVideoPromptManagement(saved);
        checkMultiVideoPropertySelection(properties, videos, saved);
        checkExtraPromptPhrase();
        checkDialogHooksPickQuote(root);
        checkInspirationModelsAndPane(root);

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
        require(tree->topLevelItem(0)->childCount() > 0, "generation has child items including prompt");
        auto *promptChild = tree->topLevelItem(0)->child(0);
        require(promptChild->data(0, PaneGeneration::RoleIsPromptItem).toBool(), "child item is prompt item");
        require(promptChild->data(0, PaneGeneration::RolePromptText).toString() == QStringLiteral("Exact prompt."), "prompt text is clean without format suffix");
        require(promptChild->isFirstColumnSpanned(), "prompt child item spans all columns");
        tree->setCurrentItem(promptChild);
        require(QMetaObject::invokeMethod(&pane, "_copySelectedGenerationPrompt", Qt::DirectConnection), "copy prompt shortcut invoked on single video");
        require(QGuiApplication::clipboard()->text() == QStringLiteral("Exact prompt."), "copied prompt to clipboard");
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
        generator.browserFailures = 0;
        generator.rejectionFailures = 1;
        answerRepeatDialog(1);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 2, "rejection in repeat mode retries with softened variant");
        require(generator.calls[0].prompt == seed.prompt, "first repeat attempt uses exact prompt");
        require(generator.calls[1].prompt != seed.prompt
                && generator.calls[1].prompt.endsWith(QStringLiteral("Video format: vertical (9:16).")),
                "second repeat attempt softens prompt while preserving format suffix");
        require(videos.recordsForProject(projectId).size() == 8, "rejection recovery saves completed take");

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

        // Test optional extra prompt lineEdit in PaneGeneration
        auto *lineEditExtra = pane.findChild<QLineEdit *>("lineEditExtraPrompt");
        require(lineEditExtra != nullptr, "lineEditExtraPrompt found in pane");
        require(lineEditExtra->isEnabled(), "lineEditExtraPrompt enabled when generation selected");

        auto findSeedItem = [&]() -> QTreeWidgetItem * {
            for (int i = 0; i < tree->topLevelItemCount(); ++i)
            {
                if (tree->topLevelItem(i)->text(0) == seedCode) return tree->topLevelItem(i);
            }
            return nullptr;
        };

        // 1. Enter text in pane's lineEditExtraPrompt
        tree->setCurrentItem(findSeedItem());
        lineEditExtra->setText(QStringLiteral("cinematic lighting, ultra detailed"));
        generator.calls.clear();
        answerRepeatDialog(1);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "run generation with extra prompt");
        require(generator.calls[0].prompt == QStringLiteral("Exact prompt. cinematic lighting, ultra detailed. Video format: vertical (9:16)."),
                "prompt correctly includes extra phrase before format suffix");
        require(lineEditExtra->text() == QStringLiteral("cinematic lighting, ultra detailed"), "lineEdit keeps current extra phrase");

        // 2. Change extra prompt via DialogGenerateAgain
        tree->setCurrentItem(findSeedItem());
        generator.calls.clear();
        answerRepeatDialog(1, true, QStringLiteral("night scene, neon glow"));
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "run generation with dialog-updated extra prompt");
        require(generator.calls[0].prompt == QStringLiteral("Exact prompt. night scene, neon glow. Video format: vertical (9:16)."),
                "prompt correctly uses dialog extra phrase");
        require(lineEditExtra->text() == QStringLiteral("night scene, neon glow"),
                "pane lineEdit is updated with dialog value");

        // 3. Clear extra prompt and regenerate
        tree->setCurrentItem(findSeedItem());
        lineEditExtra->clear();
        generator.calls.clear();
        answerRepeatDialog(1);
        again->click();
        waitForBatch(pane);
        require(generator.calls.size() == 1, "run generation with cleared extra prompt");
        require(generator.calls[0].prompt == seed.prompt, "prompt restored to original seed prompt");

        checkMultiVideo(root);
        checkRegeneratedStepOrder(root);
        checkStepSelectionGates();
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
