#include "model/TableAccounts.h"
#include "model/TableProjects.h"
#include "model/research/InspirationAdaptationMethod.h"
#include "model/research/ProductSource.h"
#include "model/research/ResearchStore.h"
#include "model/research/ResearchProject.h"
#include "model/research/ReferenceAssessor.h"
#include "gui/DialogGenerationOptions.h"
#include "gui/DialogGenerationPlan.h"
#include "gui/GenerationPlanSection.h"
#include "gui/panes/PaneGeneration.h"
#include "gui/panes/PanePublishing.h"
#include "gui/panes/PaneResearch.h"
#include "gui/panes/PaneWorkflow.h"
#include "model/FavoriteHooks.h"
#include "model/FavoriteVideoPrompts.h"
#include "model/PreferredHashtags.h"
#include "model/PromptLessons.h"
#include "model/SavedPrompts.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/AbstractVideoGenerator.h"
#include "model/videogen/TableGenerationSettings.h"
#include "model/videos/SocialStatistics.h"
#include "model/videos/TableVideos.h"
#include "../common/workingdirectory/WorkingDirectoryManager.h"
#include "AbstractCli.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QElapsedTimer>
#include <QFile>
#include <QGroupBox>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStackedWidget>
#include <QVideoWidget>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBox>
#include <QTreeWidget>
#include <QEventLoop>
#include <QProcess>
#include <stdexcept>
#include <iostream>

namespace {
class AssessmentCliFixture : public AbstractCli
{
public:
    QString response = QStringLiteral("{\"verdict\":\"suitable\",\"reason\":\"Matches the supplied visual and audience.\"}");
    QString getName() const override { return "Assessment fixture"; }
    QString getDescription() const override { return "Offline assessment fixture"; }
    bool canGenSvg() const override { return false; }
    bool canGenImages() const override { return false; }
    bool canGenVideosFromText() const override { return false; }
    bool canGenVideoFromImages() const override { return false; }
    QString getExecutable() const override { return "/usr/bin/printf"; }
    QStringList promptArgs() const override { return {"%s", response}; }
};
void requireResearch(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void writeResearch(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    requireResearch(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write research fixture");
}
QByteArray readResearch(const QString &path)
{
    QFile file(path);
    requireResearch(file.open(QIODevice::ReadOnly), "read research fixture");
    return file.readAll();
}
bool clickReferenceCard(QWidget &pane, const QString &referenceId)
{
    auto *card = pane.findChild<QWidget *>("referenceCard_" + referenceId);
    if (!card) return false;
    const QPointF localPos(12.0, 48.0);
    const QPointF globalPos = card->mapToGlobal(localPos.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, localPos, localPos, globalPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, localPos, localPos, globalPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(card, &press);
    QCoreApplication::sendEvent(card, &release);
    QApplication::processEvents();
    return true;
}
class ProductSwapCliFixture : public AbstractCli
{
public:
    QString scriptPath;
    QString getName() const override { return QStringLiteral("ProductSwap fixture CLI"); }
    QString getDescription() const override { return QStringLiteral("Offline product-swap + motion-prompt CLI fixture"); }
    bool canGenSvg() const override { return false; }
    bool canGenImages() const override { return true; }
    bool canGenVideosFromText() const override { return false; }
    bool canGenVideoFromImages() const override { return false; }
    QString getExecutable() const override { return QStandardPaths::findExecutable(QStringLiteral("python3")); }
    QStringList promptArgs() const override { return {scriptPath}; }
};

class ResearchVideoBackendFixture : public AbstractVideoGenerator
{
public:
    mutable QStringList prompts;
    mutable QList<QStringList> imagesPerCall;
    mutable QStringList deletedConversations;
    mutable int quotaOnCallNumber = 0;
    QString getId() const override { return QStringLiteral("gemini-research-test"); }
    QString getName() const override { return QStringLiteral("Gemini Research Test"); }
    QCoro::Task<Result> generate(const QString &prompt, const QStringList &images,
                                 const QString &outputDir, const QVariantMap &) const override
    {
        prompts << prompt;
        imagesPerCall << images;
        Result res;
        if (quotaOnCallNumber > 0 && prompts.size() == quotaOnCallNumber)
        {
            res.quotaExceeded = true;
            res.retryable = false;
            res.errorMessage = QStringLiteral("Gemini daily video quota reached (test limit).");
            co_return res;
        }
        res.videoPath = QDir(outputDir).filePath(QStringLiteral("animated_product.mp4"));
        writeResearch(res.videoPath, "fake animated video output");
        co_return res;
    }
    QCoro::Task<DeleteConversationsResult> deleteConversations(
        const QStringList &conversationUrls, const QVariantMap &) const override
    {
        DeleteConversationsResult res;
        for (const QString &u : conversationUrls)
        {
            deletedConversations << u;
            res.deletedUrls << u;
        }
        co_return res;
    }
};
}

void checkResearch(const QDir &callerWorkingDir)
{
    QTemporaryDir temporary;
    const QDir root(temporary.path());
    root.mkpath("source/stores/thumbs");
    root.mkpath("source/sizing/P-SHOE-studio-shoes/aplus");
    root.mkpath("source/sizing/B000000005-size-chart-only/aplus");
    root.mkpath("workspace");
    const QDir workspace(root.filePath("workspace"));
    const QDir source(root.filePath("source"));
    const QString marketplace = "A1PA6795UKMFR9";
    QJsonObject shoe{{"asin", "B000000001"}, {"sku", "SHOE-Black-39"}, {"title", "Studio shoes"},
        {"brand", "Studio"}, {"category", "Heels"}, {"color", "Black"}, {"sizeValue", "39"},
        {"mainImageUrl", "https://example.com/shoes.jpg"}};
    auto size40 = shoe; size40.insert("asin", "B000000002"); size40.insert("sku", "SHOE-Black-40"); size40.insert("sizeValue", "40");
    auto otherBrand = shoe; otherBrand.insert("asin", "B000000003"); otherBrand.insert("brand", "Other");
    auto noAPlus = shoe; noAPlus.insert("asin", "B000000004"); noAPlus.insert("sku", "FABRIC-Gold"); noAPlus.insert("title", "No A+ fabric");
    auto sizeChartOnly = shoe; sizeChartOnly.insert("asin", "B000000005"); sizeChartOnly.insert("sku", "SHOE-SizeOnly");
    auto excludedColor = shoe; excludedColor.insert("asin", "B000000006"); excludedColor.insert("sku", "SHOE-Red-39"); excludedColor.insert("color", "Red");
    auto archive = shoe; archive.insert("deleted", true);
    const QJsonArray catalog{shoe, size40, otherBrand, noAPlus, sizeChartOnly, excludedColor, archive};
    const QString catalogFile = source.filePath("stores/" + marketplace + ".json");
    const auto catalogBytes = QJsonDocument(catalog).toJson();
    writeResearch(catalogFile, catalogBytes);
    writeResearch(source.filePath("stores/sales.json"), "not a product catalog");
    QImage tinyThumb(54, 54, QImage::Format_RGB32); tinyThumb.fill(QColor("#888888"));
    requireResearch(tinyThumb.save(source.filePath("stores/thumbs/B000000004.jpg")), "save low-res store thumbnail");
    QImage image(800, 1200, QImage::Format_RGB32); image.fill(QColor("#b68c62"));
    requireResearch(image.save(source.filePath("sizing/P-SHOE-studio-shoes/black-image-01.jpg")), "save high-res A+ product photo");
    requireResearch(image.save(source.filePath("sizing/B000000005-size-chart-only/image-01.jpg")), "save size-chart-only photo");
    writeResearch(source.filePath("sizing/P-SHOE-studio-shoes/settings.ini"),
        "[aplus]\nexcluded_colors=Red\n\n[sizing]\nskus\\B000000001=SHOE-Black-39\nskus\\B000000002=SHOE-Black-40\nskus\\B000000003=SHOE-Black-39\nskus\\B000000006=SHOE-Red-39\n");
    writeResearch(source.filePath("sizing/P-SHOE-studio-shoes/aplus/index.json"),
        R"({"elements":[{"id":"image_color_black","type":"image","versions":[{"desktop":"image_color_black/desktop.png"}]}]})");
    writeResearch(source.filePath("sizing/B000000005-size-chart-only/aplus/index.json"),
        R"({"elements":[{"id":"size_chart","type":"size_chart","versions":[{"desktop":"size_chart/size_chart.png"}]}]})");
    const auto *importer = ProductSource::all().value("amazon-template3");
    requireResearch(importer != nullptr, "product source registered");
    const auto imported = importer->read(source.path());
    requireResearch(imported.error.isEmpty() && imported.products.size() == 2 && imported.archivedRecords == 1
                        && imported.skippedWithoutAPlus == 3,
                    "import only A+ products with high-res photos, excluding non-A+, size-chart-only, excluded colors, and archived records");
    QJsonObject product;
    for (const auto &v : imported.products) if (v.toObject().value("brand") == "Studio") product = v.toObject();
    requireResearch(product.value("variants").toArray().size() == 2, "two variants retained");
    requireResearch(!product.contains("inventory") && !product.value("variants").toArray()[0].toObject().contains("inventory"), "unknown inventory stays unknown");
    requireResearch(QFileInfo::exists(product.value("localImage").toString())
                        && !product.value("localImage").toString().contains("/stores/thumbs/"),
                    "resolve high-res sizing photo rather than low-res store thumbnail");
    requireResearch(readResearch(catalogFile) == catalogBytes, "import is read-only");
    const auto repeated = importer->read(source.path());
    requireResearch(repeated.products[0].toObject().value("id") == imported.products[0].toObject().value("id"), "repeat import identities stable");
    writeResearch(catalogFile, "broken");
    requireResearch(!importer->read(source.path()).error.isEmpty(), "reject malformed catalog");
    writeResearch(catalogFile, catalogBytes);

    const QString accountsFile = workspace.filePath("social_accounts.csv");
    writeResearch(accountsFile, "Type,Url,View,Followers\nTikTok,https://www.tiktok.com/@studio,5000000000,100\nPinterest,https://www.pinterest.com/studio,12,3");
    TableAccounts accounts(workspace.path());
    requireResearch(accounts.rowCount() == 2 && accounts.columnCount() == 5, "legacy account migration preserves UI columns");
    const auto firstId = accounts.accountId(0);
    const auto secondId = accounts.accountId(1);
    requireResearch(!firstId.isNull() && firstId != secondId, "stable distinct account IDs assigned");
    requireResearch(accounts.data(accounts.index(0, TableAccounts::IND_VIEW)).toLongLong() == 5000000000LL, "large metric not truncated during migration");
    requireResearch(accounts.setData(accounts.index(0, TableAccounts::IND_URL), "https://www.tiktok.com/@newstudio"), "edit account URL");
    TableAccounts reloadedAccounts(workspace.path());
    requireResearch(reloadedAccounts.accountId(0) == firstId, "identity survives URL change and reload");
    accounts.addAccount();
    accounts.removeAccount(2);
    requireResearch(accounts.accountId(1) == secondId, "row changes preserve account identity");
    const QString accountId = firstId.toString(QUuid::WithoutBraces);
    const QString otherAccountId = secondId.toString(QUuid::WithoutBraces);
    const QJsonObject profile{{"brands", QJsonArray{"Studio"}}, {"categories", QJsonArray{"Heels"}}, {"audience", "Fashion and styling"}};
    requireResearch(!ResearchStore::productRestriction(product, {}).isEmpty(), "unconfigured accounts do not allow all brands");
    requireResearch(ResearchStore::productRestriction(product, profile).isEmpty(), "explicit brand and category allowed");
    auto blocked = product; blocked.insert("brand", "Other");
    requireResearch(!ResearchStore::productRestriction(blocked, profile).isEmpty(), "wrong brand blocked");

    ResearchStore store(workspace.path());
    requireResearch(store.setProfile(accountId, profile), "persist account rules");
    requireResearch(store.replaceProducts(imported.products, source.path()), "persist imported catalog");
    requireResearch(store.replaceProducts(imported.products, source.path()) && store.products().size() == 2, "refresh does not duplicate products");
    const QString post = "https://www.tiktok.com/@creator/video/123456";
    const QString media = product.value("localImage").toString();
    const QString referenceId = store.addReference(accountId, post + "?utm_source=test", "Outfit reveal", media);
    requireResearch(!referenceId.isEmpty(), "add reference with media");
    requireResearch(store.addReference(accountId, post, "Duplicate") == referenceId && store.references().size() == 1, "deduplicate tracked URLs per account");
    const QString otherReference = store.addReference(otherAccountId, post, "Pinterest research");
    requireResearch(otherReference != referenceId && store.references().size() == 2, "same reference independently reviewed for another account");
    requireResearch(store.addReference(accountId, "file:///etc/passwd", "Invalid").isEmpty(), "reject non-web URL");
    requireResearch(store.updateReference(referenceId, {{"productId", product.value("id")}, {"notes", "Keep the opening shoe close-up and outfit reveal."}}), "link product and adaptation notes");
    requireResearch(store.review({referenceId}, "approved", "A feasible outfit format"), "approve reference");
    const QString copiedMedia = store.mediaPath(store.reference(referenceId));
    requireResearch(QFileInfo::exists(copiedMedia) && copiedMedia.startsWith(workspace.path()), "media copied inside workspace");
    ResearchStore reload(workspace.path());
    requireResearch(reload.reference(referenceId).value("status") == "approved" && reload.profile(accountId) == store.profile(accountId), "review and rules survive reload");
    requireResearch(store.updateReference(referenceId, {{"notes", "Change pacing to three fast cuts."}}), "edit reference");
    requireResearch(store.reference(referenceId).value("status") == "approved", "editing notes or product keeps approved status");
    requireResearch(!reload.review({referenceId}, "rejected", "stale"), "stale writer cannot overwrite newer update");
    requireResearch(store.attachMedia(referenceId, media) && store.reference(referenceId).value("status") == "pending", "attaching media requires renewed review");
    requireResearch(store.reference(referenceId).value("url") == post, "attaching media preserves source URL");
    requireResearch(store.review({referenceId}, "approved", "Reviewed attached media"), "approve attached media");
    requireResearch(store.reference(referenceId).value("history").toArray().size() == 3, "review history retained");

    QJsonObject rulesAssessment;
    ReferenceAssessor::all().value("account-rules")->assess({{"product", blocked}, {"profile", profile}}, nullptr, {}, {}, &store,
        [&](const QJsonObject &value) { rulesAssessment = value; });
    requireResearch(rulesAssessment.value("verdict") == "ineligible", "assessor enforces account restrictions");
    requireResearch(store.recordAssessment(referenceId, rulesAssessment) && store.reference(referenceId).value("status") == "approved", "assessment does not rewrite human decision");
    AssessmentCliFixture cli;
    for (const bool valid : {true, false})
    {
        if (!valid) cli.response = "I think it will go viral";
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(3000);
        QJsonObject assessment;
        ReferenceAssessor::all().value("cli-suitability")->assess({{"product", product}, {"profile", profile}}, &cli,
            "Inspect production feasibility", workspace.path(), &loop, [&](const QJsonObject &value) { assessment = value; loop.quit(); });
        loop.exec();
        requireResearch(assessment.value("verdict").toString() == (valid ? "suitable" : "error"), "validate real async CLI success and malformed output");
        requireResearch(assessment.value("prompt") == "Inspect production feasibility" && assessment.value("input").isObject(), "assessment preserves criteria and input evidence");
    }

    TableProjects projects(workspace.path());
    QJsonObject context{{"accountId", accountId}, {"profile", profile}, {"product", product},
        {"reference", store.reference(referenceId)}, {"referenceMediaPath", copiedMedia}};
    QString error;
    const int projectRow = ResearchProject::create(projects, context, media, &error);
    requireResearch(projectRow >= 0 && error.isEmpty(), "create project from approved match");
    const auto projectDir = projects.projectDir(projectRow);
    const auto saved = QJsonDocument::fromJson(readResearch(projectDir.filePath("research_context.json"))).object();
    requireResearch(saved.value("reference").toObject().value("id") == referenceId && saved.value("product").toObject().value("id") == product.value("id"), "freeze reference/product provenance");
    requireResearch(QFileInfo::exists(projectDir.filePath(saved.value("referenceLocalMedia").toString())), "project owns reference media copy");
    // Same approved reference can be reused across multiple allowed products without resetting status.
    QJsonObject secondAllowedProduct = product;
    secondAllowedProduct.insert("id", "prod-burgundy-heels");
    secondAllowedProduct.insert("color", "Burgundy");
    secondAllowedProduct.insert("title", "Studio Ankle Strap Heels");
    QJsonObject secondContext = context;
    secondContext.insert("product", secondAllowedProduct);
    const int secondProjectRow = ResearchProject::create(projects, secondContext, media, &error);
    requireResearch(secondProjectRow >= 0 && error.isEmpty() && projects.rowCount() == 2,
                    "same approved reference can create projects for multiple allowed products");
    requireResearch(projects.data(projects.index(secondProjectRow, TableProjects::IND_NAME)).toString().startsWith("Burgundy"),
                    "multi-product project name includes product color to distinguish projects");
    root.mkpath("generation");
    requireResearch(ResearchProject::snapshot(projectDir, QDir(root.filePath("generation")), &error), "snapshot research into generation");
    requireResearch(readResearch(root.filePath("generation/research_context.json")) == readResearch(projectDir.filePath("research_context.json")), "generation has frozen context");
    context.insert("profile", QJsonObject{});
    requireResearch(ResearchProject::create(projects, context, media, &error) == -1 && projects.rowCount() == 2, "invalid match creates no project");

    // Widget interaction exercises the actual card actions and account isolation.
    PaneResearch pane(&accounts, &store, {});
    pane.resize(1260, 850);
    pane.show();
    auto *dirEdit = pane.findChild<QLineEdit *>("amazonSourceDirectory");
    auto *productTable = pane.findChild<QTableView *>("researchProducts");
    auto *accountCombo = pane.findChild<QComboBox *>("researchAccount");
    auto *importBtn = pane.findChild<QPushButton *>("importAmazonProducts");
    requireResearch(dirEdit && dirEdit->text() == source.path(), "restore last imported directory when ui.ini is absent");
    requireResearch(productTable && productTable->model()->rowCount() == 1, "configured account filters to allowed products");
    requireResearch(accountCombo != nullptr, "account combo exists");
    accountCombo->setCurrentIndex(1);
    QApplication::processEvents();
    requireResearch(productTable->model()->rowCount() == 2, "unconfigured account shows all imported products instead of blank table");
    accountCombo->setCurrentIndex(0);
    QApplication::processEvents();
    requireResearch(productTable->model()->rowCount() == 1, "switching back to configured account restores allowed filter");

    root.mkpath("invalid_source");
    const QString invalidPath = root.filePath("invalid_source");
    QString warningText;
    dirEdit->setText(invalidPath);
    QTimer::singleShot(0, [&]() {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            warningText = box->text();
            box->accept();
        }
    });
    requireResearch(importBtn != nullptr, "import button exists");
    importBtn->click();
    requireResearch(warningText.contains("stores") && warningText.contains("invalid"), "invalid directory warns about missing stores folder");
    dirEdit->setText(source.path());

    auto *tabs = pane.findChild<QTabWidget *>();
    requireResearch(tabs && tabs->count() == 2, "Products and References subtabs");
    tabs->setCurrentIndex(1);
    // Approved card is hidden by the default pending filter.
    requireResearch(store.review({referenceId}, "pending", "UI review"), "reset card for UI check");
    const QString nextReference = store.addReference(accountId, "https://www.tiktok.com/@creator/video/789", "Next pending reference", media);
    requireResearch(!nextReference.isEmpty(), "next review card");
    QApplication::processEvents();
    auto *approve = pane.findChild<QPushButton *>("approveReference_" + referenceId);
    requireResearch(approve != nullptr, "visual card has approve button");
    approve->click();
    requireResearch(store.reference(referenceId).value("status") == "approved", "card approval persists");
    requireResearch(store.reference(otherReference).value("status") == "pending", "card approval isolated to selected account");
    auto *titleField = pane.findChild<QLineEdit *>("researchReferenceTitle");
    requireResearch(titleField && titleField->text() == "Next pending reference", "approval advances preview to next pending card");
    auto *createButton = pane.findChild<QPushButton *>("researchCreateProject");
    auto *projectProductCombo = pane.findChild<QComboBox *>("researchProjectProduct");
    auto *filterPendingBtn = pane.findChild<QPushButton *>("researchFilter_pending");
    auto *filterApprovedBtn = pane.findChild<QPushButton *>("researchFilter_approved");
    requireResearch(createButton && projectProductCombo && filterPendingBtn && filterApprovedBtn,
                    "simplified UI provides filter pills, right-side product picker, and Create generation project button");
    requireResearch(createButton->mapTo(&pane, QPoint(0, createButton->height())).y() <= pane.height(),
                    "create-project action visible without scrolling");
    requireResearch(filterApprovedBtn->text().contains("(1)"), "Approved filter pill displays live count");

    // Verify 1-click Approved view + clicking a card anywhere selects it + Create generation project keeps approved status.
    int emittedProjectCount = 0;
    QJsonObject lastEmittedContext;
    QObject::connect(&pane, &PaneResearch::createProjectRequested, &pane,
                     [&](const QJsonObject &ctx, const QString &) { ++emittedProjectCount; lastEmittedContext = ctx; });
    filterApprovedBtn->click();
    QApplication::processEvents();
    requireResearch(clickReferenceCard(pane, referenceId) && titleField->text() == "Outfit reveal",
                    "clicking anywhere on an approved card selects and previews it");
    createButton->click();
    QApplication::processEvents();
    requireResearch(emittedProjectCount == 1
                        && lastEmittedContext.value("reference").toObject().value("id") == referenceId
                        && store.reference(referenceId).value("status") == "approved"
                        && store.reference(referenceId).value("usedProducts").toArray().size() == 1,
                    "creating a generation project preserves approved status and records used product");

    // Switching back to Pending and clicking Create generation project on a pending card auto-approves it in 1 step.
    filterPendingBtn->click();
    QApplication::processEvents();
    requireResearch(clickReferenceCard(pane, nextReference) && titleField->text() == "Next pending reference",
                    "clicking anywhere on a pending card selects it");
    createButton->click();
    QApplication::processEvents();
    requireResearch(emittedProjectCount == 2 && store.reference(nextReference).value("status") == "approved",
                    "clicking Create generation project on a selected pending card auto-approves it");

    auto *spyButton = pane.findChild<QPushButton *>("researchScrollAndSave");
    requireResearch(spyButton != nullptr, "Scroll & save button exists in References tab");

    for (const QString &platformId : {QStringLiteral("pinterest"), QStringLiteral("tiktok"), QStringLiteral("youtube"), QStringLiteral("instagram")})
    {
        const auto *platform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS().value(platformId);
        requireResearch(platform && platform->supportsSpyScrollAndSave(), "social platform supports Scroll & save");
        requireResearch(!platform->defaultSpySearchUrl("burgundy heels outfit").isEmpty(), "social platform builds search URL");
    }
    requireResearch(PaneResearch::defaultSpyQueryForProduct(product).contains("Black", Qt::CaseInsensitive)
                        && PaneResearch::defaultSpyQueryForProduct(product).contains("outfit", Qt::CaseInsensitive),
                    "default spy query includes product color and outfit keyword");
    const QJsonObject verboseProduct{
        {"title", "Women's Leopard Print Slingback Pumps with 7 Cm Stiletto Heel - Elasticated Back Strap for an Adjustable Fit (Cheetah, 37=US-6)"},
        {"brand", "Pradize"},
        {"color", "Cheetah"},
        {"categories", QJsonArray{"SHOES", "HEELS"}}
    };
    const QString compactQuery = PaneResearch::defaultSpyQueryForProduct(verboseProduct);
    const QStringList queryWords = compactQuery.split(' ', Qt::SkipEmptyParts);
    requireResearch(queryWords.size() >= 3 && queryWords.size() <= 5
                        && compactQuery.contains("Leopard", Qt::CaseInsensitive)
                        && compactQuery.contains("Slingback", Qt::CaseInsensitive)
                        && compactQuery.contains("Pumps", Qt::CaseInsensitive)
                        && compactQuery.contains("outfit", Qt::CaseInsensitive)
                        && !compactQuery.contains("Cheetah", Qt::CaseInsensitive)
                        && !compactQuery.contains("Cm", Qt::CaseInsensitive)
                        && !compactQuery.contains("Elasticated", Qt::CaseInsensitive),
                    "default spy query condenses verbose Amazon title into 3-5 clean social keywords");

    // Exercise the full C++ -> Python Playwright spyScrollAndSave worker against an offline HTML fixture.
    root.mkpath("spy_fixture");
    const QString spyImgPath = root.filePath("spy_fixture/pin_outfit.jpg");
    requireResearch(image.save(spyImgPath), "save spy fixture image");
    const QString spyHtmlPath = root.filePath("spy_fixture/pins.html");
    writeResearch(spyHtmlPath, QStringLiteral(
        "<!doctype html><html><body style='min-height:2400px'>"
        "<div data-test-id='pin'><a href='https://www.pinterest.com/pin/9988776655/' aria-label='Black heels evening outfit'>"
        "<img src='file://%1' alt='Black heels evening outfit'/></a></div>"
        "<div data-test-id='pin' style='margin-top:900px'><a href='https://www.pinterest.com/pin/1122334455/' aria-label='Studio stiletto street style'>"
        "<img src='file://%1' alt='Studio stiletto street style'/></a></div>"
        "</body></html>").arg(spyImgPath).toUtf8());
    qputenv("QS_SPY_HEADLESS", "1");
    {
        QEventLoop spyLoop;
        QTimer spyTimeout;
        spyTimeout.setSingleShot(true);
        QObject::connect(&spyTimeout, &QTimer::timeout, &spyLoop, &QEventLoop::quit);
        spyTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + spyHtmlPath, 5, true, product.value("id").toString()),
            &spyLoop,
            [&spyLoop]() { spyLoop.quit(); });
        spyLoop.exec();
    }
    bool foundSpyPin = false;
    for (const auto &v : store.references())
    {
        const auto ref = v.toObject();
        if (ref.value("url").toString().contains("9988776655"))
        {
            foundSpyPin = true;
            requireResearch(ref.value("productId") == product.value("id")
                                && QFileInfo::exists(store.mediaPath(ref)),
                            "Scroll & save imports pin URL, links selected product, and copies downloaded image into store");
        }
    }
    requireResearch(foundSpyPin, "Scroll & save harvested candidate pins from browser");

    // Verify human-in-the-loop rule: pause scrolling & timeouts during CAPTCHA and
    // login modals (never scroll while login inputs or CAPTCHA are visible), then resume.
    const QString gatedHtmlPath = root.filePath("spy_fixture/gated_pins.html");
    writeResearch(gatedHtmlPath, QStringLiteral(
        "<!doctype html><html><body style='min-height:3000px;margin:0'>"
        "<div id='captcha-verify-container-main-page' style='width:340px;height:220px;background:#222;color:#fff'>"
        "Verify to continue — drag the puzzle piece</div>"
        "<div data-test-id='pin'><a href='https://www.pinterest.com/pin/2222000001/' aria-label='Pre-login heel look'>"
        "<img src='file://%1' alt='Pre-login heel look'/></a></div>"
        "<div id='post-login-results' style='margin-top:900px'></div>"
        "<script>"
        "let scrolledDuringCaptcha = false;"
        "let loginShown = false;"
        "let loginOpen = false;"
        "let scrolledDuringLogin = false;"
        "window.addEventListener('scroll', () => {"
        "  if (document.getElementById('captcha-verify-container-main-page')) scrolledDuringCaptcha = true;"
        "  if (loginOpen) { scrolledDuringLogin = true; return; }"
        "  if (!loginShown) {"
        "    loginShown = true;"
        "    loginOpen = true;"
        "    const dlg = document.createElement('div');"
        "    dlg.id = 'test-login-modal';"
        "    dlg.setAttribute('role', 'dialog');"
        "    dlg.style.cssText = 'position:fixed;top:80px;left:120px;width:380px;height:280px;background:#fff;z-index:9999';"
        "    dlg.innerHTML = '<h2>Welcome to Pinterest</h2>'"
        "      + '<input id=\"email\" name=\"id\" type=\"email\" style=\"width:240px;height:32px;display:block\"/>'"
        "      + '<input id=\"password\" name=\"password\" type=\"password\" style=\"width:240px;height:32px;display:block\"/>';"
        "    document.body.appendChild(dlg);"
        "    document.getElementById('email').focus();"
        "    setTimeout(() => {"
        "      loginOpen = false;"
        "      dlg.remove();"
        "      if (!scrolledDuringCaptcha && !scrolledDuringLogin) {"
        "        document.getElementById('post-login-results').innerHTML = "
        "          \"<div data-test-id='pin'><a href='https://www.pinterest.com/pin/2222000002/' aria-label='Post-login heel outfit'>\""
        "          + \"<img src='file://%1' alt='Post-login heel outfit'/></a></div>\";"
        "      }"
        "    }, 750);"
        "  }"
        "});"
        "setTimeout(() => {"
        "  if (window.scrollY > 0) scrolledDuringCaptcha = true;"
        "  const cap = document.getElementById('captcha-verify-container-main-page');"
        "  if (cap) cap.remove();"
        "}, 650);"
        "</script></body></html>").arg(spyImgPath).toUtf8());
    {
        QEventLoop gatedLoop;
        QTimer gatedTimeout;
        gatedTimeout.setSingleShot(true);
        QObject::connect(&gatedTimeout, &QTimer::timeout, &gatedLoop, &QEventLoop::quit);
        gatedTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + gatedHtmlPath, 5, true, product.value("id").toString()),
            &gatedLoop,
            [&gatedLoop]() { gatedLoop.quit(); });
        gatedLoop.exec();
    }
    bool foundPostLoginPin = false;
    for (const auto &v : store.references())
    {
        if (v.toObject().value("url").toString().contains("2222000002"))
        {
            foundPostLoginPin = true;
            break;
        }
    }
    requireResearch(foundPostLoginPin,
                    "Scroll & save pauses during CAPTCHA and login modal without scrolling, then resumes after human completion");

    // Verify login redirect recovery: target search -> login.html -> home.html -> returns to target search.
    const QString homeHtmlPath = root.filePath("spy_fixture/home.html");
    const QString loginHtmlPath = root.filePath("spy_fixture/login.html");
    const QString redirectSearchPath = root.filePath("spy_fixture/redirect_search.html");
    writeResearch(homeHtmlPath, "<!doctype html><html><body><h1>Generic Home Feed</h1></body></html>");
    writeResearch(loginHtmlPath, QStringLiteral(
        "<!doctype html><html><body>"
        "<h1>Log in</h1>"
        "<input type='email' name='email' style='width:240px;height:32px;display:block'/>"
        "<input type='password' name='password' style='width:240px;height:32px;display:block'/>"
        "<script>"
        "setTimeout(() => { window.name = 'qs_logged_in'; window.location.href = 'file://%1'; }, 550);"
        "</script></body></html>").arg(homeHtmlPath).toUtf8());
    writeResearch(redirectSearchPath, QStringLiteral(
        "<!doctype html><html><body style='min-height:1400px'>"
        "<div id='results'></div>"
        "<script>"
        "if (window.name !== 'qs_logged_in') {"
        "  window.location.href = 'file://%1';"
        "} else {"
        "  document.getElementById('results').innerHTML = "
        "    \"<div data-test-id='pin'><a href='https://www.pinterest.com/pin/3333000003/' aria-label='Authenticated search result'>\""
        "    + \"<img src='file://%2' alt='Authenticated search result'/></a></div>\";"
        "}"
        "</script></body></html>").arg(loginHtmlPath, spyImgPath).toUtf8());
    {
        QEventLoop redirectLoop;
        QTimer redirectTimeout;
        redirectTimeout.setSingleShot(true);
        QObject::connect(&redirectTimeout, &QTimer::timeout, &redirectLoop, &QEventLoop::quit);
        redirectTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + redirectSearchPath, 5, true, product.value("id").toString()),
            &redirectLoop,
            [&redirectLoop]() { redirectLoop.quit(); });
        redirectLoop.exec();
    }
    bool foundRedirectSearchPin = false;
    for (const auto &v : store.references())
    {
        if (v.toObject().value("url").toString().contains("3333000003"))
        {
            foundRedirectSearchPin = true;
            break;
        }
    }
    requireResearch(foundRedirectSearchPin,
                    "Scroll & save waits on login page and returns to original search URL after post-login redirect");

    // Verify statistics capture, quality threshold filtering, ad creative preservation + bold "ads" badge, UI card stats display, and reject rate.
    const QString tenDaysAgoIso = QDate::currentDate().addDays(-10).toString(Qt::ISODate);
    const QString statsHtmlPath = root.filePath("spy_fixture/stats_pins.html");
    writeResearch(statsHtmlPath, QStringLiteral(
        "<!doctype html><html><body style='min-height:1800px'>"
        "<div data-test-id='pin' data-saves='1' data-likes='0' data-author='@spam1'>"
        "  <a href='https://www.pinterest.com/pin/4444000001/' aria-label='Affiliate spam pin page'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='6' data-likes='1' data-author='@weak2'>"
        "  <a href='https://www.pinterest.com/pin/4444000002/' aria-label='Weak pin page'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='55' data-likes='2' data-author='@nouvele_fusion'>"
        "  <a href='https://www.pinterest.com/pin/4444000003/' aria-label='Chic leopard slingbacks pin page'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='2' data-likes='1' data-author='@weak4'>"
        "  <a href='https://www.pinterest.com/pin/4444000004/' aria-label='Another weak pin page'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='1449' data-likes='96' data-author='@kabasu30' data-created='%2T08:32:17Z'>"
        "  <a href='https://www.pinterest.com/pin/4444000005/' aria-label='Untitled pin page'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-promoted='true' data-saves='3' data-likes='0' data-author='@paidbrand'>"
        "  <a href='https://www.pinterest.com/pin/4444000006/' aria-label='Promoted heel creative pin page'>"
        "  <img src='file://%1'/></a></div>"
        "</body></html>").arg(spyImgPath, tenDaysAgoIso).toUtf8());
    {
        QEventLoop statsLoop;
        QTimer statsTimeout;
        statsTimeout.setSingleShot(true);
        QObject::connect(&statsTimeout, &QTimer::timeout, &statsLoop, &QEventLoop::quit);
        statsTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + statsHtmlPath, 10, true, product.value("id").toString(), 10),
            &statsLoop,
            [&statsLoop]() { statsLoop.quit(); });
        statsLoop.exec();
    }
    const QJsonObject spySession = store.lastSpySession(accountId);
    requireResearch(spySession.value("browsedCount").toInt() == 6
                        && spySession.value("savedCount").toInt() == 3
                        && spySession.value("rejectedCount").toInt() == 3
                        && spySession.value("minEngagement").toInt() == 10,
                    "spy session records browsed, saved (including ad creative), and rejected counts (50% reject rate)");
    QString topPinRefId;
    QString adPinRefId;
    bool foundRejectedSpamPin = false;
    for (const auto &v : store.references())
    {
        const auto ref = v.toObject();
        const QString u = ref.value("url").toString();
        if (u.contains("4444000001") || u.contains("4444000002") || u.contains("4444000004"))
        {
            foundRejectedSpamPin = true;
        }
        if (u.contains("4444000005"))
        {
            topPinRefId = ref.value("id").toString();
            const QJsonObject st = ref.value("stats").toObject();
            requireResearch(st.value("saves").toInt() == 1449
                                && st.value("likes").toInt() == 96
                                && st.value("author").toString() == "@kabasu30"
                                && st.value("publishedAt").toString() == tenDaysAgoIso
                                && !ref.value("title").toString().contains("Untitled", Qt::CaseInsensitive)
                                && !ref.value("title").toString().contains("pin page", Qt::CaseInsensitive),
                            "saved pin stores structured stats including creation date and cleans Untitled/pin page noise");
        }
        if (u.contains("4444000006"))
        {
            adPinRefId = ref.value("id").toString();
            requireResearch(ref.value("stats").toObject().value("isAd").toBool(),
                            "promoted pin is saved with isAd=true");
        }
    }
    requireResearch(!foundRejectedSpamPin && !topPinRefId.isEmpty() && !adPinRefId.isEmpty(),
                    "quality filter excludes low-stat organic pins while keeping high-stat pins and ad creatives");
    QApplication::processEvents();
    auto *metricsSummary = pane.findChild<QLabel *>("researchMetricsSummary");
    auto *topCardStats = pane.findChild<QLabel *>("referenceStats_" + topPinRefId);
    auto *adCardStats = pane.findChild<QLabel *>("referenceStats_" + adPinRefId);
    requireResearch(metricsSummary
                        && metricsSummary->text().contains("Browsed 6")
                        && metricsSummary->text().contains("Saved 3 (50.0%)")
                        && metricsSummary->text().contains("Rejected 3 (50.0% reject rate"),
                    "UI metrics summary displays browse, save %, and reject rate %");
    requireResearch(topCardStats
                        && topCardStats->text().contains("1,449 saves")
                        && topCardStats->text().contains("145 saves/day")
                        && topCardStats->text().contains("96 likes")
                        && topCardStats->text().contains(tenDaysAgoIso + " (10d old)")
                        && topCardStats->text().contains("@kabasu30"),
                    "UI reference card displays pin saves, saves/day, likes, creation date with days old, and author");
    requireResearch(adCardStats
                        && adCardStats->textFormat() == Qt::RichText
                        && adCardStats->text().contains("<b")
                        && adCardStats->text().contains(">ads</b>")
                        && adCardStats->text().contains("3 saves"),
                    "UI reference card displays bold 'ads' badge alongside statistics for ad creatives");

    // Verify WebP reference images render properly even when Qt's optional libqwebp plugin is absent.
    const QString webpPath = root.filePath("spy_fixture/sample_pin.webp");
    {
        QProcess mkWebp;
        mkWebp.start(QStringLiteral("ffmpeg"), {
            QStringLiteral("-y"),
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-i"), spyImgPath,
            webpPath
        });
        requireResearch(mkWebp.waitForFinished(10000) && mkWebp.exitCode() == 0 && QFileInfo::exists(webpPath),
                        "create WebP fixture image");
    }
    const QString webpRefId = store.addReference(accountId, "https://www.pinterest.com/pin/7777000007/", "WebP heel pin", webpPath);
    requireResearch(!webpRefId.isEmpty(), "add WebP reference to store");
    QApplication::processEvents();
    auto *webpCard = pane.findChild<QWidget *>("referenceCard_" + webpRefId);
    requireResearch(webpCard != nullptr, "WebP reference card rendered");
    bool hasRenderedWebpPixmap = false;
    for (auto *lbl : webpCard->findChildren<QLabel *>())
    {
        if (!lbl->pixmap().isNull())
        {
            hasRenderedWebpPixmap = true;
            break;
        }
    }
    requireResearch(hasRenderedWebpPixmap, "WebP reference card displays rendered thumbnail image instead of fallback text");
    const QString webpHtmlPath = root.filePath("spy_fixture/webp_pin.html");
    writeResearch(webpHtmlPath, QStringLiteral(
        "<!doctype html><html><body>"
        "<div data-test-id='pin' data-saves='25' data-likes='4'>"
        "  <a href='https://www.pinterest.com/pin/7777000008/' aria-label='Converted WebP pin'>"
        "  <img src='file://%1'/></a></div>"
        "</body></html>").arg(webpPath).toUtf8());
    {
        QEventLoop webpLoop;
        QTimer webpTimeout;
        webpTimeout.setSingleShot(true);
        QObject::connect(&webpTimeout, &QTimer::timeout, &webpLoop, &QEventLoop::quit);
        webpTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + webpHtmlPath, 5, true, product.value("id").toString(), 10),
            &webpLoop,
            [&webpLoop]() { webpLoop.quit(); });
        webpLoop.exec();
    }
    bool foundConvertedWebpAsJpg = false;
    for (const auto &v : store.references())
    {
        const auto ref = v.toObject();
        if (ref.value("url").toString().contains("7777000008"))
        {
            const QString storedMedia = store.mediaPath(ref);
            foundConvertedWebpAsJpg = storedMedia.endsWith(".jpg", Qt::CaseInsensitive)
                && QImageReader(storedMedia).canRead();
            break;
        }
    }
    requireResearch(foundConvertedWebpAsJpg, "Scroll & save converts downloaded WebP images to Qt-native JPEG");

    store.review({referenceId}, "pending", "Screenshot fixture");
    const QString screenshot = qEnvironmentVariable("QS_RESEARCH_SCREENSHOT");
    if (!screenshot.isEmpty())
    {
        const QString second = store.addReference(accountId, "https://www.pinterest.com/pin/56789", "Three ways to style", media);
        requireResearch(!second.isEmpty(), "second visual card fixture");
        store.updateReference(second, {{"productId", product.value("id")}});
        QApplication::processEvents();
        requireResearch(clickReferenceCard(pane, referenceId), "click reference card for screenshot");
        requireResearch(pane.grab().save(screenshot), "save research UI screenshot");
    }

    // Verify "Delete selection" and "Delete all" buttons clean up references, cached media, and preserve other accounts.
    const QString adMediaPath = store.mediaPath(store.reference(adPinRefId));
    requireResearch(!adMediaPath.isEmpty() && QFileInfo::exists(adMediaPath), "ad pin has cached media before deletion");
    requireResearch(clickReferenceCard(pane, adPinRefId), "click ad card to select before deletion");
    auto *deleteSelBtn = pane.findChild<QPushButton *>("researchDeleteSelection");
    auto *deleteAllBtn = pane.findChild<QPushButton *>("researchDeleteAll");
    requireResearch(deleteSelBtn && deleteAllBtn, "Delete selection and Delete all buttons exist");
    QTimer::singleShot(0, [&]() {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            if (auto *def = box->defaultButton()) def->click();
            else box->accept();
        }
    });
    deleteSelBtn->click();
    QApplication::processEvents();
    requireResearch(store.reference(adPinRefId).isEmpty()
                        && !QFileInfo::exists(adMediaPath)
                        && !store.reference(topPinRefId).isEmpty(),
                    "Delete selection removes selected reference and its cached media without touching others");

    const QString topMediaPath = store.mediaPath(store.reference(topPinRefId));
    requireResearch(!topMediaPath.isEmpty() && QFileInfo::exists(topMediaPath), "top pin has cached media before Delete all");
    QTimer::singleShot(0, [&]() {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            if (auto *def = box->defaultButton()) def->click();
            else box->accept();
        }
    });
    deleteAllBtn->click();
    QApplication::processEvents();
    int remainingForFirstAccount = 0;
    for (const auto &v : store.references())
    {
        if (v.toObject().value("accountId").toString() == accountId)
        {
            ++remainingForFirstAccount;
        }
    }
    requireResearch(remainingForFirstAccount == 0
                        && !QFileInfo::exists(topMediaPath)
                        && store.lastSpySession(accountId).isEmpty()
                        && !store.reference(otherReference).isEmpty(),
                    "Delete all removes all references and cached media for current account while preserving other accounts");

    // Verify rejected pins are never re-added or resurrected on subsequent Scroll & save runs
    // (even across country subdomains like fr.pinterest.com and even after Delete all).
    const QString rejectedPinId = store.addReference(
        accountId, "https://www.pinterest.com/pin/9090000001/", "Bad collage pin", spyImgPath);
    requireResearch(!rejectedPinId.isEmpty() && store.review({rejectedPinId}, "rejected", "Off-brand collage"),
                    "mark reference as rejected");
    const QString repeatSpyHtml = root.filePath("spy_fixture/repeat_with_rejected.html");
    writeResearch(repeatSpyHtml, QStringLiteral(
        "<!doctype html><html><body>"
        "<div data-test-id='pin' data-saves='500' data-likes='40'>"
        "  <a href='https://fr.pinterest.com/pin/9090000001/' aria-label='Same rejected pin on FR domain'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='80' data-likes='12'>"
        "  <a href='https://www.pinterest.com/pin/9090000002/' aria-label='Fresh unseen heel pin'>"
        "  <img src='file://%1'/></a></div>"
        "</body></html>").arg(spyImgPath).toUtf8());
    {
        QEventLoop repeatLoop;
        QTimer repeatTimeout;
        repeatTimeout.setSingleShot(true);
        QObject::connect(&repeatTimeout, &QTimer::timeout, &repeatLoop, &QEventLoop::quit);
        repeatTimeout.start(30000);
        // Request maxPosts=1: scroller must skip 9090000001 and fill its 1 slot with 9090000002
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + repeatSpyHtml, 1, true, product.value("id").toString(), 10),
            &repeatLoop,
            [&repeatLoop]() { repeatLoop.quit(); });
        repeatLoop.exec();
    }
    requireResearch(store.reference(rejectedPinId).value("status").toString() == "rejected",
                    "rejected pin stays rejected and is not reset to pending");
    bool foundFreshUnseenPin = false;
    for (const auto &v : store.references())
    {
        if (v.toObject().value("url").toString().contains("9090000002"))
        {
            foundFreshUnseenPin = true;
            break;
        }
    }
    requireResearch(foundFreshUnseenPin,
                    "scroller skips previously rejected pin (even on fr.pinterest.com) and collects fresh unseen pin");

    // Delete all cards, then verify the tombstone still blocks 9090000001 on future scrapes
    QStringList allAccRefs;
    for (const auto &v : store.references())
    {
        const auto r = v.toObject();
        if (r.value("accountId").toString() == accountId)
        {
            allAccRefs << r.value("id").toString();
        }
    }
    requireResearch(store.removeReferences(allAccRefs), "delete all cards including rejected card");
    {
        QEventLoop afterDelLoop;
        QTimer afterDelTimeout;
        afterDelTimeout.setSingleShot(true);
        QObject::connect(&afterDelTimeout, &QTimer::timeout, &afterDelLoop, &QEventLoop::quit);
        afterDelTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + repeatSpyHtml, 5, true, product.value("id").toString(), 10),
            &afterDelLoop,
            [&afterDelLoop]() { afterDelLoop.quit(); });
        afterDelLoop.exec();
    }
    bool readdedRejectedPin = false;
    bool readdedFreshPin = false;
    for (const auto &v : store.references())
    {
        const auto r = v.toObject();
        if (r.value("accountId").toString() != accountId) continue;
        if (r.value("url").toString().contains("9090000001")) readdedRejectedPin = true;
        if (r.value("url").toString().contains("9090000002")) readdedFreshPin = true;
    }
    requireResearch(!readdedRejectedPin && readdedFreshPin,
                    "human-rejected URL remains permanently excluded even after deleting cards");

    // Verify maxDaysOld filter rejects stale pins even when they have high total saves, while keeping fresh pins.
    const QString fresh15dIso = QDate::currentDate().addDays(-15).toString(Qt::ISODate);
    const QString stale400dIso = QDate::currentDate().addDays(-400).toString(Qt::ISODate);
    const QString maxAgeHtmlPath = root.filePath("spy_fixture/max_age_pins.html");
    writeResearch(maxAgeHtmlPath, QStringLiteral(
        "<!doctype html><html><body>"
        "<div data-test-id='pin' data-saves='2500' data-likes='120' data-created='%2'>"
        "  <a href='https://www.pinterest.com/pin/9595000001/' aria-label='Old viral pin'>"
        "  <img src='file://%1'/></a></div>"
        "<div data-test-id='pin' data-saves='120' data-likes='14' data-created='%3'>"
        "  <a href='https://www.pinterest.com/pin/9595000002/' aria-label='Fresh trending pin'>"
        "  <img src='file://%1'/></a></div>"
        "</body></html>").arg(spyImgPath, stale400dIso, fresh15dIso).toUtf8());
    {
        QEventLoop ageLoop;
        QTimer ageTimeout;
        ageTimeout.setSingleShot(true);
        QObject::connect(&ageTimeout, &QTimer::timeout, &ageLoop, &QEventLoop::quit);
        ageTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + maxAgeHtmlPath, 5, true, product.value("id").toString(), 10, 90),
            &ageLoop,
            [&ageLoop]() { ageLoop.quit(); });
        ageLoop.exec();
    }
    bool foundStale400dPin = false;
    bool foundFresh15dPin = false;
    for (const auto &v : store.references())
    {
        const auto r = v.toObject();
        if (r.value("accountId").toString() != accountId) continue;
        if (r.value("url").toString().contains("9595000001")) foundStale400dPin = true;
        if (r.value("url").toString().contains("9595000002")) foundFresh15dPin = true;
    }
    QApplication::processEvents();
    const QJsonObject ageSession = store.lastSpySession(accountId);
    requireResearch(!foundStale400dPin && foundFresh15dPin
                        && ageSession.value("browsedCount").toInt() == 2
                        && ageSession.value("savedCount").toInt() == 1
                        && ageSession.value("rejectedCount").toInt() == 1
                        && ageSession.value("maxDaysOld").toInt() == 90
                        && metricsSummary && metricsSummary->text().contains("max age ≤ 90d"),
                    "maxDaysOld filter skips pins older than limit, saves fresh pins, and reports max age in summary");

    // Verify Pinterest video pin capture (.mp4 preferred over static poster) + end-to-end
    // Step 1 (Replace Pinterest product with our product -> review starting image) ->
    // Step 2 (Animate product-swapped image into video with motion prompt from Pinterest video).
    const QString pinVideoPath = root.filePath("spy_fixture/pinterest_pin_video.mp4");
    {
        QProcess mkVideo;
        mkVideo.start(QStringLiteral("ffmpeg"), {
            QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("color=c=#3b2a26:s=180x320:d=1.2:r=10"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            pinVideoPath
        });
        requireResearch(mkVideo.waitForFinished(15000) && mkVideo.exitCode() == 0 && QFileInfo::exists(pinVideoPath),
                        "create synthetic Pinterest .mp4 video fixture");
    }
    const QString videoPinHtmlPath = root.filePath("spy_fixture/video_pin.html");
    writeResearch(videoPinHtmlPath, QStringLiteral(
        "<!doctype html><html><body>"
        "<div data-test-id='pin' data-saves='920' data-likes='65' data-video-url='file://%2'>"
        "  <a href='https://www.pinterest.com/pin/9898000001/' aria-label='Walking down marble stairs in heels'>"
        "  <img src='file://%1'/></a></div>"
        "</body></html>").arg(spyImgPath, pinVideoPath).toUtf8());
    {
        QEventLoop vidLoop;
        QTimer vidTimeout;
        vidTimeout.setSingleShot(true);
        QObject::connect(&vidTimeout, &QTimer::timeout, &vidLoop, &QEventLoop::quit);
        vidTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("pinterest", "file://" + videoPinHtmlPath, 5, true, {}, 10),
            &vidLoop,
            [&vidLoop]() { vidLoop.quit(); });
        vidLoop.exec();
    }
    QString videoRefId;
    for (const auto &v : store.references())
    {
        const auto r = v.toObject();
        if (r.value("url").toString().contains("9898000001"))
        {
            videoRefId = r.value("id").toString();
            break;
        }
    }
    requireResearch(!videoRefId.isEmpty()
                        && store.mediaPath(store.reference(videoRefId)).endsWith(".mp4", Qt::CaseInsensitive),
                    "Pinterest Scroll & save downloads actual .mp4 video stream when pin is a video");

    // Select the Pinterest video card and click Create generation project
    QJsonObject emittedVideoProjectCtx;
    QString emittedProductImg;
    QObject::connect(&pane, &PaneResearch::createProjectRequested, &pane,
                     [&](const QJsonObject &ctx, const QString &img) {
                         emittedVideoProjectCtx = ctx;
                         emittedProductImg = img;
                     });
    QApplication::processEvents();
    requireResearch(clickReferenceCard(pane, videoRefId), "select Pinterest video reference card");
    createButton->click();
    QApplication::processEvents();
    requireResearch(!emittedVideoProjectCtx.isEmpty()
                        && emittedVideoProjectCtx.value("referenceMediaPath").toString().endsWith(".mp4", Qt::CaseInsensitive),
                    "Create generation project passes Pinterest .mp4 video into Generation");

    // Verify TikTok Search Scroll & save: outer card traversal (DivItemContainerV2 wrapping search_top-item +
    // sibling search-card-desc), JSON hydration stats (playCount/diggCount/createTime), inner #grid-main
    // container scrolling when body is non-scrollable, min_score=1000 + maxDaysOld=365 filtering, and .mp4 capture.
    const qint64 recentUnixTs = QDateTime::currentDateTimeUtc().addDays(-12).toSecsSinceEpoch();
    const QString tiktokSearchHtmlPath = root.filePath("spy_fixture/tiktok_search.html");
    writeResearch(tiktokSearchHtmlPath, QStringLiteral(
        "<!doctype html><html><body style='margin:0;height:100vh;overflow:hidden'>"
        "<script id='__UNIVERSAL_DATA_FOR_REHYDRATION__' type='application/json'>"
        "{\"__DEFAULT_SCOPE__\":{\"webapp.search\":{\"data\":["
        "  {\"id\":\"7610000000000000001\",\"desc\":\"Rainbow stiletto runway walk #heels\",\"createTime\":%3,"
        "   \"author\":{\"uniqueId\":\"kerina.wang\"},\"stats\":{\"playCount\":64400,\"diggCount\":2065,\"commentCount\":12,\"shareCount\":240,\"collectCount\":716}},"
        "  {\"id\":\"7610000000000000002\",\"desc\":\"Low view heel clip\",\"createTime\":%3,"
        "   \"author\":{\"uniqueId\":\"tinyacct\"},\"stats\":{\"playCount\":120,\"diggCount\":5,\"commentCount\":0,\"shareCount\":0,\"collectCount\":0}},"
        "  {\"id\":\"7610000000000000003\",\"desc\":\"Scrolled marble stairs heel transition\",\"createTime\":%3,"
        "   \"author\":{\"uniqueId\":\"heelsobsession\"},\"stats\":{\"playCount\":88200,\"diggCount\":4100,\"commentCount\":34,\"shareCount\":510,\"collectCount\":900}}"
        "]}}}"
        "</script>"
        "<main id='grid-main' style='height:520px;overflow-y:auto'>"
        "  <div class='DivThreeColumnContainer' style='min-height:1500px'>"
        "    <div class='DivItemContainerV2'>"
        "      <div data-e2e='search_top-item'><div class='DivWrapper'>"
        "        <a href='https://www.tiktok.com/@kerina.wang/video/7610000000000000001'>"
        "          <img src='file://%1'/></a>"
        "      </div></div>"
        "      <div data-e2e='search-card-desc'>"
        "        <span data-e2e='search-card-video-caption'>Rainbow stiletto runway walk #heels</span>"
        "      </div>"
        "    </div>"
        "    <div class='DivItemContainerV2'>"
        "      <div data-e2e='search_top-item'><div class='DivWrapper'>"
        "        <a href='https://www.tiktok.com/@tinyacct/video/7610000000000000002'>"
        "          <img src='file://%1'/></a>"
        "      </div></div>"
        "      <div data-e2e='search-card-desc'>"
        "        <span data-e2e='search-card-video-caption'>Low view heel clip</span>"
        "      </div>"
        "    </div>"
        "    <div id='tiktok-scrolled-slot' style='margin-top:750px'></div>"
        "  </div>"
        "</main>"
        "<script>"
        "const gm = document.getElementById('grid-main');"
        "let injected = false;"
        "gm.addEventListener('scroll', () => {"
        "  if (!injected && gm.scrollTop > 100) {"
        "    injected = true;"
        "    document.getElementById('tiktok-scrolled-slot').innerHTML ="
        "      \"<div class='DivItemContainerV2' data-video-url='file://%2'>\""
        "      + \"<div data-e2e='search_top-item'><div class='DivWrapper'>\""
        "      + \"<a href='https://www.tiktok.com/@heelsobsession/video/7610000000000000003'>\""
        "      + \"<img src='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'/></a>\""
        "      + \"</div></div><div data-e2e='search-card-desc'>\""
        "      + \"<span data-e2e='search-card-video-caption'>Scrolled marble stairs heel transition</span></div></div>\";"
        "  }"
        "});"
        "</script></body></html>").arg(spyImgPath, pinVideoPath).arg(recentUnixTs).toUtf8());
    {
        QEventLoop ttLoop;
        QTimer ttTimeout;
        ttTimeout.setSingleShot(true);
        QObject::connect(&ttTimeout, &QTimer::timeout, &ttLoop, &QEventLoop::quit);
        ttTimeout.start(30000);
        QCoro::connect(
            pane.runScrollAndSave("tiktok", "file://" + tiktokSearchHtmlPath, 5, true, {}, 1000, 365),
            &ttLoop,
            [&ttLoop]() { ttLoop.quit(); });
        ttLoop.exec();
    }
    const QJsonObject ttSpySession = store.lastSpySession(accountId);
    requireResearch(ttSpySession.value("platformId").toString() == "tiktok"
                        && ttSpySession.value("browsedCount").toInt() == 3
                        && ttSpySession.value("savedCount").toInt() == 2
                        && ttSpySession.value("rejectedCount").toInt() == 1,
                    "TikTok Search Scroll & save scrolls inner #grid-main, enriches stats, saves >=1000 views, and rejects low-view posts");
    bool foundTikTokScrolledMp4 = false;
    bool foundTikTokHydratedStats = false;
    for (const auto &v : store.references())
    {
        const auto r = v.toObject();
        const QString u = r.value("url").toString();
        if (u.contains("7610000000000000001"))
        {
            const QJsonObject st = r.value("stats").toObject();
            foundTikTokHydratedStats = (st.value("views").toInt() == 64400
                                        && st.value("likes").toInt() == 2065
                                        && st.value("shares").toInt() == 956);
        }
        if (u.contains("7610000000000000003"))
        {
            foundTikTokScrolledMp4 = store.mediaPath(r).endsWith(".mp4", Qt::CaseInsensitive);
        }
    }
    requireResearch(foundTikTokHydratedStats && foundTikTokScrolledMp4,
                    "TikTok Search Scroll & save captures exact playCount/diggCount/shares+collects and .mp4 video stream");

    // Verify clicking "✕ Reject" on a card never scrolls down to the bottom (Qt focus-cascade bug)
    // and "Next after review" selects the card that slides into the same visual grid slot.
    {
        QStringList bulkIds;
        for (int i = 0; i < 16; ++i)
        {
            const QString bid = store.addReference(
                accountId,
                QStringLiteral("https://www.tiktok.com/@scrolltest/video/7620000000000000%1")
                    .arg(i, 3, 10, QLatin1Char('0')),
                QStringLiteral("Scroll stability card %1").arg(i),
                spyImgPath);
            requireResearch(!bid.isEmpty(), "add scroll stability card");
            store.updateReference(bid, QJsonObject{
                {QStringLiteral("stats"), QJsonObject{{QStringLiteral("views"), 50000 - i * 100}}}
            });
            bulkIds << bid;
        }
        if (auto *tabs = pane.findChild<QTabWidget *>())
        {
            tabs->setCurrentIndex(1); // References tab
        }
        if (auto *pendingBtn = pane.findChild<QPushButton *>("researchFilter_pending"))
        {
            pendingBtn->click();
        }
        pane.resize(1180, 560);
        pane.show();
        auto *scrollArea = pane.findChild<QScrollArea *>("referenceScrollArea");
        requireResearch(scrollArea && scrollArea->verticalScrollBar(), "reference scroll area exists");
        auto *vbar = scrollArea->verticalScrollBar();
        {
            QElapsedTimer settleTimer;
            settleTimer.start();
            while (vbar->maximum() <= 200 && settleTimer.elapsed() < 500)
            {
                if (scrollArea->widget())
                {
                    scrollArea->widget()->adjustSize();
                }
                QApplication::processEvents();
                QThread::msleep(15);
            }
        }
        requireResearch(vbar->maximum() > 200,
                        QStringLiteral("16+ reference cards make scroll area scrollable (max=%1, vpH=%2, cardsH=%3)")
                            .arg(vbar->maximum())
                            .arg(scrollArea->viewport()->height())
                            .arg(scrollArea->widget() ? scrollArea->widget()->height() : -1)
                            .toUtf8().constData());

        vbar->setValue(140);
        QApplication::processEvents();
        const int beforeY = vbar->value();
        requireResearch(beforeY == 140, "scroll position set to 140px before clicking Reject");

        const QString targetCardId = bulkIds[2];
        const QString expectedNextSlotCardId = bulkIds[3];
        auto *rejectBtn = pane.findChild<QPushButton *>("rejectReference_" + targetCardId);
        requireResearch(rejectBtn != nullptr, "find per-card Reject button");
        rejectBtn->setFocus(Qt::MouseFocusReason);
        const int afterFocusY = vbar->value();
        rejectBtn->click();
        const int afterClickY = vbar->value();
        {
            QEventLoop settleAfterReject;
            QTimer::singleShot(45, &settleAfterReject, &QEventLoop::quit);
            settleAfterReject.exec();
        }

        requireResearch(vbar->value() == beforeY,
                        QStringLiteral("clicking Reject preserves exact scroll position (got %1, expected %2, afterFocus=%3, afterClick=%4, max=%5)")
                            .arg(vbar->value())
                            .arg(beforeY)
                            .arg(afterFocusY)
                            .arg(afterClickY)
                            .arg(vbar->maximum())
                            .toUtf8().constData());
        auto *nextSelectedCard = pane.findChild<QFrame *>("referenceCard_" + expectedNextSlotCardId);
        requireResearch(nextSelectedCard != nullptr
                            && nextSelectedCard->styleSheet().contains("#58a6ff"),
                        "Next after review selects the card sliding into the vacated visual grid slot");

        store.removeReferences(bulkIds);
        QApplication::processEvents();
    }

    // Verify Favorite Influencers CRUD, handle/URL normalization, stalest-first ordering,
    // Reference detail "☆ Star @author" action, Scroll & save dialog 3rd mode + influencer UI,
    // and end-to-end Browser HumanPick mode (hover live stats, Alt+Click, unpick, cross-page persistence,
    // in-browser ☆ Star creator, direct-to-Approved import, and lastCheckedAt / lastPickedAt tracking).
    {
        const QString infNeverId = store.addInfluencer(accountId, "tiktok", "@fresh_muse", "Editorial heels");
        const QString infOldId = store.addInfluencer(accountId, "tiktok", "https://www.tiktok.com/@heelsobsession/video/7610000000000000003");
        const QString infTodayId = store.addInfluencer(accountId, "tiktok", "kerina.wang");
        requireResearch(!infNeverId.isEmpty() && !infOldId.isEmpty() && !infTodayId.isEmpty(),
                        "add favorite TikTok influencers from @handle, bare handle, and video URL");
        requireResearch(store.influencer(infOldId).value("handle").toString() == "@heelsobsession"
                            && store.influencer(infOldId).value("url").toString() == "https://www.tiktok.com/@heelsobsession",
                        "TikTok video URL normalizes to creator handle and profile URL");
        requireResearch(store.addInfluencer(accountId, "tiktok", "@FRESH_MUSE", "Updated note") == infNeverId
                            && store.influencer(infNeverId).value("notes").toString() == "Updated note",
                        "duplicate influencer handle deduplicates case-insensitively and updates notes");

        const QString twentyDaysAgoTs = QDateTime::currentDateTimeUtc().addDays(-20).toString(Qt::ISODateWithMs);
        const QString todayTs = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        requireResearch(store.recordInfluencerActivity(infOldId, true, true, 4, twentyDaysAgoTs),
                        "record 20d-old checked+picked activity");
        requireResearch(store.recordInfluencerActivity(infTodayId, true, false, 0, todayTs),
                        "record today checked-only activity");

        const QJsonArray orderedTtInf = store.influencers(accountId, "tiktok");
        requireResearch(orderedTtInf.size() == 3
                            && orderedTtInf[0].toObject().value("id").toString() == infNeverId
                            && orderedTtInf[1].toObject().value("id").toString() == infOldId
                            && orderedTtInf[2].toObject().value("id").toString() == infTodayId,
                        "influencers sort stalest-first: Never checked -> 20d ago -> checked today");
        requireResearch(PaneResearch::formatInfluencerOptionLabel(orderedTtInf[0].toObject()).contains("Never checked")
                            && PaneResearch::formatInfluencerOptionLabel(orderedTtInf[1].toObject()).contains("20d ago")
                            && PaneResearch::formatInfluencerOptionLabel(orderedTtInf[1].toObject()).contains("4 saved")
                            && PaneResearch::formatInfluencerOptionLabel(orderedTtInf[2].toObject()).contains("today")
                            && PaneResearch::formatInfluencerOptionLabel(orderedTtInf[2].toObject()).contains("No picks yet"),
                        "formatInfluencerOptionLabel renders relative checked/picked dates and saved counts");

        // Verify Reference detail "☆ Star @author" button stars an author in 1 click
        QString kabasuRefId;
        for (const auto &v : store.references())
        {
            const auto r = v.toObject();
            if (r.value("accountId").toString() == accountId
                && r.value("url").toString().contains("kerina.wang"))
            {
                kabasuRefId = r.value("id").toString();
                break;
            }
        }
        const QString unstarredAuthorRefId = store.addReference(
            accountId, "https://www.pinterest.com/pin/8080000099/", "Runway heel closeup", spyImgPath);
        requireResearch(!unstarredAuthorRefId.isEmpty(), "add reference with author for star button test");
        store.updateReference(unstarredAuthorRefId, QJsonObject{
            {QStringLiteral("stats"), QJsonObject{{QStringLiteral("author"), QStringLiteral("@velvet_steps")}}}
        });
        if (auto *pendingBtn = pane.findChild<QPushButton *>("researchFilter_pending"))
        {
            pendingBtn->click();
        }
        QApplication::processEvents();
        requireResearch(clickReferenceCard(pane, unstarredAuthorRefId), "select reference card with @velvet_steps author");
        auto *starAuthorBtn = pane.findChild<QPushButton *>("researchStarAuthor");
        requireResearch(starAuthorBtn && starAuthorBtn->isVisible()
                            && starAuthorBtn->text().contains("☆ Star @velvet_steps"),
                        "Reference detail shows ☆ Star @velvet_steps button");
        starAuthorBtn->click();
        QApplication::processEvents();
        const QJsonArray pinInfAfterStar = store.influencers(accountId, "pinterest");
        requireResearch(pinInfAfterStar.size() == 1
                            && pinInfAfterStar[0].toObject().value("handle").toString() == "@velvet_steps"
                            && starAuthorBtn->text().contains("★ @velvet_steps"),
                        "clicking ☆ Star @author saves Pinterest influencer and updates button to ★");
        const QString velvetStepsInfId = pinInfAfterStar[0].toObject().value("id").toString();

        // Exercise the Scroll & save dialog's 3rd mode ("pick") + Favorite influencer UI controls
        bool verifiedSpyDialogPickAndInfluencers = false;
        QTimer spyDlgTimer;
        spyDlgTimer.setInterval(25);
        QObject::connect(&spyDlgTimer, &QTimer::timeout, [&]() {
            auto *activeModal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!activeModal || activeModal->objectName() != QLatin1String("dialogScrollAndSave"))
            {
                return;
            }
            spyDlgTimer.stop();
            auto *platCombo = activeModal->findChild<QComboBox *>("spyPlatformCombo");
            auto *modeCombo = activeModal->findChild<QComboBox *>("spyModeCombo");
            auto *filtersBox = activeModal->findChild<QWidget *>("spyAutoFiltersBox");
            auto *kwRadio = activeModal->findChild<QRadioButton *>("spyTargetKeywordRadio");
            auto *infRadio = activeModal->findChild<QRadioButton *>("spyTargetInfluencerRadio");
            auto *infCombo = activeModal->findChild<QComboBox *>("spyInfluencerCombo");
            auto *addInfBtn = activeModal->findChild<QPushButton *>("spyAddInfluencerBtn");
            auto *rmInfBtn = activeModal->findChild<QPushButton *>("spyRemoveInfluencerBtn");
            auto *queryEd = activeModal->findChild<QLineEdit *>("spyQueryEdit");
            auto *urlPrev = activeModal->findChild<QLabel *>("spyTargetUrlPreview");
            if (platCombo && modeCombo && filtersBox && kwRadio && infRadio && infCombo && addInfBtn && rmInfBtn && queryEd && urlPrev)
            {
                const int pickIdx = modeCombo->findData(QStringLiteral("pick"));
                modeCombo->setCurrentIndex(pickIdx);
                queryEd->clear();
                QApplication::processEvents();
                const bool pickHidesFiltersAndAllowsBlankQuery =
                    pickIdx == 2
                    && !filtersBox->isVisible()
                    && urlPrev->text() == QLatin1String("https://www.pinterest.com/");

                infRadio->setChecked(true);
                QApplication::processEvents();
                // Separate singleShot timer because Qt 6 suppresses re-entrant firing of the outer QTimer inside nested QDialog::exec()
                QTimer::singleShot(15, []() {
                    if (auto *addModal = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                        addModal && addModal->objectName() == QLatin1String("dialogAddInfluencer"))
                    {
                        if (auto *inp = addModal->findChild<QLineEdit *>("addInfluencerInput"))
                        {
                            inp->setText(QStringLiteral("@temp_dialog_star"));
                        }
                        addModal->accept();
                    }
                });
                addInfBtn->click();
                QApplication::processEvents();
                const bool addedViaDialog =
                    infCombo->count() == 2
                    && infCombo->currentText().contains("@temp_dialog_star")
                    && urlPrev->text() == QLatin1String("https://www.pinterest.com/temp_dialog_star");
                rmInfBtn->click();
                QApplication::processEvents();
                const bool removedViaDialog =
                    infCombo->count() == 1
                    && infCombo->currentText().contains("@velvet_steps");

                verifiedSpyDialogPickAndInfluencers =
                    pickHidesFiltersAndAllowsBlankQuery && addedViaDialog && removedViaDialog;
            }
            activeModal->reject();
        });
        spyDlgTimer.start();
        spyButton->click();
        spyDlgTimer.stop();
        requireResearch(verifiedSpyDialogPickAndInfluencers,
                        "Scroll & save dialog provides 3rd 'pick' mode, hides auto filters, previews default home URL, and adds/removes favorite influencers");

        // End-to-end Browser HumanPick session:
        // Page 1 hovers Pin A (verifies '+ Pick · 1.4K saves' in Shadow DOM), clicks to pick Pin A,
        // Alt+Clicks Pin B then unpicks Pin B from the tray, picks low-stat Pin C (3 saves), and navigates to Page 2.
        // Page 2 verifies Picked: 2 survived navigation, stars '@velvet_new_star' via HUD, picks video Pin D (.mp4),
        // and clicks '✓ Save & Finish (3)'.
        const QString pickPage2Path = root.filePath("spy_fixture/pick_page2.html");
        const QString pickPage1Path = root.filePath("spy_fixture/pick_page1.html");
        writeResearch(pickPage2Path, QStringLiteral(
            "<!doctype html><html><body data-profile-handle='@velvet_new_star' style='margin:0;padding:20px'>"
            "<div data-test-id='pin' data-saves='890' data-likes='45' data-author='@velvet_steps' data-video-url='file://%2' style='width:220px;height:280px'>"
            "  <a id='pinD' href='https://www.pinterest.com/pin/8181000004/' aria-label='Velvet steps runway video'>"
            "    <img src='file://%1' style='width:220px;height:260px;display:block'/></a>"
            "</div>"
            "<script>"
            "function stepPage2() {"
            "  const host = document.getElementById('__qs_picker_host');"
            "  if (!host || !host.shadowRoot) { setTimeout(stepPage2, 40); return; }"
            "  const sr = host.shadowRoot;"
            "  const toggleBtn = sr.getElementById('qs-tray-toggle');"
            "  const starBtn = sr.getElementById('qs-star-creator-btn');"
            "  const finishBtn = sr.getElementById('qs-finish-btn');"
            "  if (!toggleBtn || !toggleBtn.textContent.includes('Picked: 2') || !starBtn || starBtn.style.display === 'none') {"
            "    setTimeout(stepPage2, 40); return;"
            "  }"
            "  starBtn.click();"
            "  const pinD = document.getElementById('pinD');"
            "  const rD = pinD.getBoundingClientRect();"
            "  pinD.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, altKey: true, clientX: rD.left + 40, clientY: rD.top + 40 }));"
            "  function waitPicked3() {"
            "    if (!toggleBtn.textContent.includes('Picked: 3') || !starBtn.textContent.includes('★ @velvet_new_star')) {"
            "      setTimeout(waitPicked3, 40); return;"
            "    }"
            "    setTimeout(() => finishBtn.click(), 80);"
            "  }"
            "  setTimeout(waitPicked3, 60);"
            "}"
            "setTimeout(stepPage2, 80);"
            "</script></body></html>").arg(spyImgPath, pinVideoPath).toUtf8());

        writeResearch(pickPage1Path, QStringLiteral(
            "<!doctype html><html><body style='margin:0;padding:20px;display:flex;gap:24px'>"
            "<div data-test-id='pin' data-saves='1420' data-likes='88' data-author='@velvet_steps' style='width:220px;height:280px'>"
            "  <a id='pinA' href='https://www.pinterest.com/pin/8181000001/' aria-label='High-stat picked pin'>"
            "    <img src='file://%1' style='width:220px;height:260px;display:block'/></a>"
            "</div>"
            "<div data-test-id='pin' data-saves='2' data-likes='0' data-author='@other' style='width:220px;height:280px'>"
            "  <a id='pinB' href='https://www.pinterest.com/pin/8181000002/' aria-label='Accidentally clicked then unpicked pin'>"
            "    <img src='file://%1' style='width:220px;height:260px;display:block'/></a>"
            "</div>"
            "<div data-test-id='pin' data-saves='3' data-likes='1' data-author='@indie_stylist' style='width:220px;height:280px'>"
            "  <a id='pinC' href='https://www.pinterest.com/pin/8181000003/' aria-label='Low-stat human-chosen gem'>"
            "    <img src='file://%1' style='width:220px;height:260px;display:block'/></a>"
            "</div>"
            "<script>"
            "function stepPage1() {"
            "  const host = document.getElementById('__qs_picker_host');"
            "  if (!host || !host.shadowRoot) { setTimeout(stepPage1, 40); return; }"
            "  const sr = host.shadowRoot;"
            "  const hoverPill = sr.getElementById('qs-hover-pill');"
            "  const toggleBtn = sr.getElementById('qs-tray-toggle');"
            "  const pinA = document.getElementById('pinA');"
            "  const rA = pinA.getBoundingClientRect();"
            "  window.dispatchEvent(new MouseEvent('mousemove', { bubbles: true, clientX: rA.left + 50, clientY: rA.top + 50 }));"
            "  if (!hoverPill || hoverPill.style.display === 'none' || !hoverPill.textContent.includes('1.4K saves')) {"
            "    setTimeout(stepPage1, 40); return;"
            "  }"
            "  hoverPill.click();"
            "  const pinB = document.getElementById('pinB');"
            "  const rB = pinB.getBoundingClientRect();"
            "  pinB.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, altKey: true, clientX: rB.left + 50, clientY: rB.top + 50 }));"
            "  const pinC = document.getElementById('pinC');"
            "  const rC = pinC.getBoundingClientRect();"
            "  pinC.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, altKey: true, clientX: rC.left + 50, clientY: rC.top + 50 }));"
            "  function waitAllThreeThenUnpickB() {"
            "    if (!toggleBtn.textContent.includes('Picked: 3')) { setTimeout(waitAllThreeThenUnpickB, 40); return; }"
            "    toggleBtn.click();"
            "    const trayRows = Array.from(sr.querySelectorAll('.qs-tray-row'));"
            "    const rowB = trayRows.find(r => r.textContent.includes('unpicked pin') || r.textContent.includes('8181000002'));"
            "    if (!rowB) { setTimeout(waitAllThreeThenUnpickB, 40); return; }"
            "    rowB.querySelector('.qs-tray-rm').click();"
            "    function waitAfterUnpick() {"
            "      if (!toggleBtn.textContent.includes('Picked: 2')) { setTimeout(waitAfterUnpick, 40); return; }"
            "      window.location.href = 'file://%2';"
            "    }"
            "    setTimeout(waitAfterUnpick, 40);"
            "  }"
            "  setTimeout(waitAllThreeThenUnpickB, 60);"
            "}"
            "setTimeout(stepPage1, 80);"
            "</script></body></html>").arg(spyImgPath, pickPage2Path).toUtf8());

        {
            QEventLoop pickLoop;
            QTimer pickTimeout;
            pickTimeout.setSingleShot(true);
            QObject::connect(&pickTimeout, &QTimer::timeout, &pickLoop, &QEventLoop::quit);
            pickTimeout.start(30000);
            QCoro::connect(
                pane.runScrollAndSaveMode(
                    "pinterest",
                    "file://" + pickPage1Path,
                    24,
                    AbstractSocialAccount::SpyMode::HumanPick,
                    product.value("id").toString(),
                    500, // high minEngagement must NOT reject human-picked Pin C (3 saves)
                    30,
                    velvetStepsInfId),
                &pickLoop,
                [&pickLoop]() { pickLoop.quit(); });
            pickLoop.exec();
        }
        QApplication::processEvents();

        bool foundPickedA = false;
        bool foundUnpickedB = false;
        bool foundLowStatPickedC = false;
        bool foundVideoPickedD = false;
        for (const auto &v : store.references())
        {
            const auto r = v.toObject();
            if (r.value("accountId").toString() != accountId) continue;
            const QString u = r.value("url").toString();
            if (u.contains("8181000001"))
            {
                foundPickedA = (r.value("status").toString() == "approved"
                                && r.value("stats").toObject().value("saves").toInt() == 1420
                                && QFileInfo::exists(store.mediaPath(r)));
            }
            if (u.contains("8181000002"))
            {
                foundUnpickedB = true;
            }
            if (u.contains("8181000003"))
            {
                foundLowStatPickedC = (r.value("status").toString() == "approved"
                                       && r.value("stats").toObject().value("saves").toInt() == 3);
            }
            if (u.contains("8181000004"))
            {
                foundVideoPickedD = (r.value("status").toString() == "approved"
                                     && store.mediaPath(r).endsWith(".mp4", Qt::CaseInsensitive));
            }
        }
        requireResearch(foundPickedA && !foundUnpickedB && foundLowStatPickedC && foundVideoPickedD,
                        "HumanPick imports only picked posts across navigations (respecting unpick & low-stat human choices), downloads .mp4, and lands directly in Approved");
        requireResearch(filterApprovedBtn->isChecked(),
                        "PaneResearch automatically switches to Approved view after HumanPick imports approved posts");

        const QString todayPrefix = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).left(10);
        const QJsonObject updatedVelvet = store.influencer(velvetStepsInfId);
        requireResearch(updatedVelvet.value("lastCheckedAt").toString().startsWith(todayPrefix)
                            && updatedVelvet.value("lastPickedAt").toString().startsWith(todayPrefix)
                            && updatedVelvet.value("pickedCount").toInt() >= 2,
                        "HumanPick updates target favorite influencer lastCheckedAt, lastPickedAt, and pickedCount");

        bool foundBrowserStarredCreator = false;
        for (const auto &v : store.influencers(accountId, "pinterest"))
        {
            if (v.toObject().value("handle").toString() == "@velvet_new_star")
            {
                foundBrowserStarredCreator = v.toObject().value("lastCheckedAt").toString().startsWith(todayPrefix);
                break;
            }
        }
        requireResearch(foundBrowserStarredCreator,
                        "clicking ☆ Star @creator inside Chrome saves favorite influencer and records visited profile date");
    }

    const QJsonObject autoBatchSpec = pane.buildAutoBatchSpec(
        {product.value("id").toString()},
        3,
        true,  // includeImage
        true,  // includeCarousel
        true,  // includeVideo
        false, // carouselHookOverlay off by default
        QStringLiteral("vertical (9:16)"),
        QStringLiteral("gemini-research-test"));
    requireResearch(autoBatchSpec.value("products").toArray().size() == 1
                        && !autoBatchSpec.value("approvedReferences").toArray().isEmpty(),
                    "buildAutoBatchSpec packages selected products and ranked approved references");

    // Verify Auto-Batch dialog puts pre-checked products at the very top (hiding unchecked ones
    // by default) and displays both row thumbnails and the large product photo preview.
    {
        QJsonArray multiCatalog = store.products();
        QJsonObject goldShoe = product;
        goldShoe.insert("id", "prod-gold-ore-pump");
        goldShoe.insert("color", "Gold Ore");
        goldShoe.insert("title", "Stiletto Pump Pointed Toe Ankle Strap");
        multiCatalog.append(goldShoe);
        requireResearch(store.replaceProducts(multiCatalog, source.path()), "append second eligible product");
        QApplication::processEvents();

        auto *prodProxy = qobject_cast<QSortFilterProxyModel *>(productTable->model());
        auto *prodSrcModel = prodProxy ? qobject_cast<QStandardItemModel *>(prodProxy->sourceModel()) : nullptr;
        requireResearch(prodSrcModel && prodSrcModel->rowCount() == 2, "two eligible products in table");
        for (int r = 0; r < prodSrcModel->rowCount(); ++r)
        {
            auto *it = prodSrcModel->item(r, 0);
            it->setCheckState(it->data(Qt::UserRole).toString() == QLatin1String("prod-gold-ore-pump")
                                  ? Qt::Checked
                                  : Qt::Unchecked);
        }

        bool verifiedDialogSelectionAndThumb = false;
        QTimer::singleShot(0, [&]() {
            if (auto *abDlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            {
                auto *list = abDlg->findChild<QListWidget *>("autoBatchProductsList");
                auto *previewLbl = abDlg->findChild<QLabel *>("autoBatchProductPreview");
                if (list && list->count() == 2 && previewLbl)
                {
                    auto *firstItem = list->item(0);
                    auto *secondItem = list->item(1);
                    verifiedDialogSelectionAndThumb =
                        firstItem->data(Qt::UserRole).toString() == QLatin1String("prod-gold-ore-pump")
                        && firstItem->checkState() == Qt::Checked
                        && !firstItem->isHidden()
                        && !firstItem->icon().isNull()
                        && secondItem->isHidden()
                        && !previewLbl->pixmap().isNull();
                }
                abDlg->reject();
            }
        });
        if (auto *abBtn = pane.findChild<QPushButton *>("researchAutoBatchProducts"))
        {
            abBtn->click();
        }
        requireResearch(verifiedDialogSelectionAndThumb,
                        "Auto-Batch dialog shows pre-selected products at top with shoe thumbnails and photo preview");
    }

    pane.close();

    // Exercise the full Generation workflow on that Pinterest video project:
    // 1. DialogGenerationOptions preselects ReplaceReferenceProduct
    // 2. Image CLI receives reference_scene.png (extracted from .mp4) + my_product.<ext> -> produces generation_source.png
    // 3. Review dialog shows product-swapped starting image before animating
    // 4. Suggestion CLI receives chronological keyframes reference_frame_1..4.png -> suggests Pinterest motion prompt
    // 5. DialogGenerationPlan pre-fills motion prompt & checks Video backend -> animates generation_source.png!
    {
        QTemporaryDir genWorkTmp;
        const QDir genWork(genWorkTmp.path());
        WorkingDirectoryManager::instance()->setWorkingDir(genWork);

        const QString mockCliScript = root.filePath("mock_product_swap_cli.py");
        writeResearch(mockCliScript, QByteArrayLiteral(
            "import json, os, re, shutil, sys\n"
            "prompt = sys.stdin.read()\n"
            "if 'auto_plan.json' in prompt:\n"
            "    assert '\"hooks\"' in prompt, 'auto_plan must request full hooks + hashtags pool'\n"
            "    out = {\n"
            "        'slots': [\n"
            "            {'slot': 0, 'prompt': 'Editorial street look showcasing Studio Black Heels', 'hook': 'Stop scrolling for these heels', 'description': 'Studio shoes styled right #highheels #shoes', 'hookCategory': 'contrarian'},\n"
            "            {'slot': 1, 'prompt': 'Four-slide outfit breakdown featuring Studio Black Heels', 'hook': '4 ways to style black heels', 'description': 'Carousel shoes styling #highheels #shoes', 'hookCategory': 'direct_value'},\n"
            "            {'slot': 2, 'prompt': 'Generate a video: Confident walk down marble stairs in Studio Black Heels', 'hook': 'Wait for the heel walk', 'description': 'Video shoes walk #highheels #shoes', 'hookCategory': 'transformation'}\n"
            "        ],\n"
            "        'hooks': [\n"
            "            {'hook': 'Stop scrolling for these heels', 'description': 'Studio shoes styled right #highheels #ootdfashion #shoes'},\n"
            "            {'hook': '4 ways to style black heels', 'description': 'Carousel shoes styling #highheels #ootdfashion #shoes'},\n"
            "            {'hook': 'Wait for the heel walk', 'description': 'Video shoes walk #highheels #ootdfashion #shoes'},\n"
            "            {'hook': 'The secret to effortless evening outfits', 'description': 'Classic black shoes look #highheels #ootdfashion #shoes'},\n"
            "            {'hook': 'Would you wear these to dinner?', 'description': 'Night out shoes #highheels #ootdfashion #shoes'}\n"
            "        ],\n"
            "        'newProperties': [\n"
            "            {'property': 'Setting', 'values': [{'name': 'Marble Lobby', 'fragment': 'in a sunlit marble hotel lobby'}, {'name': 'Parisian Café', 'fragment': 'outside a chic Parisian café'}], 'used': 'Marble Lobby'}\n"
            "        ]\n"
            "    }\n"
            "    with open('auto_plan.json', 'w', encoding='utf-8') as f:\n"
            "        json.dump(out, f)\n"
            "    print('done')\n"
            "elif 'RASTER_IMAGE_GENERATION' in prompt:\n"
            "    m = re.search(r\"as exactly:? '?([A-Za-z0-9_.-]+\\.png)'?\", prompt)\n"
            "    out_name = m.group(1) if m else 'generation_source.png'\n"
            "    src = 'reference_scene.png' if os.path.isfile('reference_scene.png') else [f for f in os.listdir('.') if f.startswith('my_product.')][0]\n"
            "    shutil.copyfile(src, out_name)\n"
            "    print(out_name)\n"
            "elif 'suggestions.json' in prompt:\n"
            "    assert os.path.isfile('generation_source.png'), 'missing generation_source.png'\n"
            "    assert os.path.isfile('reference_frame_1.png'), 'missing extracted video motion frame'\n"
            "    assert '\"videoPrompts\"' in prompt, 'missing videoPrompts request in metaPrompt'\n"
            "    out = {\n"
            "        'hooks': [{'hook': 'Wait for the heel reveal', 'description': 'Studio heels outfit #heels'}],\n"
            "        'videoPrompts': ['Generate a video: Woman steps down marble stairs showcasing Studio Black Heels']\n"
            "    }\n"
            "    with open('suggestions.json', 'w', encoding='utf-8') as f:\n"
            "        json.dump(out, f)\n"
            "    print('done')\n"
            "elif 'scheduling an automated social-media publishing queue' in prompt:\n"
            "    codes = re.findall(r'code=(QS-[0-9A-Z]+)', prompt)\n"
            "    plan = []\n"
            "    for idx, c in enumerate(codes[:2]):\n"
            "        plan.append({'shortCode': c, 'accountIndex': 0, 'delayMinutes': 0 if idx == 0 else 240, 'reason': f'AI variety slot #{idx+1}'})\n"
            "    print(json.dumps({'plan': plan}))\n"
            "elif 'Write ONE high-converting TikTok post hook and SEO description' in prompt:\n"
            "    print(json.dumps({'hook': 'How do you rate these black heels?', 'description': 'Classic Studio black shoes styled for evening elegance #highheels #shoes #ootdfashion #elegance'}))\n"
            "else:\n"
            "    print('{\"ok\": true, \"reason\": \"Matches brief\"}')\n"
        ));
        ProductSwapCliFixture swapCli;
        swapCli.scriptPath = mockCliScript;
        ResearchVideoBackendFixture researchVideoBackend;
        AbstractVideoGenerator::Recorder videoReg(&researchVideoBackend);

        TreeProperties gProps(genWork.filePath("properties.json"));
        TreeProperties gArchive(genWork.filePath("archive.json"));
        TableVideos gVideos(genWork.path());
        TableGenerationSettings gSettings(genWork.path());
        PreferredHashtags gHashtags(genWork.path());
        gHashtags.setHashtags({QStringLiteral("#highheels"), QStringLiteral("#ootdfashion")});
        PromptLessons gLessons(genWork.path());
        FavoriteHooks gHooks(genWork.path());
        FavoriteVideoPrompts gFavPrompts(genWork.path());
        SavedPrompts gSavedPrompts(genWork.path());

        PaneGeneration genPane(&gProps, &gArchive, &gVideos, &gSettings, &gHashtags,
                               &gLessons, &gHooks, &gFavPrompts, &gSavedPrompts);
        genPane.setAvailableClis({&swapCli});
        genPane.resize(1280, 820);
        genPane.show();

        requireResearch(genPane.createResearchProject(emittedVideoProjectCtx, emittedProductImg),
                        "createResearchProject succeeds in PaneGeneration");
        auto *toolBox = genPane.findChild<QToolBox *>("toolBox");
        requireResearch(toolBox && toolBox->currentIndex() == 1,
                        "createResearchProject switches directly to Generation page so Generate is visible");

        bool sawReplaceModePreselected = false;
        bool sawReviewSwappedDialog = false;
        bool sawPrefilledMotionPlan = false;
        QTimer modalDriver;
        modalDriver.setInterval(25);
        QObject::connect(&modalDriver, &QTimer::timeout, [&]() {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            if (auto *optDlg = qobject_cast<DialogGenerationOptions *>(modal))
            {
                sawReplaceModePreselected =
                    (optDlg->imageMode() == DialogGenerationOptions::ImageMode::ReplaceReferenceProduct);
                optDlg->accept();
            }
            else if (modal->objectName() == QLatin1String("dialogReviewSwappedImage"))
            {
                sawReviewSwappedDialog = true;
                if (auto *acceptBtn = modal->findChild<QPushButton *>("buttonAcceptSwappedImage"))
                {
                    acceptBtn->click();
                }
            }
            else if (auto *planDlg = qobject_cast<DialogGenerationPlan *>(modal))
            {
                const auto p = planDlg->plan();
                sawPrefilledMotionPlan = (!p.oneImage && !p.slideshow && !p.videos.isEmpty()
                    && p.videos.first().prompt.contains("Woman steps down marble stairs showcasing Studio Black Heels"));
                // Route test execution exclusively to the offline ResearchVideoBackendFixture
                for (auto *grp : planDlg->findChildren<QGroupBox *>())
                {
                    if (auto *sec = dynamic_cast<GenerationPlanSection *>(grp))
                    {
                        sec->setChecked(sec->title().contains("Gemini Research Test"));
                    }
                }
                planDlg->accept();
            }
            else if (auto * anyDlg = qobject_cast<QDialog *>(modal))
            {
                // Do not auto-close the non-modal/progress dialog unless finished
                if (anyDlg->windowTitle().contains("New A/B"))
                {
                    anyDlg->reject();
                }
            }
        });
        modalDriver.start();

        auto *genBtn = genPane.findChild<QPushButton *>("buttonGenerate");
        requireResearch(genBtn != nullptr, "Generate button exists in PaneGeneration");
        genBtn->click();

        QElapsedTimer waitTimer;
        waitTimer.start();
        while ((researchVideoBackend.prompts.isEmpty() || !genPane.isEnabled()) && waitTimer.elapsed() < 20000)
        {
            QApplication::processEvents();
            QThread::msleep(15);
        }
        modalDriver.stop();

        const QDir createdProjDir = genPane.projectsModel()->projectDir(0);
        requireResearch(sawReplaceModePreselected,
                        "DialogGenerationOptions preselects ReplaceReferenceProduct for Research reference project");
        requireResearch(sawReviewSwappedDialog
                            && QFileInfo::exists(createdProjDir.filePath("generation_source.png")),
                        "Step 1 regenerates product-swapped starting image, persists it on project, and shows review dialog");
        requireResearch(sawPrefilledMotionPlan
                            && !researchVideoBackend.prompts.isEmpty()
                            && researchVideoBackend.prompts.first().contains("Woman steps down marble stairs showcasing Studio Black Heels")
                            && !researchVideoBackend.imagesPerCall.isEmpty()
                            && !researchVideoBackend.imagesPerCall.first().isEmpty()
                            && researchVideoBackend.imagesPerCall.first().first().endsWith("generation_source.png"),
                        "Step 2 animates the product-swapped generation_source.png using the Pinterest video's motion prompt");

        // Now verify unattended Auto-Batch Mode (1 product × 3 generations: Image + Carousel + Video)
        // runs to completion with ZERO modal popups, reuses the product's project row, seeds A/B
        // properties automatically, attaches hooks + qsXXXX codes, and saves creative_dna.json!
        const int recordsBeforeBatch = gVideos.rowCount();
        bool sawUnexpectedModalDuringAutoBatch = false;
        requireResearch(genPane.startAutoBatch(autoBatchSpec), "startAutoBatch launches unattended campaign");
        QElapsedTimer autoTimer;
        autoTimer.start();
        while ((gVideos.rowCount() < recordsBeforeBatch + 3 || !genPane.isEnabled())
               && autoTimer.elapsed() < 30000)
        {
            if (QApplication::activeModalWidget() != nullptr)
            {
                sawUnexpectedModalDuringAutoBatch = true;
                QApplication::activeModalWidget()->close();
            }
            QApplication::processEvents();
            QThread::msleep(15);
        }
        genPane.close();

        requireResearch(!sawUnexpectedModalDuringAutoBatch,
                        "Auto-Batch runs completely unattended with zero modal popups");
        requireResearch(genPane.projectsModel()->rowCount() == 1
                            && gVideos.rowCount() == recordsBeforeBatch + 3,
                        "Auto-Batch reuses existing product project and creates all 3 requested creatives");
        requireResearch(!gProps.topLevelNodes().isEmpty(),
                        "Auto-Batch automatically seeds initial A/B properties when catalog is empty");

        const auto &registeredMethods = InspirationAdaptationMethod::all();
        requireResearch(registeredMethods.contains("reference_swap")
                            && registeredMethods.contains("reference_variant")
                            && registeredMethods.contains("reference_properties_full")
                            && registeredMethods.contains("editorial_ab")
                            && !registeredMethods.value("reference_swap")->usesSceneProperties()
                            && registeredMethods.value("reference_variant")->usesSceneProperties()
                            && registeredMethods.value("reference_properties_full")->usesSceneProperties(),
                        "InspirationAdaptationMethod::Recorder registers both property-skipping and property-using methods");

        QSet<QString> seenFormats;
        bool sawSkippedPropsMethod = false;
        bool sawUsedPropsMethod = false;
        for (const auto *rec : gVideos.recordsForProject(genPane.projectsModel()->projectId(0)))
        {
            const QDir gDir = genPane.projectsModel()->generationDir(0, rec->shortCode, false);
            const QString dnaPath = gDir.filePath("creative_dna.json");
            if (!QFileInfo::exists(dnaPath))
            {
                continue; // skip the earlier manual test record
            }
            const QJsonObject dna = QJsonDocument::fromJson(readResearch(dnaPath)).object();
            const QString fmt = dna.value("format").toString();
            const QString strat = dna.value("strategy").toString();
            const bool usedProps = dna.value("usedSceneProperties").toBool(true);
            if (usedProps) sawUsedPropsMethod = true;
            else sawSkippedPropsMethod = true;
            seenFormats.insert(fmt);
            const QString hookDesc = QString::fromUtf8(readResearch(gDir.filePath("hook-description.txt")));
            const QJsonObject hooksJson = QJsonDocument::fromJson(readResearch(gDir.filePath("hooks.json"))).object();
            const bool propsConsistent = usedProps
                ? !dna.value("propertyValueIds").toArray().isEmpty()
                : dna.value("propertyValueIds").toArray().isEmpty();
            requireResearch(hookDesc.contains("qs", Qt::CaseInsensitive)
                                && hookDesc.contains("#highheels", Qt::CaseInsensitive)
                                && hooksJson.value("hooks").toArray().size() >= 5
                                && !dna.value("productId").toString().isEmpty()
                                && !dna.value("referenceId").toString().isEmpty()
                                && registeredMethods.contains(strat)
                                && propsConsistent,
                            "each Auto-Batch creative has hook-description.txt, multi-option hooks.json, Recorder strategy, and consistent propertyValueIds");
            if (fmt == QLatin1String("image"))
            {
                requireResearch(gDir.entryList({"*.png"}, QDir::Files).size() == 1,
                                "Auto-Batch image slot produces 1 PNG");
            }
            else if (fmt == QLatin1String("carousel"))
            {
                requireResearch(gDir.entryList({"*.png"}, QDir::Files).size() == 4,
                                "Auto-Batch carousel slot produces 4 coherent PNG slides");
            }
            else if (fmt == QLatin1String("video"))
            {
                requireResearch(gDir.entryList({"*.mp4"}, QDir::Files).size() == 1,
                                "Auto-Batch video slot produces animated MP4");
            }
        }
        requireResearch(seenFormats == QSet<QString>({"image", "carousel", "video"}),
                        "Auto-Batch diversifies across all ticked formats (Image, Carousel, Video)");
        requireResearch(sawSkippedPropsMethod && sawUsedPropsMethod,
                        "Auto-Batch tests both skipping scene Properties (reference_swap) and applying scene Properties");

        // Verify PaneGeneration "Auto-publish…" button emits autoPublishRequested()
        bool autoPublishSignalEmitted = false;
        QObject::connect(&genPane, &PaneGeneration::autoPublishRequested, &genPane,
                         [&]() { autoPublishSignalEmitted = true; });
        auto *autoPubBtn = genPane.findChild<QPushButton *>("buttonAutoPublish");
        requireResearch(autoPubBtn != nullptr, "PaneGeneration exposes Auto-publish… button");
        autoPubBtn->click();
        requireResearch(autoPublishSignalEmitted, "Clicking Auto-publish… emits autoPublishRequested()");

        // Verify SocialAccountTiktok supports auto-publishing
        auto *tiktokPlatform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS().value("tiktok");
        requireResearch(tiktokPlatform && tiktokPlatform->supportsAutoPublish(),
                        "SocialAccountTiktok supports automated browser publishing");

        // Backdate the Auto-Batch Video and 4-slide Carousel creatives in videos.json to 12 days old,
        // leaving the other 2 creatives at 0 days old so we test default minDaysOld = 10 filtering!
        QString agedCarouselCode;
        QString agedVideoCode;
        for (const auto *rec : gVideos.recordsForProject(genPane.projectsModel()->projectId(0)))
        {
            const QDir gDir = genPane.projectsModel()->generationDir(0, rec->shortCode, false);
            const QString dnaPath = gDir.filePath("creative_dna.json");
            if (!QFileInfo::exists(dnaPath))
            {
                continue;
            }
            const QString fmt = QJsonDocument::fromJson(readResearch(dnaPath)).object().value("format").toString();
            if (fmt == QLatin1String("carousel"))
            {
                agedCarouselCode = rec->shortCode;
            }
            else if (fmt == QLatin1String("video"))
            {
                agedVideoCode = rec->shortCode;
            }
        }
        requireResearch(!agedCarouselCode.isEmpty() && !agedVideoCode.isEmpty(),
                        "located Auto-Batch carousel and video shortCodes for publishing test");
        {
            const QString videosJsonPath = genWork.filePath("videos.json");
            QJsonObject vRoot = QJsonDocument::fromJson(readResearch(videosJsonPath)).object();
            QJsonArray vArr = vRoot.value("videos").toArray();
            const QString twoDaysAgo = QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODate);
            const QString twentyFiveDaysAgo = QDateTime::currentDateTimeUtc().addDays(-25).toString(Qt::ISODate);
            for (int i = 0; i < vArr.size(); ++i)
            {
                QJsonObject vo = vArr[i].toObject();
                const QString sc = vo.value("shortCode").toString();
                vo.insert("generatedDate",
                          (sc == agedCarouselCode || sc == agedVideoCode) ? twoDaysAgo : twentyFiveDaysAgo);
                vArr[i] = vo;
            }
            vRoot.insert("videos", vArr);
            writeResearch(videosJsonPath, QJsonDocument(vRoot).toJson(QJsonDocument::Indented));
        }

        // Simulate a creative missing hook-description.txt & hooks.json
        // to verify PanePublishing auto-generates a full hook + description + #hashtags before posting!
        {
            const QDir legacyDir = genPane.projectsModel()->generationDir(0, agedVideoCode, false);
            const QDir legacyTemp = genPane.projectsModel()->generationTempDir(0, agedVideoCode);
            QFile::remove(legacyDir.filePath("hook-description.txt"));
            QFile::remove(legacyDir.filePath("hooks.json"));
            QFile::remove(legacyTemp.filePath("hooks.json"));
        }

        // Reload fresh models from genWork (shared between PaneGeneration and PanePublishing just like MainWindow)
        TableAccounts pubAccounts(genWork.path());
        pubAccounts.addAccount();
        pubAccounts.setData(pubAccounts.index(0, TableAccounts::IND_TYPE), QStringLiteral("TikTok"));
        pubAccounts.setData(pubAccounts.index(0, TableAccounts::IND_URL),
                            QStringLiteral("https://www.tiktok.com/@pradize.high.heel"));
        TableVideos pubVideos(genWork.path());
        PaneGeneration liveGenPane(&gProps, &gArchive, &pubVideos, &gSettings, &gHashtags,
                                   &gLessons, &gHooks, &gFavPrompts, &gSavedPrompts);
        liveGenPane.show();
        TableProjects &pubProjects = *liveGenPane.projectsModel();

        PanePublishing panePub(&pubAccounts, &pubProjects, &pubVideos, {&swapCli}, nullptr,
                               &gHashtags, &gHooks);
        panePub.resize(1100, 760);
        panePub.show();

        // Check dialog defaults: maxDaysOld == 10 (created <= 10 days ago), TikTok pre-checked,
        // 2 recent creatives ready (<=10d: 1 video + 1 carousel), 1 older video (>10d) skipped,
        // and 1 single-image creative skipped for TikTok!
        bool verifiedPublishDialogDefaults = false;
        QTimer::singleShot(0, [&]() {
            if (auto *pubDlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            {
                auto *spinMaxAge = pubDlg->findChild<QSpinBox *>("spinPublishMaxDaysOld");
                auto *spinPerDay = pubDlg->findChild<QSpinBox *>("spinPublishPostsPerDay");
                auto *radioDay12 = pubDlg->findChild<QRadioButton *>("radioSpacingDaytime12h");
                auto *radioEven24 = pubDlg->findChild<QRadioButton *>("radioSpacingEven24h");
                auto *radioFixed = pubDlg->findChild<QRadioButton *>("radioSpacingFixed");
                auto *spinFixed = pubDlg->findChild<QSpinBox *>("spinPublishFixedMinutes");
                auto *accList = pubDlg->findChild<QListWidget *>("autoPublishAccountsList");
                auto *countLbl = pubDlg->findChild<QLabel *>("labelEligiblePublishCount");
                if (spinMaxAge && spinPerDay && radioDay12 && radioEven24 && radioFixed && spinFixed
                    && accList && accList->count() == 1 && countLbl)
                {
                    spinPerDay->setValue(6);
                    radioFixed->setChecked(true);
                    verifiedPublishDialogDefaults =
                        spinMaxAge->value() == 10
                        && radioDay12->text().contains("120 min")
                        && radioEven24->text().contains("240 min")
                        && spinFixed->isEnabled()
                        && spinFixed->value() == 60
                        && accList->item(0)->checkState() == Qt::Checked
                        && countLbl->text().contains("2 unpublished creative(s) created")
                        && countLbl->text().contains("1 older > 10d skipped")
                        && countLbl->text().contains("1 single-image skipped for TikTok");
                }
                pubDlg->reject();
            }
        });
        panePub.openAutoPublishDialog();
        requireResearch(verifiedPublishDialogDefaults,
                        "Auto-Publish dialog defaults to max 10 days old, pre-checks TikTok, and skips both older >10d and single-image creatives");

        // Create offline TikTok Studio HTML fixture verifying @pradize.high.heel, multi-file upload,
        // Photo Mode '+ Ajouter un son' + catchy titleInput, Video Mode 'Sons', contenteditable caption with qsXXXX code, and Publier!
        const QString pubLogDir = genWork.filePath("publish_logs");
        QDir().mkpath(pubLogDir);
        qputenv("QS_PUBLISH_HEADLESS", "1");
        qputenv("QS_PUBLISH_LOG_DIR", pubLogDir.toUtf8());

        const QString studioHtmlPath = genWork.filePath("tiktok_studio_fixture.html");
        writeResearch(studioHtmlPath, QByteArrayLiteral(
            "<!doctype html><html><head><meta charset='utf-8'><title>TikTok Studio Upload</title></head><body>"
            "<header><a href='/@pradize.high.heel'>@pradize.high.heel</a></header>"
            "<input id='ttFile' type='file' accept='video/*,image/*' multiple />"
            "<div id='editorWrap' style='display:none; margin-top:16px;'>"
            "  <input id='photoTitleInp' class='titleInput-JiU8Rn' placeholder='Ajoute un titre accrocheur' style='display:none;' />"
            "  <div id='capEd' contenteditable='true' data-e2e='caption-editor' style='min-height:60px;border:1px solid #888;'></div>"
            "  <button id='sonsBtn' type='button' class='editor-entrance' style='margin-left:340px;'>Sons</button>"
            "  <button id='photoSoundBtn' type='button' style='display:none;margin-left:340px;'>Ajouter un son</button>"
            "  <button id='postBtn' data-e2e='post_video_button' disabled>Publier</button>"
            "</div>"
            "<div id='musicModalWrap'></div>"
            "<div id='doneMsg' style='display:none;'></div>"
            "<script>"
            "const inp = document.getElementById('ttFile');"
            "const wrap = document.getElementById('editorWrap');"
            "const ed = document.getElementById('capEd');"
            "const btn = document.getElementById('postBtn');"
            "function checkReady() {"
            "  const hasFiles = inp.files && inp.files.length > 0;"
            "  const txt = (ed.innerText || '');"
            "  btn.disabled = !(hasFiles && txt.length >= 25 && /#[A-Za-z0-9_]+/.test(txt) && /qs[0-9a-z]{4}/i.test(txt));"
            "}"
            "document.getElementById('sonsBtn').addEventListener('click', () => {"
            "  document.getElementById('musicModalWrap').innerHTML = "
            "    '<div class=\"MusicPanelContainer__root\" style=\"position:fixed;inset:20px;background:#fff;z-index:5000;\">'"
            "    + '<div class=\"EditorLeftSideMenu__menuItem\" style=\"position:fixed;left:14px;top:140px;width:56px;height:54px;\" onclick=\"document.getElementById(\\'textSubPanel\\').style.display=\\'block\\'\">Texte</div>'"
            "    + '<div id=\"textSubPanel\" style=\"display:none;\"><button type=\"button\" class=\"AddTextPanel__addTextBasicButton\" onclick=\"document.getElementById(\\'textPropWrap\\').style.display=\\'block\\'\">Ajouter du texte</button></div>'"
            "    + '<div id=\"textPropWrap\" style=\"display:none;\"><textarea class=\"PropSettingTextArea__input\">Text</textarea></div>'"
            "    + '<div class=\"MusicPanelMusicItem__wrap\"><div>Trending Runway Beat</div><div>00:30 · Studio</div><button type=\"button\" class=\"Button__root--type-primary\">+</button></div>'"
            "    + '<button type=\"button\" onclick=\"document.getElementById(\\'musicModalWrap\\').innerHTML=\\'\\'\">Enregistrer</button>'"
            "    + '</div>';"
            "});"
            "document.getElementById('photoSoundBtn').addEventListener('click', () => {"
            "  document.getElementById('musicModalWrap').innerHTML = "
            "    '<div class=\"MusicPickerView__root\" style=\"position:fixed;inset:20px;background:#fff;z-index:5000;\">'"
            "    + '<div><div>Trending Runway Beat</div><div>01:00</div><button type=\"button\" class=\"MusicPickerView__addButton\" onclick=\"document.getElementById(\\'musicModalWrap\\').innerHTML=\\'\\'\">Use</button></div>'"
            "    + '</div>';"
            "});"
            "inp.addEventListener('change', () => {"
            "  if (inp.files.length > 0) {"
            "    wrap.style.display = 'block';"
            "    const isPhoto = Array.from(inp.files).every(f => /\\.(png|jpe?g|webp)$/i.test(f.name));"
            "    document.getElementById('sonsBtn').style.display = isPhoto ? 'none' : 'inline-block';"
            "    document.getElementById('photoSoundBtn').style.display = isPhoto ? 'inline-block' : 'none';"
            "    document.getElementById('photoTitleInp').style.display = isPhoto ? 'block' : 'none';"
            "    const tux = document.createElement('div');"
            "    tux.className = 'TUXModal-overlay';"
            "    tux.style.cssText = 'position:fixed;inset:0;z-index:9999;background:rgba(0,0,0,0.4);';"
            "    tux.innerHTML = '<button type=\"button\" onclick=\"this.parentElement.remove()\">Activer</button>';"
            "    document.body.appendChild(tux);"
            "    const jr = document.createElement('div');"
            "    jr.id = 'react-joyride-portal';"
            "    jr.innerHTML = '<div role=\"presentation\" data-test-id=\"overlay\" class=\"react-joyride__overlay\" style=\"position:fixed;inset:0;z-index:9998;background:rgba(0,0,0,0.3);\"><button type=\"button\" onclick=\"document.getElementById(\\'react-joyride-portal\\').remove()\">J\\'ai compris</button></div>';"
            "    document.body.appendChild(jr);"
            "  }"
            "  checkReady();"
            "});"
            "ed.addEventListener('input', checkReady);"
            "btn.addEventListener('click', () => {"
            "  const names = Array.from(inp.files).map(f => f.name).join(',');"
            "  const d = document.getElementById('doneMsg');"
            "  d.textContent = 'Ta vidéo a été publiée — Gérer tes publications (' + names + ')';"
            "  d.style.display = 'block';"
            "});"
            "</script></body></html>"));

        PanePublishing::PublishJobConfig jobCfg;
        jobCfg.targets.append(PanePublishing::AccountTarget{
            QStringLiteral("tiktok"),
            QStringLiteral("TikTok"),
            QStringLiteral("https://www.tiktok.com/@pradize.high.heel"),
            1200
        });
        jobCfg.maxDaysOld = 10;
        jobCfg.postsPerDay = 6;
        jobCfg.totalPosts = 2;
        jobCfg.spacingMode = PanePublishing::SpacingMode::FixedInterval;
        jobCfg.fixedIntervalMinutes = 60;
        jobCfg.useSystemChromeProfile = false;
        jobCfg.dryRun = false;
        jobCfg.reviewBeforePublish = true;
        jobCfg.plannerCli = &swapCli;
        jobCfg.uploadUrlOverride = QStringLiteral("file://") + studioHtmlPath;

        // 1) Test Step 2 Review & Customize Selection Dialog when selecting 1 out of 3 eligible candidates:
        //    verify AI picks 1 checked at #1 + 2 unchecked candidates below, all with preview thumbnails & details,
        //    toggle selection + Move to #1, then Cancel cleanly without publishing.
        {
            PanePublishing::PublishJobConfig cancelReviewCfg = jobCfg;
            cancelReviewCfg.maxDaysOld = 0; // 3 eligible TikTok candidates (1 older video + 1 carousel + 1 video)
            cancelReviewCfg.totalPosts = 1;

            bool verifiedSelectionSwapAndPreviews = false;
            QTimer reviewModalTimer;
            reviewModalTimer.setInterval(30);
            QObject::connect(&reviewModalTimer, &QTimer::timeout, [&]() {
                auto *revDlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!revDlg || revDlg->objectName() != QLatin1String("dialogReviewPublishPlan"))
                {
                    return;
                }
                reviewModalTimer.stop();
                auto *candTable = revDlg->findChild<QTableWidget *>("tableReviewCandidates");
                auto *largePrev = revDlg->findChild<QLabel *>("labelReviewLargePreview");
                auto *detailsEd = revDlg->findChild<QPlainTextEdit *>("textReviewCandidateDetails");
                auto *btnMoveTop = revDlg->findChild<QPushButton *>("buttonMoveCandidateTop");
                auto *summaryLbl = revDlg->findChild<QLabel *>("labelSelectedPublishSummary");
                if (candTable && candTable->rowCount() == 3 && largePrev && detailsEd && btnMoveTop && summaryLbl)
                {
                    bool allHaveThumbs = true;
                    for (int r = 0; r < 3; ++r)
                    {
                        const QPixmap px = candTable->item(r, 1)->data(Qt::DecorationRole).value<QPixmap>();
                        if (px.isNull() || candTable->item(r, 2)->text().isEmpty())
                        {
                            allHaveThumbs = false;
                        }
                    }
                    const bool initialAiPickOk =
                        candTable->item(0, 0)->checkState() == Qt::Checked
                        && candTable->item(1, 0)->checkState() == Qt::Unchecked
                        && candTable->item(2, 0)->checkState() == Qt::Unchecked
                        && summaryLbl->text().contains("1 of 3")
                        && !largePrev->pixmap().isNull()
                        && detailsEd->toPlainText().contains("Code:");

                    // Uncheck AI's #1 pick and promote candidate row 2 to #1!
                    const QString thirdCandidateDetails = candTable->item(2, 2)->text().section(' ', 0, 0);
                    candTable->item(0, 0)->setCheckState(Qt::Unchecked);
                    candTable->selectRow(2);
                    btnMoveTop->click();

                    const bool swapOrderOk =
                        candTable->item(0, 0)->checkState() == Qt::Checked
                        && candTable->item(0, 0)->text().contains("Publish #1")
                        && candTable->item(0, 2)->text().startsWith(thirdCandidateDetails)
                        && summaryLbl->text().contains("1 of 3");

                    verifiedSelectionSwapAndPreviews = allHaveThumbs && initialAiPickOk && swapOrderOk;
                }
                revDlg->reject();
            });
            reviewModalTimer.start();
            requireResearch(panePub.startPublishJob(cancelReviewCfg), "start 1-of-3 review cancel test");
            QElapsedTimer cancelWait;
            cancelWait.start();
            while (panePub.isJobRunning() && cancelWait.elapsed() < 15000)
            {
                QApplication::processEvents();
                QThread::msleep(20);
            }
            reviewModalTimer.stop();
            requireResearch(verifiedSelectionSwapAndPreviews && !panePub.isJobRunning(),
                            "Step 2 Review dialog displays all 3 candidates with preview images, allows changing selection & order, and cancels cleanly");
        }

        // 2) Now run full 2-slot publish job with Step 2 Review dialog active:
        //    inspect multi-slide carousel preview strip, swap #1 and #2 order via Move Down, and confirm!
        QString expectedFirstCodeAfterReorder;
        bool verifiedReorderAndConfirm = false;
        QTimer confirmModalTimer;
        confirmModalTimer.setInterval(30);
        QObject::connect(&confirmModalTimer, &QTimer::timeout, [&]() {
            auto *revDlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!revDlg || revDlg->objectName() != QLatin1String("dialogReviewPublishPlan"))
            {
                return;
            }
            confirmModalTimer.stop();
            auto *candTable = revDlg->findChild<QTableWidget *>("tableReviewCandidates");
            auto *slideStrip = revDlg->findChild<QWidget *>("widgetReviewSlideStrip");
            auto *videoBar = revDlg->findChild<QWidget *>("widgetVideoTransportBar");
            auto *btnPlayVid = revDlg->findChild<QPushButton *>("buttonPlayPauseCandidateVideo");
            auto *prevStack = revDlg->findChild<QStackedWidget *>("stackedReviewPreview");
            auto *vidWidget = revDlg->findChild<QVideoWidget *>("videoReviewPlayerWidget");
            auto *vidPlayer = revDlg->findChild<QMediaPlayer *>("playerReviewVideo");
            auto *btnMoveDown = revDlg->findChild<QPushButton *>("buttonMoveCandidateDown");
            if (candTable && candTable->rowCount() == 2 && slideStrip && videoBar
                && btnPlayVid && prevStack && vidWidget && vidPlayer && btnMoveDown)
            {
                // 1. Select the VIDEO row, test inline video playback, AND customize native TikTok video text!
                bool inlineVideoPlayed = false;
                bool videoTextCustomized = false;
                auto *chkVidTxt = revDlg->findChild<QCheckBox *>("checkReviewVideoTextOverlay");
                auto *editVidTxt = revDlg->findChild<QLineEdit *>("editReviewVideoOverlayText");
                for (int r = 0; r < 2; ++r)
                {
                    if (candTable->item(r, 2)->text().contains("VIDEO"))
                    {
                        candTable->selectRow(r);
                        QApplication::processEvents();
                        const bool barVisible = videoBar->isVisible() && !slideStrip->isVisible();
                        btnPlayVid->click();
                        QApplication::processEvents();
                        inlineVideoPlayed =
                            barVisible
                            && prevStack->currentWidget() == vidWidget
                            && vidPlayer->source().isLocalFile();
                        auto *comboVidStyle = revDlg->findChild<QComboBox *>("comboReviewVideoTextStyle");
                        if (chkVidTxt && editVidTxt && comboVidStyle && chkVidTxt->isVisible()
                            && comboVidStyle->count() == 4)
                        {
                            chkVidTxt->setChecked(true);
                            const int serifIdx = comboVidStyle->findData(QStringLiteral("elegance_serif"));
                            if (serifIdx >= 0)
                            {
                                comboVidStyle->setCurrentIndex(serifIdx);
                            }
                            editVidTxt->setText(QStringLiteral("Custom Edited Runway Hook"));
                            emit editVidTxt->textEdited(QStringLiteral("Custom Edited Runway Hook"));
                            QApplication::processEvents();
                            videoTextCustomized =
                                candTable->item(r, 2)->text().contains("Custom Edited Runway Hook")
                                && candTable->item(r, 2)->text().contains("Elegance Serif");
                        }
                        break;
                    }
                }

                // 2. Select the CAROUSEL row to verify inline video stops and multi-slide strip renders
                for (int r = 0; r < 2; ++r)
                {
                    if (candTable->item(r, 2)->text().contains("SLIDESHOW"))
                    {
                        candTable->selectRow(r);
                        QApplication::processEvents();
                        break;
                    }
                }
                const bool slideStripShown = slideStrip->isVisible() && !videoBar->isVisible();

                const QString origSecondCode = candTable->item(1, 2)->text().section(' ', 0, 0);
                candTable->selectRow(0);
                btnMoveDown->click();

                expectedFirstCodeAfterReorder = origSecondCode;
                verifiedReorderAndConfirm =
                    inlineVideoPlayed
                    && videoTextCustomized
                    && slideStripShown
                    && candTable->item(0, 2)->text().startsWith(origSecondCode)
                    && candTable->item(0, 3)->text().contains("Immediate (+0m)")
                    && candTable->item(1, 3)->text().contains("+60m");
            }
            revDlg->accept();
        });
        confirmModalTimer.start();

        requireResearch(panePub.startPublishJob(jobCfg), "startPublishJob accepts eligible <=10d creatives");
        auto *pubQueueTable = panePub.findChild<QTableWidget *>("tablePublishQueue");
        auto *pubLogs = panePub.findChild<QPlainTextEdit *>("textPublishLogs");
        auto *btnNextNow = panePub.findChild<QPushButton *>("buttonPublishNextNow");
        requireResearch(pubQueueTable && pubLogs && btnNextNow, "PanePublishing exposes queue table, logs, and Publish Next Now");

        // Wait for Slot #1 (delayMinutes=0) to publish automatically, then trigger Slot #2 (delayMinutes=240) via Publish Next Now
        bool triggeredSecondSlot = false;
        QElapsedTimer pubTimer;
        pubTimer.start();
        while (panePub.isJobRunning() && pubTimer.elapsed() < 45000)
        {
            if (!triggeredSecondSlot && btnNextNow->isEnabled()
                && pubQueueTable->rowCount() == 2
                && pubQueueTable->item(0, 7)
                && pubQueueTable->item(0, 7)->text().startsWith("Published"))
            {
                triggeredSecondSlot = true;
                btnNextNow->click();
            }
            QApplication::processEvents();
            QThread::msleep(25);
        }
        confirmModalTimer.stop();

        requireResearch(verifiedReorderAndConfirm
                            && !expectedFirstCodeAfterReorder.isEmpty()
                            && pubQueueTable->rowCount() == 2
                            && pubQueueTable->item(0, 1)->text() == expectedFirstCodeAfterReorder,
                        "Step 2 Review dialog shows multi-slide previews and executes user-reordered #1/#2 publish queue");
        requireResearch(!panePub.isJobRunning() && triggeredSecondSlot,
                        "Auto-Publish job completes both Slot #1 (immediate) and Slot #2 (Publish Next Now)");
        const QString allLogs = pubLogs->toPlainText();
        requireResearch(allLogs.contains("[LEARNING]")
                            && allLogs.contains("[AI-PLANNER]")
                            && allLogs.contains("[USER-PLAN]")
                            && allLogs.contains("[CAPTION-AUTO]")
                            && allLogs.contains("[SLIDESHOW-TEXT]")
                            && allLogs.contains("[VIDEO-TEXT]")
                            && allLogs.contains("[SOUND]")
                            && allLogs.contains("[UPLOAD]")
                            && allLogs.contains("[CAPTION]")
                            && allLogs.contains("[SUCCESS]")
                            && allLogs.contains("[JOB-DONE]"),
                        "PanePublishing logs measured learning summary, applies native TikTok video text + slideshow text + #1 sound, and completes publish");

        // Both aged creatives (Video + 4-slide Carousel) must now be marked published with linux-chrome DNA
        int publishedAgedCount = 0;
        for (int r = 0; r < pubVideos.rowCount(); ++r)
        {
            const auto *rec = pubVideos.recordAt(r);
            if (rec->shortCode == agedCarouselCode || rec->shortCode == agedVideoCode)
            {
                requireResearch(rec->published && !rec->publications.isEmpty()
                                    && rec->publications.first().platformId == QLatin1String("tiktok"),
                                "TableVideos marks auto-published creative as published with TikTok publication");
                const QDir gDir = pubProjects.generationDir(0, rec->shortCode, false);
                const QJsonObject dna = QJsonDocument::fromJson(readResearch(gDir.filePath("creative_dna.json"))).object();
                requireResearch(dna.value("publishChannel").toString() == QLatin1String("linux-chrome")
                                    && dna.value("publishedCaption").toString().contains('#')
                                    && dna.value("tiktokSound").toString() == QLatin1String("Trending Runway Beat"),
                                "Auto-Publish records publishChannel=linux-chrome, #hashtags caption, and #1 TikTok sound in creative_dna.json for both Video and Photo Slideshow");
                if (rec->shortCode == agedVideoCode)
                {
                    requireResearch(dna.value("mutedOriginalAudio").toBool()
                                        && dna.value("tiktokVideoTextAdded").toBool()
                                        && dna.value("tiktokVideoText").toString() == QLatin1String("Custom Edited Runway Hook")
                                        && dna.value("tiktokVideoTextStyle").toString() == QLatin1String("elegance_serif")
                                        && QFileInfo::exists(gDir.filePath("hook-description.txt")),
                                    "Video publish mutes original audio, applies native TikTok text track + chosen style, and records tiktokVideoText/tiktokVideoTextStyle in creative_dna.json");
                }
                else if (rec->shortCode == agedCarouselCode)
                {
                    const QDir tDir = pubProjects.generationTempDir(0, rec->shortCode);
                    requireResearch(dna.value("hasSlideTextOverlay").toBool()
                                        && QFileInfo::exists(tDir.filePath("slide1_before_overlay.png")),
                                    "Photo Slideshow publish automatically overlays hook text onto Slide 1 and backs up clean Slide 1");
                }
                ++publishedAgedCount;
            }
            else
            {
                requireResearch(!rec->published, "Older >10d creatives remain skipped and unpublished");
            }
        }
        requireResearch(publishedAgedCount == 2,
                        "Both recent (<=10d) Video and 4-slide Carousel were published");

        // Verify PaneGeneration's Generation tree & Mark published button immediately reflect published state!
        auto *genTree = liveGenPane.findChild<QTreeWidget *>("treeViewGenerations");
        auto *markPubBtn = liveGenPane.findChild<QPushButton *>("buttonPublish");
        requireResearch(genTree && markPubBtn, "liveGenPane exposes treeViewGenerations and buttonPublish");
        int greenItemsInGenTree = 0;
        for (int ti = 0; ti < genTree->topLevelItemCount(); ++ti)
        {
            QTreeWidgetItem *tItem = genTree->topLevelItem(ti);
            if (tItem->text(0) == agedVideoCode || tItem->text(0) == agedCarouselCode)
            {
                if (tItem->background(0).style() != Qt::NoBrush)
                {
                    ++greenItemsInGenTree;
                }
                genTree->setCurrentItem(tItem);
                QApplication::processEvents();
                requireResearch(markPubBtn->text() == QLatin1String("Unpublish"),
                                "PaneGeneration buttonPublish automatically switches to Unpublish when published in TikTok");
            }
        }
        requireResearch(greenItemsInGenTree == 2,
                        "PaneGeneration treeViewGenerations highlights newly published TikTok items in green immediately");

        // Verify fatal safety auto-stop: targeting @wrong.handle against @pradize.high.heel Chrome session
        // stops the job automatically and leaves remaining creatives unpublished!
        PanePublishing::PublishJobConfig mismatchCfg = jobCfg;
        mismatchCfg.reviewBeforePublish = false;
        mismatchCfg.maxDaysOld = 0; // allow all ages so the 2 older unpublished creatives are candidates
        mismatchCfg.totalPosts = 2;
        mismatchCfg.targets[0].accountUrl = QStringLiteral("https://www.tiktok.com/@some.other.brand");
        requireResearch(panePub.startPublishJob(mismatchCfg), "start mismatch safety test job");
        QElapsedTimer mismatchTimer;
        mismatchTimer.start();
        while (panePub.isJobRunning() && mismatchTimer.elapsed() < 30000)
        {
            QApplication::processEvents();
            QThread::msleep(25);
        }
        requireResearch(!panePub.isJobRunning()
                            && pubLogs->toPlainText().contains("ACCOUNT-MISMATCH")
                            && pubLogs->toPlainText().contains("[AUTO-STOP]"),
                        "Account mismatch triggers fatal safety auto-stop with clear diagnostic logs");

        // Verify continuous measurement & self-improvement loop for native TikTok video text:
        // record measured views on the with-text video vs a clean video without text, and verify
        // SocialStatistics::summarizeVideoTextExperiment aggregates performance and promotes winning hooks!
        for (int r = 0; r < pubVideos.rowCount(); ++r)
        {
            const auto *rec = pubVideos.recordAt(r);
            if (rec->shortCode == agedVideoCode && !rec->publications.isEmpty())
            {
                AbstractSocialAccount::VideoStatistics vs;
                vs.views = 1850;
                vs.likes = 140;
                vs.comments = 12;
                vs.shares = 18;
                pubVideos.addSnapshot(rec->id, rec->publications.first().postUrl,
                                      AbstractSocialAccount::StatSource::PublicView, vs);
            }
            else if (rec->shortCode == agedCarouselCode && !rec->publications.isEmpty())
            {
                AbstractSocialAccount::VideoStatistics cs;
                cs.views = 940;
                cs.likes = 65;
                cs.comments = 6;
                cs.shares = 22;
                pubVideos.addSnapshot(rec->id, rec->publications.first().postUrl,
                                      AbstractSocialAccount::StatSource::PublicView, cs);
            }
            else if (!rec->published)
            {
                const int pRow = pubProjects.rowOfId(rec->projectId);
                const QDir candGDir = pubProjects.generationDir(qMax(0, pRow), rec->shortCode, false);
                if (!candGDir.entryList({QStringLiteral("*.mp4")}, QDir::Files).isEmpty())
                {
                    pubVideos.setPublished(rec->id, true);
                    const QString cleanUrl = QStringLiteral("https://www.tiktok.com/@pradize.high.heel/video/999");
                    pubVideos.addPublication(rec->id, QStringLiteral("tiktok"), cleanUrl,
                                             QDateTime::currentDateTimeUtc(), 1200);
                    AbstractSocialAccount::VideoStatistics cleanStats;
                    cleanStats.views = 210;
                    cleanStats.likes = 9;
                    cleanStats.comments = 1;
                    cleanStats.shares = 0;
                    pubVideos.addSnapshot(rec->id, cleanUrl,
                                          AbstractSocialAccount::StatSource::PublicView, cleanStats);
                }
            }
        }
        const auto expSummary = SocialStatistics::summarizeVideoTextExperiment(&pubVideos, &gHooks);
        requireResearch(expSummary.withTextCount == 1
                            && qRound64(expSummary.withTextAvgViews) == 1850
                            && expSummary.withoutTextCount == 1
                            && qRound64(expSummary.withoutTextAvgViews) == 210
                            && !expSummary.styleBreakdown.isEmpty()
                            && expSummary.styleBreakdown.first().styleId == QLatin1String("elegance_serif")
                            && qRound64(expSummary.styleBreakdown.first().avgViews) == 1850
                            && !expSummary.winningTexts.isEmpty()
                            && expSummary.winningTexts.first().text == QLatin1String("Custom Edited Runway Hook")
                            && gHooks.hooks(false).contains(QStringLiteral("Custom Edited Runway Hook"))
                            && expSummary.plannerPromptAddendum().contains("Custom Edited Runway Hook")
                            && expSummary.plannerPromptAddendum().contains("elegance_serif"),
                        "SocialStatistics::summarizeVideoTextExperiment measures With-Text vs Clean-Video views, per-style breakdown, promotes winning text hooks, and builds AI learning prompt");

        const auto refExpSummary = SocialStatistics::summarizeReferenceExperiment(&pubVideos);
        requireResearch(refExpSummary.totalMeasuredPosts >= 2
                            && !refExpSummary.references.isEmpty()
                            && refExpSummary.references.first().isWinner
                            && !refExpSummary.strategyBreakdown.isEmpty()
                            && refExpSummary.plannerPromptAddendum().contains("SOURCE VIDEO & ADAPTATION MODE LEARNING"),
                        "SocialStatistics::summarizeReferenceExperiment learns winning source references and adaptation strategies (swap vs variant)");

        // Verify end-to-end 3-Step Guided Workflow Tab (PaneWorkflow):
        // Step 1 (Pick Products & inline generation) -> auto-advance to Step 2 (inline WidgetCreativeReview,
        // AI Select & Order, delete unwanted creative) -> Step 3 (spacing radio buttons + Auto-Publish job)!
        {
            liveGenPane.setAvailableClis({&swapCli});
            ResearchStore wfStore(genWork.path());
            const QString wfAccId = pubAccounts.accountId(0).toString(QUuid::WithoutBraces);
            wfStore.setProfile(wfAccId, QJsonObject{
                {QStringLiteral("brands"), QJsonArray{QStringLiteral("Pradize")}},
                {QStringLiteral("categories"), QJsonArray{QStringLiteral("Heels")}}
            });
            wfStore.replaceProducts(QJsonArray{
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("wf-prod-1")},
                    {QStringLiteral("title"), QStringLiteral("Pradize Velvet Slingback Stiletto")},
                    {QStringLiteral("color"), QStringLiteral("Burgundy")},
                    {QStringLiteral("brand"), QStringLiteral("Pradize")},
                    {QStringLiteral("categories"), QJsonArray{QStringLiteral("Heels")}},
                    {QStringLiteral("localImage"), spyImgPath}
                },
                QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("wf-prod-2")},
                    {QStringLiteral("title"), QStringLiteral("Pradize Crystal Strap Pump")},
                    {QStringLiteral("color"), QStringLiteral("Silver")},
                    {QStringLiteral("brand"), QStringLiteral("Pradize")},
                    {QStringLiteral("categories"), QJsonArray{QStringLiteral("Heels")}},
                    {QStringLiteral("localImage"), spyImgPath}
                }
            }, genWork.path());

            // Seed 3 approved inspirations for wfAccId:
            //   1) winRefId:   1 published post that performed well (2,400 views) -> privileged from 1st win!
            //   2) flopRefId:  2 published posts that flopped (75 & 85 views)     -> retired after 2 low performers!
            //   3) freshRefId: 0 published posts                                  -> fresh exploration pool
            const QString winRefId = wfStore.addReference(
                wfAccId, QStringLiteral("https://www.tiktok.com/@inspo/video/101"),
                QStringLiteral("Winning Runway Source"), spyImgPath);
            const QString flopRefId = wfStore.addReference(
                wfAccId, QStringLiteral("https://www.tiktok.com/@inspo/video/102"),
                QStringLiteral("Two-Flop Weak Source"), spyImgPath);
            const QString freshRefId = wfStore.addReference(
                wfAccId, QStringLiteral("https://www.tiktok.com/@inspo/video/103"),
                QStringLiteral("Fresh Untested Source"), spyImgPath);
            requireResearch(!winRefId.isEmpty() && !flopRefId.isEmpty() && !freshRefId.isEmpty()
                                && wfStore.review({winRefId, flopRefId, freshRefId},
                                                  QStringLiteral("approved"),
                                                  QStringLiteral("Approved for workflow test")),
                            "seed 3 approved references (1st-win, 2-flop, fresh) in wfStore");

            const auto seedHistoricalRefPost = [&](const QString &refId,
                                                   const QString &postUrl,
                                                   qint64 views,
                                                   qint64 likes,
                                                   qint64 shares,
                                                   bool attachSnapshotNow,
                                                   QUuid *outVidId = nullptr) {
                const QUuid vid = pubVideos.addVideo({}, {}, QStringLiteral("auto_batch"), pubProjects.projectId(0));
                const auto *vRec = pubVideos.recordFromId(vid);
                requireResearch(vRec != nullptr, "create historical ref video record");
                const QDir gDir = pubProjects.generationDir(0, vRec->shortCode, true);
                writeResearch(gDir.filePath("creative_dna.json"), QJsonDocument(QJsonObject{
                    {QStringLiteral("referenceId"), refId},
                    {QStringLiteral("referenceUrl"), QStringLiteral("https://www.tiktok.com/@inspo/video/") + refId},
                    {QStringLiteral("strategy"), QStringLiteral("reference_swap")},
                    {QStringLiteral("format"), QStringLiteral("video")}
                }).toJson());
                pubVideos.setPublished(vid, true);
                pubVideos.addPublication(vid, QStringLiteral("tiktok"), postUrl,
                                         QDateTime::currentDateTimeUtc(), 1200);
                if (attachSnapshotNow)
                {
                    AbstractSocialAccount::VideoStatistics st;
                    st.views = views;
                    st.likes = likes;
                    st.comments = 4;
                    st.shares = shares;
                    pubVideos.addSnapshot(vid, postUrl, AbstractSocialAccount::StatSource::PublicView, st);
                }
                if (outVidId)
                {
                    *outVidId = vid;
                }
            };

            seedHistoricalRefPost(winRefId, QStringLiteral("https://www.tiktok.com/@pradize.high.heel/video/5001"),
                                  2400, 190, 28, true);
            seedHistoricalRefPost(flopRefId, QStringLiteral("https://www.tiktok.com/@pradize.high.heel/video/5002"),
                                  75, 2, 0, true);
            QUuid pendingFlopVidId;
            const QString pendingFlopUrl = QStringLiteral("https://www.tiktok.com/@pradize.high.heel#QS-PENDING");
            seedHistoricalRefPost(flopRefId, pendingFlopUrl, 85, 3, 0, false, &pendingFlopVidId);

            PaneResearch wfResearch(&pubAccounts, &wfStore, {&swapCli});
            PaneWorkflow wfPane(&pubAccounts, &wfStore, &wfResearch, &liveGenPane, &panePub,
                                &pubVideos, {&swapCli}, &gHooks, nullptr, &gHashtags);
            wfPane.resize(1200, 780);
            wfPane.show();
            QApplication::processEvents();

            // Verify Workflow detects unmeasured newly-published posts (#QS-PENDING without snapshots),
            // and clears needsStatsSyncBeforeGeneration() once latest snapshots are recorded!
            auto *wfBtnSyncStats = wfPane.findChild<QPushButton *>("buttonWorkflowSyncStats");
            auto *wfChkAutoSync = wfPane.findChild<QCheckBox *>("checkWorkflowAutoSyncStats");
            auto *wfInspoInfo = wfPane.findChild<QLabel *>("labelWorkflowInspirationsInfo");
            requireResearch(wfBtnSyncStats && wfChkAutoSync && wfChkAutoSync->isChecked()
                                && wfInspoInfo && wfPane.needsStatsSyncBeforeGeneration(),
                            "PaneWorkflow exposes Sync Stats controls and detects unmeasured published posts needing sync");

            {
                AbstractSocialAccount::VideoStatistics flop2Stats;
                flop2Stats.views = 85;
                flop2Stats.likes = 3;
                flop2Stats.comments = 0;
                flop2Stats.shares = 0;
                pubVideos.addSnapshot(pendingFlopVidId, pendingFlopUrl,
                                      AbstractSocialAccount::StatSource::PublicView, flop2Stats);
            }
            wfPane.refreshStep1Products();
            QApplication::processEvents();
            requireResearch(!wfPane.needsStatsSyncBeforeGeneration(),
                            "needsStatsSyncBeforeGeneration becomes false once all published posts have fresh (<30m) stats");

            const auto wfRefExp = SocialStatistics::summarizeReferenceExperiment(&pubVideos);
            bool verifiedFirstWinPrivileged = false;
            bool verifiedTwoFlopsRetired = false;
            for (const auto &rp : wfRefExp.references)
            {
                if (rp.referenceId == winRefId)
                {
                    verifiedFirstWinPrivileged = (rp.publishedCount == 1 && rp.successCount == 1
                                                  && rp.isWinner && !rp.isRetired);
                }
                else if (rp.referenceId == flopRefId)
                {
                    verifiedTwoFlopsRetired = (rp.publishedCount == 2 && rp.lowPerformerCount == 2
                                               && rp.isRetired && !rp.isWinner);
                }
            }
            requireResearch(verifiedFirstWinPrivileged && verifiedTwoFlopsRetired
                                && wfInspoInfo->text().contains("proven winner(s) privileged")
                                && wfInspoInfo->text().contains("retired"),
                            "SocialStatistics privileges source from 1st win, retires source starting at 2 low performers, and displays counts in Step 1B");

            requireResearch(wfPane.currentStep() == 0, "PaneWorkflow opens on Step 1 (Pick Products)");
            auto *wfProdList = wfPane.findChild<QListWidget *>("listWorkflowProducts");
            auto *wfCheckAll = wfPane.findChild<QPushButton *>("buttonWorkflowCheckAllProducts");
            auto *wfSpinPerProd = wfPane.findChild<QSpinBox *>("spinWorkflowGenPerProduct");
            auto *wfComboPromptCli = wfPane.findChild<QComboBox *>("comboWorkflowPromptCli");
            auto *wfComboImageCli = wfPane.findChild<QComboBox *>("comboWorkflowImageCli");
            auto *wfChkVideo = wfPane.findChild<QCheckBox *>("checkWorkflowFormatVideo");
            auto *wfChkCarousel = wfPane.findChild<QCheckBox *>("checkWorkflowFormatCarousel");
            auto *wfChkImage = wfPane.findChild<QCheckBox *>("checkWorkflowFormatImage");
            auto *wfBtnGen = wfPane.findChild<QPushButton *>("buttonWorkflowGenerate");
            auto *wfGenLog = wfPane.findChild<QPlainTextEdit *>("textWorkflowGenLog");
            requireResearch(wfProdList && wfProdList->count() == 2 && wfCheckAll && wfSpinPerProd
                                && wfComboPromptCli && wfComboPromptCli->currentText().contains("ProductSwap fixture CLI")
                                && wfComboImageCli && wfComboImageCli->currentText().contains("ProductSwap fixture CLI")
                                && wfChkVideo && wfChkCarousel && wfChkImage && wfBtnGen && wfGenLog,
                            "PaneWorkflow Step 1 displays eligible products, Content/Image CLI selectors, and batch controls");

            auto *wfComboVideoGen = wfPane.findChild<QComboBox *>("comboWorkflowVideoGenerator");
            wfCheckAll->click();
            wfSpinPerProd->setValue(1);
            wfChkVideo->setChecked(true);
            if (wfComboVideoGen)
            {
                const int vgIdx = wfComboVideoGen->findData(researchVideoBackend.getId());
                if (vgIdx >= 0)
                {
                    wfComboVideoGen->setCurrentIndex(vgIdx);
                }
            }
            wfChkCarousel->setChecked(false);
            wfChkImage->setChecked(false);
            QApplication::processEvents();
            requireResearch(wfBtnGen->isEnabled() && wfBtnGen->text().contains("Generate 2 Creative(s)"),
                            "Step 1 computes total creatives (2 products x 1 = 2) and enables Generate button");

            // Simulate Gemini hitting video quota on the 2nd product so we verify:
            //   1) Limit-reached banner appears in Workflow & Generation tab (1/2 saved, 1 remaining),
            //   2) User can move temporarily to Publish (Step 2) while keeping the remaining slot paused,
            //   3) Returning and clicking Resume completes the remaining 2nd creative seamlessly!
            researchVideoBackend.quotaOnCallNumber = researchVideoBackend.prompts.size() + 2;
            bool sawModalDuringWorkflowGen = false;
            wfBtnGen->click();
            QElapsedTimer wfGenWait;
            wfGenWait.start();
            while (liveGenPane.isAutoBatchRunning() && wfGenWait.elapsed() < 30000)
            {
                if (QApplication::activeModalWidget() != nullptr)
                {
                    sawModalDuringWorkflowGen = true;
                }
                QApplication::processEvents();
                QThread::msleep(20);
            }
            QApplication::processEvents();

            auto *wfLimitBanner = wfPane.findChild<QWidget *>("widgetWorkflowLimitBanner");
            auto *wfBtnPauseAndPub = wfPane.findChild<QPushButton *>("buttonWorkflowPauseAndPublish");
            auto *wfBtnStopAndPub = wfPane.findChild<QPushButton *>("buttonWorkflowStopAndPublish");
            auto *wfBtnResume = wfPane.findChild<QPushButton *>("buttonWorkflowResumeBatch");
            auto *wfBtnStep1 = wfPane.findChild<QPushButton *>("buttonWorkflowStep1");
            auto *genPausedBanner = liveGenPane.findChild<QWidget *>("widgetGenerationPausedBanner");
            requireResearch(!sawModalDuringWorkflowGen
                                && wfPane.currentStep() == 0
                                && liveGenPane.hasPausedAutoBatch()
                                && liveGenPane.pausedAutoBatchCompleted() == 1
                                && liveGenPane.pausedAutoBatchTotal() == 2
                                && liveGenPane.pausedAutoBatchRemaining() == 1
                                && QFileInfo::exists(genWork.filePath("paused_auto_batch.json"))
                                && wfLimitBanner && wfLimitBanner->isVisible()
                                && wfBtnPauseAndPub && wfBtnStopAndPub && wfBtnResume && wfBtnStep1
                                && genPausedBanner && !genPausedBanner->isHidden(),
                            "Workflow pauses cleanly on Gemini video quota limit, persists paused_auto_batch.json, and shows Limit Reached controls in both Workflow and Generation tabs");

            // User chooses to move temporarily to Publish (Step 2) while keeping remaining generation paused!
            wfBtnPauseAndPub->click();
            QApplication::processEvents();
            requireResearch(wfPane.currentStep() == 1
                                && liveGenPane.hasPausedAutoBatch()
                                && wfBtnStep1->text().contains("Limit"),
                            "Publish Now & Resume Later moves temporarily to Step 2 while keeping remaining batch paused and flagged on Step 1");

            // User comes back to Step 1 after quota resets and resumes the remaining creative!
            wfPane.setCurrentStep(0);
            QApplication::processEvents();
            researchVideoBackend.quotaOnCallNumber = 0;
            wfBtnResume->click();
            wfGenWait.restart();
            while ((liveGenPane.isAutoBatchRunning() || wfPane.currentStep() == 0) && wfGenWait.elapsed() < 30000)
            {
                if (QApplication::activeModalWidget() != nullptr)
                {
                    sawModalDuringWorkflowGen = true;
                }
                QApplication::processEvents();
                QThread::msleep(20);
            }
            QApplication::processEvents();
            requireResearch(!sawModalDuringWorkflowGen
                                && wfPane.currentStep() == 1
                                && !liveGenPane.hasPausedAutoBatch()
                                && !QFileInfo::exists(genWork.filePath("paused_auto_batch.json"))
                                && !wfLimitBanner->isVisible()
                                && wfGenLog->toPlainText().contains("Saved")
                                && wfGenLog->toPlainText().contains("1 proven winner(s) privileged, 1 retired"),
                            "Resuming paused batch finishes remaining creative (2/2), clears checkpoint, privileges 1st-win source, and auto-advances to Step 2");

            // Verify newly generated wf-prod-1 and wf-prod-2 creatives both privileged winRefId (0% flopRefId)
            int winRefUsedCount = 0;
            int flopRefUsedCount = 0;
            for (int r = 0; r < pubVideos.rowCount(); ++r)
            {
                const auto *rec = pubVideos.recordAt(r);
                if (!rec || rec->published)
                {
                    continue;
                }
                const int pRow = pubProjects.rowOfId(rec->projectId);
                if (pRow < 0)
                {
                    continue;
                }
                const QDir gDir = pubProjects.generationDir(pRow, rec->shortCode, false);
                const QString dnaPath = gDir.filePath("creative_dna.json");
                if (!QFileInfo::exists(dnaPath))
                {
                    continue;
                }
                const QJsonObject dna = QJsonDocument::fromJson(readResearch(dnaPath)).object();
                const QString pid = dna.value("productId").toString();
                if (pid == QLatin1String("wf-prod-1") || pid == QLatin1String("wf-prod-2"))
                {
                    const QString usedRef = dna.value("referenceId").toString();
                    if (usedRef == winRefId) ++winRefUsedCount;
                    if (usedRef == flopRefId) ++flopRefUsedCount;
                }
            }
            requireResearch(winRefUsedCount == 2 && flopRefUsedCount == 0,
                            "Auto-Batch exploit slots privilege 1st-win source (2/2) and never reuse 2-flop retired source (0/2)");

            // Step 2: Review Generated Content inline (1 VIDEO + 1 SLIDESHOW)
            auto *wfCandTable = wfPane.findChild<QTableWidget *>("tableReviewCandidates");
            auto *wfBtnAiSelect = wfPane.findChild<QPushButton *>("buttonWorkflowAiSelect");
            auto *wfBtnToStep3 = wfPane.findChild<QPushButton *>("buttonWorkflowToStep3");
            requireResearch(wfCandTable && wfCandTable->rowCount() >= 2 && wfBtnAiSelect && wfBtnToStep3,
                            "Step 2 embeds WidgetCreativeReview with newly generated creatives");

            wfBtnAiSelect->click();
            QElapsedTimer wfAiWait;
            wfAiWait.start();
            while (!wfBtnAiSelect->isEnabled() && wfAiWait.elapsed() < 10000)
            {
                QApplication::processEvents();
                QThread::msleep(20);
            }
            const int rowsBeforeDelete = wfCandTable->rowCount();
            int nonVideoDelRow = rowsBeforeDelete - 1;
            for (int r = 0; r < rowsBeforeDelete; ++r)
            {
                if (wfCandTable->item(r, 2) && !wfCandTable->item(r, 2)->text().contains("VIDEO"))
                {
                    nonVideoDelRow = r;
                    break;
                }
            }
            const QString codeToDelete = wfCandTable->item(nonVideoDelRow, 2)->text().section(' ', 0, 0);
            auto *wfInspectorDelBtn = wfPane.findChild<QPushButton *>("buttonDeleteInspectorCreative");
            auto *wfRowDelBtn = wfPane.findChild<QPushButton *>(QStringLiteral("buttonDeleteRow_") + codeToDelete);
            requireResearch(wfInspectorDelBtn && wfInspectorDelBtn->isVisible()
                                && wfRowDelBtn && !wfCandTable->isColumnHidden(4),
                            "Step 2 exposes visible Delete buttons both under the right preview inspector and on each candidate row");
            QTimer::singleShot(15, []() {
                if (auto *msgBox = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                {
                    if (auto *yesBtn = msgBox->button(QMessageBox::Yes))
                    {
                        yesBtn->click();
                    }
                    else
                    {
                        msgBox->accept();
                    }
                }
            });
            wfRowDelBtn->click();
            QApplication::processEvents();
            requireResearch(wfCandTable->rowCount() == rowsBeforeDelete - 1
                                && pubVideos.recordFromShortCode(codeToDelete) == nullptr,
                            "clicking per-row Delete button removes unwanted unpublished creative from disk, TableVideos, and review table");

            // Start playing the VIDEO candidate in Step 2, then move to Step 3 and verify video/audio stops
            // and stays stopped even when Step 3 schedule settings refresh WidgetCreativeReview.
            auto *wfBtnPlayVid = wfPane.findChild<QPushButton *>("buttonPlayPauseCandidateVideo");
            auto *wfPrevStack = wfPane.findChild<QStackedWidget *>("stackedReviewPreview");
            auto *wfVidWidget = wfPane.findChild<QVideoWidget *>("videoReviewPlayerWidget");
            auto *wfVidPlayer = wfPane.findChild<QMediaPlayer *>("playerReviewVideo");
            requireResearch(wfBtnPlayVid && wfPrevStack && wfVidWidget && wfVidPlayer,
                            "Step 2 exposes inline video player controls");
            int wfVideoRow = -1;
            for (int r = 0; r < wfCandTable->rowCount(); ++r)
            {
                if (wfCandTable->item(r, 2) && wfCandTable->item(r, 2)->text().contains("VIDEO"))
                {
                    wfVideoRow = r;
                    break;
                }
            }
            requireResearch(wfVideoRow >= 0, "Step 2 contains newly generated VIDEO creative");
            wfCandTable->item(wfVideoRow, 0)->setCheckState(Qt::Checked);
            wfCandTable->selectRow(wfVideoRow);
            QApplication::processEvents();
            wfBtnPlayVid->click();
            QApplication::processEvents();
            requireResearch(wfPrevStack->currentWidget() == wfVidWidget && wfVidPlayer->source().isLocalFile(),
                            "Step 2 starts inline video playback when Play Video is clicked");

            requireResearch(wfBtnToStep3->isEnabled(), "Step 2 enables Continue to Step 3 with checked creative");
            wfBtnToStep3->click();
            QApplication::processEvents();
            requireResearch(wfPane.currentStep() == 2
                                && wfVidPlayer->playbackState() != QMediaPlayer::PlayingState
                                && wfVidPlayer->source().isEmpty()
                                && wfPrevStack->currentWidget() != wfVidWidget,
                            "Step 2 advances cleanly to Step 3 and immediately stops Step 2 video/audio playback");

            // Step 3: Verify live spacing radio buttons + execute Dry-Run Auto-Publish job
            auto *wfRadioFixed = wfPane.findChild<QRadioButton *>("radioWorkflowSpacingFixed");
            auto *wfSpinFixed = wfPane.findChild<QSpinBox *>("spinWorkflowFixedMinutes");
            auto *wfChkDryRun = wfPane.findChild<QCheckBox *>("checkWorkflowDryRun");
            auto *wfQueueTable = wfPane.findChild<QTableWidget *>("tableWorkflowPublishQueue");
            auto *wfPubLogs = wfPane.findChild<QPlainTextEdit *>("textWorkflowPublishLogs");
            auto *wfBtnStartPub = wfPane.findChild<QPushButton *>("buttonWorkflowStartPublish");
            requireResearch(wfRadioFixed && wfSpinFixed && wfChkDryRun && wfQueueTable && wfPubLogs && wfBtnStartPub,
                            "Step 3 exposes spacing radio buttons, queue preview table, logs, and Start Auto-Publish button");

            wfRadioFixed->setChecked(true);
            wfSpinFixed->setValue(45);
            wfChkDryRun->setChecked(true);
            wfPane.setUploadUrlOverride(QStringLiteral("file://") + studioHtmlPath);
            QApplication::processEvents();
            requireResearch(wfQueueTable->rowCount() >= 1
                                && wfBtnStartPub->isEnabled()
                                && wfVidPlayer->playbackState() != QMediaPlayer::PlayingState
                                && wfVidPlayer->source().isEmpty(),
                            "Step 3 previews scheduled queue and never restarts hidden Step 2 video playback");

            wfBtnStartPub->click();
            QElapsedTimer wfPubWait;
            wfPubWait.start();
            while (panePub.isJobRunning() && wfPubWait.elapsed() < 30000)
            {
                QApplication::processEvents();
                QThread::msleep(25);
            }
            requireResearch(!panePub.isJobRunning()
                                && wfPubLogs->toPlainText().contains("[JOB-START]")
                                && wfPubLogs->toPlainText().contains("[JOB-DONE]")
                                && wfQueueTable->item(0, 6)
                                && wfQueueTable->item(0, 6)->text().contains("Dry-run OK"),
                            "Step 3 executes planned Auto-Publish job on PanePublishing and streams live status & logs");

            // Verify top-bar "Clear Gemini Chats" button + dialog (shows each cached chat & creation date,
            // supports deleting selected chat(s) via "Delete" and clearing all via "Delete all").
            {
                const QString testChatCachePath = genWork.filePath(QStringLiteral("test_gemini_chat_cache.json"));
                qputenv("QS_GEMINI_CHAT_CACHE_PATH", testChatCachePath.toUtf8());
                wfPane.setGeminiVideoGeneratorOverride(&researchVideoBackend);
                researchVideoBackend.deletedConversations.clear();

                const QList<PaneWorkflow::GeminiCachedChat> seededChats{
                    {QStringLiteral("chat_oldest_001"),
                     QStringLiteral("https://gemini.google.com/u/4/app/chat_oldest_001"),
                     QStringLiteral("2026-11-23T08:15:00Z"),
                     QStringLiteral("active")},
                    {QStringLiteral("chat_middle_002"),
                     QStringLiteral("https://gemini.google.com/u/4/app/chat_middle_002"),
                     QStringLiteral("2026-11-24T12:30:00Z"),
                     QStringLiteral("delete_failed")},
                    {QStringLiteral("chat_newest_003"),
                     QStringLiteral("https://gemini.google.com/u/4/app/chat_newest_003?pageId=none&pli=1"),
                     QStringLiteral("2026-11-25T06:45:00Z"),
                     QStringLiteral("active")},
                };
                requireResearch(PaneWorkflow::saveGeminiChatCache(seededChats),
                                "seed isolated Gemini chat cache JSON");
                wfPane.refreshAll();
                QApplication::processEvents();

                auto *btnClearChats = wfPane.findChild<QPushButton *>("buttonWorkflowClearGeminiChats");
                requireResearch(btnClearChats && btnClearChats->isVisible()
                                    && btnClearChats->text().contains("Clear Gemini Chats (3)"),
                                "PaneWorkflow top bar exposes Clear Gemini Chats button with cached chat count");

                bool verifiedClearChatsDialog = false;
                QTimer clearChatsDriver;
                clearChatsDriver.setInterval(25);
                QObject::connect(&clearChatsDriver, &QTimer::timeout, [&]() {
                    auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dlg || dlg->objectName() != QLatin1String("dialogClearGeminiChats"))
                    {
                        return;
                    }
                    clearChatsDriver.stop();

                    auto *tbl = dlg->findChild<QTableWidget *>("tableGeminiCachedChats");
                    auto *btnDelSel = dlg->findChild<QPushButton *>("buttonDeleteSelectedGeminiChats");
                    auto *btnDelAll = dlg->findChild<QPushButton *>("buttonDeleteAllGeminiChats");
                    auto *btnClose = dlg->findChild<QPushButton *>("buttonCloseGeminiChats");
                    if (tbl && btnDelSel && btnDelAll && btnClose && tbl->rowCount() == 3)
                    {
                        const bool initialOk =
                            !btnDelSel->isEnabled()
                            && btnDelAll->isEnabled()
                            && tbl->item(0, 1)->text().contains("chat_newest_003")
                            && !tbl->item(0, 0)->text().isEmpty()
                            && tbl->item(2, 1)->text().contains("chat_oldest_001");

                        // 1) Select row 0 (newest) and click "Delete"
                        tbl->selectRow(0);
                        QApplication::processEvents();
                        const bool selEnabled = btnDelSel->isEnabled();
                        btnDelSel->click();
                        QElapsedTimer waitDel;
                        waitDel.start();
                        while (tbl->rowCount() == 3 && waitDel.elapsed() < 5000)
                        {
                            QApplication::processEvents();
                            QThread::msleep(15);
                        }
                        const bool afterSingleDelOk =
                            tbl->rowCount() == 2
                            && researchVideoBackend.deletedConversations.size() == 1
                            && researchVideoBackend.deletedConversations.first().contains("chat_newest_003")
                            && PaneWorkflow::loadGeminiChatCache().size() == 2;

                        // 2) Click "Delete all" to remove the remaining 2 cached chats
                        btnDelAll->click();
                        waitDel.restart();
                        while (tbl->rowCount() > 0 && waitDel.elapsed() < 5000)
                        {
                            QApplication::processEvents();
                            QThread::msleep(15);
                        }
                        const bool afterDeleteAllOk =
                            tbl->rowCount() == 0
                            && !btnDelAll->isEnabled()
                            && researchVideoBackend.deletedConversations.size() == 3
                            && PaneWorkflow::loadGeminiChatCache().isEmpty();

                        verifiedClearChatsDialog = initialOk && selEnabled && afterSingleDelOk && afterDeleteAllOk;
                    }
                    if (btnClose)
                    {
                        btnClose->click();
                    }
                    else
                    {
                        dlg->accept();
                    }
                });
                clearChatsDriver.start();
                btnClearChats->click();
                clearChatsDriver.stop();
                QApplication::processEvents();

                requireResearch(verifiedClearChatsDialog
                                    && btnClearChats->text() == QLatin1String("Clear Gemini Chats")
                                    && PaneWorkflow::loadGeminiChatCache().isEmpty(),
                                "Clear Gemini Chats dialog lists cached chats with creation dates and supports Delete selection and Delete all");
                wfPane.setGeminiVideoGeneratorOverride(nullptr);
                qunsetenv("QS_GEMINI_CHAT_CACHE_PATH");
            }

            wfPane.close();
        }

        liveGenPane.close();
        panePub.close();
        qunsetenv("QS_PUBLISH_HEADLESS");
        qunsetenv("QS_PUBLISH_LOG_DIR");

        QSettings().clear();
        WorkingDirectoryManager::instance()->setWorkingDir(callerWorkingDir);
    }

    // Corrupt/unsupported data must never be silently overwritten.
    root.mkpath("corrupt/research");
    const auto corrupt = root.filePath("corrupt/research/research.json");
    writeResearch(corrupt, "{broken}");
    ResearchStore damaged(root.filePath("corrupt"));
    requireResearch(!damaged.setProfile(accountId, profile) && readResearch(corrupt) == "{broken}", "corrupt research is protected");

    const QString realSource = qEnvironmentVariable("QS_TEST_AMAZON_SOURCE");
    if (!realSource.isEmpty())
    {
        const auto actual = importer->read(realSource);
        requireResearch(actual.error.isEmpty() && !actual.products.isEmpty(), "read actual Amazon directory without modifying it");
        for (const auto &v : actual.products)
        {
            const auto p = v.toObject();
            const QString img = p.value("localImage").toString();
            const QSize sz = QImageReader(img).size();
            requireResearch(!p.value("sizingFolder").toString().isEmpty()
                                && !img.contains("/stores/thumbs/")
                                && sz.isValid() && qMax(sz.width(), sz.height()) >= 200,
                            "every imported real product has A+ content and a high-resolution image");
        }
        std::cout << "Actual A+ catalog: " << actual.products.size() << " high-res groups, "
                  << actual.archivedRecords << " archived, "
                  << actual.skippedWithoutAPlus << " non-A+ skipped.\n";
    }
    std::cout << "Research checks passed: migration, import, restrictions, review, provenance, stale writers, and visual card actions.\n";
}
