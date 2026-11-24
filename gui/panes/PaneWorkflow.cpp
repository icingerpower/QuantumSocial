// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PaneWorkflow.h"

#include "../WidgetCreativeReview.h"
#include "AbstractCli.h"
#include "PaneGeneration.h"
#include "PaneResearch.h"
#include "model/FavoriteHooks.h"
#include "model/TableAccounts.h"
#include "model/TableProjects.h"
#include "model/accounts/AbstractSocialAccount.h"
#include "model/research/ResearchStore.h"
#include "model/videogen/AbstractVideoGenerator.h"
#include "model/videos/SocialStatistics.h"
#include "model/videos/TableVideos.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTime>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QPixmap loadFastProductThumb(const QString &imgPath, int edge)
{
    if (imgPath.isEmpty())
    {
        return {};
    }
    QImageReader reader(imgPath);
    reader.setAutoTransform(true);
    const QSize orig = reader.size();
    if (orig.isValid() && (orig.width() > edge * 2 || orig.height() > edge * 2))
    {
        reader.setScaledSize(orig.scaled(edge, edge, Qt::KeepAspectRatio));
    }
    const QImage img = reader.read();
    if (img.isNull())
    {
        return {};
    }
    return QPixmap::fromImage(img.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

QString jsonText(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString().trimmed();
}

} // namespace

PaneWorkflow::PaneWorkflow(TableAccounts *accounts,
                           ResearchStore *researchStore,
                           PaneResearch *paneResearch,
                           PaneGeneration *paneGeneration,
                           PanePublishing *panePublishing,
                           TableVideos *videos,
                           const QList<AbstractCli *> &availableClis,
                           FavoriteHooks *favoriteHooks,
                           QWidget *parent)
    : QWidget(parent)
    , m_accounts(accounts)
    , m_store(researchStore)
    , m_paneResearch(paneResearch)
    , m_paneGeneration(paneGeneration)
    , m_panePublishing(panePublishing)
    , m_videos(videos)
    , m_favoriteHooks(favoriteHooks)
    , m_clis(availableClis)
    , m_aiPlanCancelFlag(std::make_shared<bool>(false))
{
    setObjectName(QStringLiteral("paneWorkflow"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // Top persistent context + 3-step stepper header
    auto *headerCard = new QFrame(this);
    headerCard->setFrameShape(QFrame::StyledPanel);
    headerCard->setStyleSheet(QStringLiteral(
        "QFrame { background: rgba(30, 36, 50, 0.55); border: 1px solid rgba(128, 140, 165, 0.28); border-radius: 8px; }"));
    auto *headerLayout = new QVBoxLayout(headerCard);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(6);

    auto *topContextRow = new QHBoxLayout;
    auto *accLbl = new QLabel(tr("Account:"), headerCard);
    accLbl->setStyleSheet(QStringLiteral("border: none; background: transparent; font-weight: 600;"));
    topContextRow->addWidget(accLbl);

    m_accountCombo = new QComboBox(headerCard);
    m_accountCombo->setObjectName(QStringLiteral("workflowAccountCombo"));
    m_accountCombo->setMinimumContentsLength(24);
    topContextRow->addWidget(m_accountCombo);

    m_headerSummaryLabel = new QLabel(headerCard);
    m_headerSummaryLabel->setObjectName(QStringLiteral("labelWorkflowHeaderSummary"));
    m_headerSummaryLabel->setStyleSheet(QStringLiteral("border: none; background: transparent; color: #93c5fd;"));
    m_headerSummaryLabel->setWordWrap(true);
    topContextRow->addWidget(m_headerSummaryLabel, 1);

    auto *btnOpenResearch = new QPushButton(tr("Open Research…"), headerCard);
    btnOpenResearch->setObjectName(QStringLiteral("buttonWorkflowOpenResearch"));
    btnOpenResearch->setToolTip(tr("Switch to the Research tab to import products, edit account rules, or approve inspirations."));
    auto *btnOpenAccounts = new QPushButton(tr("Accounts…"), headerCard);
    btnOpenAccounts->setObjectName(QStringLiteral("buttonWorkflowOpenAccounts"));
    topContextRow->addWidget(btnOpenResearch);
    topContextRow->addWidget(btnOpenAccounts);
    headerLayout->addLayout(topContextRow);

    auto *stepBar = new QHBoxLayout;
    stepBar->setSpacing(8);

    const auto makeStepBtn = [headerCard](const QString &text, const QString &objName) {
        auto *btn = new QPushButton(text, headerCard);
        btn->setObjectName(objName);
        btn->setCheckable(true);
        btn->setMinimumHeight(34);
        return btn;
    };
    m_btnStep1 = makeStepBtn(tr("1. Pick Products"), QStringLiteral("buttonWorkflowStep1"));
    m_btnStep2 = makeStepBtn(tr("2. Review Generated Content"), QStringLiteral("buttonWorkflowStep2"));
    m_btnStep3 = makeStepBtn(tr("3. Start Auto-Publish Jobs"), QStringLiteral("buttonWorkflowStep3"));

    auto *arrow12 = new QLabel(QStringLiteral("→"), headerCard);
    arrow12->setStyleSheet(QStringLiteral("border: none; background: transparent; font-weight: bold; color: #9ca3af;"));
    auto *arrow23 = new QLabel(QStringLiteral("→"), headerCard);
    arrow23->setStyleSheet(QStringLiteral("border: none; background: transparent; font-weight: bold; color: #9ca3af;"));

    stepBar->addWidget(m_btnStep1, 1);
    stepBar->addWidget(arrow12);
    stepBar->addWidget(m_btnStep2, 1);
    stepBar->addWidget(arrow23);
    stepBar->addWidget(m_btnStep3, 1);
    headerLayout->addLayout(stepBar);

    root->addWidget(headerCard);

    // Stacked 3-step pages
    m_stepStack = new QStackedWidget(this);
    m_stepStack->setObjectName(QStringLiteral("stackedWorkflowSteps"));
    m_stepStack->addWidget(_buildStep1Page());
    m_stepStack->addWidget(_buildStep2Page());
    m_stepStack->addWidget(_buildStep3Page());
    root->addWidget(m_stepStack, 1);

    connect(btnOpenResearch, &QPushButton::clicked, this, &PaneWorkflow::openResearchTabRequested);
    connect(btnOpenAccounts, &QPushButton::clicked, this, &PaneWorkflow::openAccountsTabRequested);

    connect(m_btnStep1, &QPushButton::clicked, this, [this]() { setCurrentStep(0); });
    connect(m_btnStep2, &QPushButton::clicked, this, [this]() { setCurrentStep(1); });
    connect(m_btnStep3, &QPushButton::clicked, this, [this]() { setCurrentStep(2); });

    connect(m_accountCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updatingUi)
        {
            return;
        }
        _syncResearchAccountSelection();
        refreshStep1Products();
        _updateHeaderAndStepBadges();
    });

    if (m_accounts)
    {
        connect(m_accounts, &QAbstractItemModel::modelReset, this, &PaneWorkflow::refreshAll);
        connect(m_accounts, &QAbstractItemModel::rowsInserted, this, &PaneWorkflow::refreshAll);
        connect(m_accounts, &QAbstractItemModel::rowsRemoved, this, &PaneWorkflow::refreshAll);
        connect(m_accounts, &QAbstractItemModel::dataChanged, this, &PaneWorkflow::refreshAll);
    }
    if (m_store)
    {
        connect(m_store, &ResearchStore::changed, this, [this]() {
            refreshStep1Products();
            _updateHeaderAndStepBadges();
        });
    }
    if (m_videos)
    {
        connect(m_videos, &QAbstractItemModel::rowsInserted, this, [this]() {
            _updateHeaderAndStepBadges();
        });
        connect(m_videos, &QAbstractItemModel::rowsRemoved, this, [this]() {
            _updateHeaderAndStepBadges();
        });
        connect(m_videos, &QAbstractItemModel::dataChanged, this, [this]() {
            _updateHeaderAndStepBadges();
        });
    }
    if (m_paneGeneration)
    {
        connect(m_paneGeneration, &PaneGeneration::autoBatchStarted, this, [this](int total) {
            m_genProgressBox->setVisible(true);
            m_genProgressBar->setRange(0, qMax(1, total));
            m_genProgressBar->setValue(0);
            m_btnCancelGen->setEnabled(true);
            m_btnCancelGen->setVisible(true);
            m_btnGenerateAndNext->setEnabled(false);
        });
        connect(m_paneGeneration, &PaneGeneration::autoBatchProgress, this,
                [this](int completed, int total, const QString &msg) {
            m_genProgressBox->setVisible(true);
            if (total > 0)
            {
                m_genProgressBar->setRange(0, total);
                m_genProgressBar->setValue(completed);
            }
            const QString compact = msg.simplified();
            m_genStatusLabel->setText(compact.size() > 160
                ? (compact.left(159) + QStringLiteral("…"))
                : compact);
            m_genLogConsole->appendPlainText(
                QTime::currentTime().toString(QStringLiteral("HH:mm:ss  ")) + msg);
        });
        connect(m_paneGeneration, &PaneGeneration::autoBatchFinished, this,
                [this](int completed, int total, bool cancelled, const QString &errorMsg) {
            m_btnCancelGen->setEnabled(false);
            m_btnCancelGen->setVisible(false);
            m_genProgressBar->setRange(0, qMax(1, total));
            m_genProgressBar->setValue(completed);
            if (!errorMsg.isEmpty())
            {
                m_genStatusLabel->setText(tr("Paused (%1/%2 saved): %3").arg(completed).arg(total).arg(errorMsg));
            }
            else if (cancelled)
            {
                m_genStatusLabel->setText(tr("Cancelled (%1/%2 creative(s) saved).").arg(completed).arg(total));
            }
            else
            {
                m_genStatusLabel->setText(tr("Finished: %1/%2 creative(s) generated!").arg(completed).arg(total));
            }
            _updateStep1SummaryAndButtons();
            _updateHeaderAndStepBadges();

            const bool shouldAdvance = m_triggeredBatchFromWorkflow && completed > 0;
            m_triggeredBatchFromWorkflow = false;
            if (shouldAdvance)
            {
                refreshStep2Candidates(false);
                setCurrentStep(1);
            }
        });
    }
    if (m_panePublishing)
    {
        connect(m_panePublishing, &PanePublishing::logLineAppended, this, [this](const QString &line) {
            if (m_publishLogConsole)
            {
                m_publishLogConsole->appendPlainText(line);
            }
        });
        connect(m_panePublishing, &PanePublishing::queueUpdated, this, [this]() {
            _refreshStep3QueueTable();
            _updateStep3Buttons();
            _updateHeaderAndStepBadges();
        });
        connect(m_panePublishing, &PanePublishing::statusTextChanged, this, [this](const QString &txt) {
            if (m_publishStatusLabel && m_panePublishing->isJobRunning())
            {
                m_publishStatusLabel->setText(txt);
            }
        });
        connect(m_panePublishing, &PanePublishing::jobStateChanged, this, [this](bool) {
            if (m_publishStatusLabel)
            {
                m_publishStatusLabel->setText(m_panePublishing->statusText());
            }
            _refreshStep3QueueTable();
            _updateStep3Buttons();
            _updateHeaderAndStepBadges();
        });
    }

    refreshAll();
    setCurrentStep(0);
}

PaneWorkflow::~PaneWorkflow()
{
    if (m_aiPlanCancelFlag)
    {
        *m_aiPlanCancelFlag = true;
    }
}

QWidget *PaneWorkflow::_buildStep1Page()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageWorkflowStep1"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Horizontal, page);
    pageLayout->addWidget(splitter, 1);

    // Left: Product selection + live product photo preview
    auto *leftBox = new QGroupBox(tr("Step 1A — Pick Eligible Products to Generate For"), splitter);
    auto *leftLayout = new QVBoxLayout(leftBox);

    auto *filterRow = new QHBoxLayout;
    m_prodSearchEdit = new QLineEdit(leftBox);
    m_prodSearchEdit->setObjectName(QStringLiteral("editWorkflowProductSearch"));
    m_prodSearchEdit->setPlaceholderText(tr("Filter products by color, title, ASIN, or category…"));
    m_prodSearchEdit->setClearButtonEnabled(true);
    filterRow->addWidget(m_prodSearchEdit, 1);

    m_chkOnlySelectedProducts = new QCheckBox(tr("Only checked"), leftBox);
    m_chkOnlySelectedProducts->setObjectName(QStringLiteral("checkWorkflowOnlySelected"));
    filterRow->addWidget(m_chkOnlySelectedProducts);

    m_btnCheckAllProducts = new QPushButton(tr("Check all"), leftBox);
    m_btnCheckAllProducts->setObjectName(QStringLiteral("buttonWorkflowCheckAllProducts"));
    m_btnClearProducts = new QPushButton(tr("Clear"), leftBox);
    m_btnClearProducts->setObjectName(QStringLiteral("buttonWorkflowClearProducts"));
    filterRow->addWidget(m_btnCheckAllProducts);
    filterRow->addWidget(m_btnClearProducts);
    leftLayout->addLayout(filterRow);

    auto *listAndPreviewRow = new QHBoxLayout;
    m_productList = new QListWidget(leftBox);
    m_productList->setObjectName(QStringLiteral("listWorkflowProducts"));
    m_productList->setIconSize(QSize(48, 48));
    listAndPreviewRow->addWidget(m_productList, 1);

    auto *prodPreviewBox = new QFrame(leftBox);
    prodPreviewBox->setFrameShape(QFrame::StyledPanel);
    prodPreviewBox->setFixedWidth(190);
    auto *prodPreviewLayout = new QVBoxLayout(prodPreviewBox);
    prodPreviewLayout->setContentsMargins(6, 6, 6, 6);
    m_prodPreviewImg = new QLabel(prodPreviewBox);
    m_prodPreviewImg->setObjectName(QStringLiteral("labelWorkflowProductPreview"));
    m_prodPreviewImg->setAlignment(Qt::AlignCenter);
    m_prodPreviewImg->setFixedSize(174, 174);
    m_prodPreviewImg->setStyleSheet(QStringLiteral(
        "QLabel { background: #141821; border: 1px solid #2b3245; border-radius: 8px; }"));
    m_prodPreviewCaption = new QLabel(prodPreviewBox);
    m_prodPreviewCaption->setWordWrap(true);
    m_prodPreviewCaption->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    m_prodPreviewCaption->setStyleSheet(QStringLiteral("font-size: 11px;"));
    prodPreviewLayout->addWidget(m_prodPreviewImg);
    prodPreviewLayout->addWidget(m_prodPreviewCaption, 1);
    listAndPreviewRow->addWidget(prodPreviewBox);

    leftLayout->addLayout(listAndPreviewRow, 1);

    m_selectedProductsLabel = new QLabel(leftBox);
    m_selectedProductsLabel->setObjectName(QStringLiteral("labelWorkflowSelectedProducts"));
    leftLayout->addWidget(m_selectedProductsLabel);
    splitter->addWidget(leftBox);

    // Right: Creative batch options + inline progress
    auto *rightBox = new QGroupBox(tr("Step 1B — Batch Formats && Inline Generation"), splitter);
    auto *rightLayout = new QVBoxLayout(rightBox);

    m_inspirationsInfoLabel = new QLabel(rightBox);
    m_inspirationsInfoLabel->setObjectName(QStringLiteral("labelWorkflowInspirationsInfo"));
    m_inspirationsInfoLabel->setWordWrap(true);
    m_inspirationsInfoLabel->setStyleSheet(QStringLiteral(
        "background: rgba(59, 130, 246, 0.12); border: 1px solid rgba(59, 130, 246, 0.30); "
        "border-radius: 6px; padding: 6px 8px;"));
    rightLayout->addWidget(m_inspirationsInfoLabel);

    QSettings uiSettings = m_store
        ? QSettings(m_store->directory().filePath(QStringLiteral("ui.ini")), QSettings::IniFormat)
        : QSettings();
    QSettings appSettings;

    auto *form = new QFormLayout;
    m_spinGenPerProduct = new QSpinBox(rightBox);
    m_spinGenPerProduct->setObjectName(QStringLiteral("spinWorkflowGenPerProduct"));
    m_spinGenPerProduct->setRange(1, 50);
    m_spinGenPerProduct->setValue(uiSettings.value(QStringLiteral("autoBatch/generationsPerProduct"), 6).toInt());
    form->addRow(tr("Generations per product:"), m_spinGenPerProduct);

    m_comboPromptCli = new QComboBox(rightBox);
    m_comboPromptCli->setObjectName(QStringLiteral("comboWorkflowPromptCli"));
    m_comboPromptCli->setToolTip(
        tr("AI CLI used to plan creative prompts, write hooks & captions, and run quality checks."));
    int defaultPromptIdx = 0;
    bool foundTextOnlyFallback = false;
    for (AbstractCli *cli : m_clis)
    {
        if (!cli) continue;
        if (!foundTextOnlyFallback && !cli->canGenImages())
        {
            defaultPromptIdx = m_comboPromptCli->count();
            foundTextOnlyFallback = true;
        }
        m_comboPromptCli->addItem(cli->getName(), QVariant::fromValue(cli));
    }
    if (m_comboPromptCli->count() == 0)
    {
        m_comboPromptCli->addItem(tr("(No AI CLI detected)"), QVariant{});
        m_comboPromptCli->setEnabled(false);
    }
    else
    {
        const QString savedPromptCli = uiSettings.value(
            QStringLiteral("autoBatch/promptCli"),
            appSettings.value(QStringLiteral("generation/promptCli")).toString()).toString();
        const int pIdx = m_comboPromptCli->findText(savedPromptCli);
        m_comboPromptCli->setCurrentIndex(pIdx >= 0 ? pIdx : defaultPromptIdx);
    }
    form->addRow(tr("Content / prompt CLI:"), m_comboPromptCli);

    m_comboImageCli = new QComboBox(rightBox);
    m_comboImageCli->setObjectName(QStringLiteral("comboWorkflowImageCli"));
    m_comboImageCli->setToolTip(
        tr("Image-capable AI CLI used for product swaps, video starting frames, 4-slide carousels, and single images."));
    for (AbstractCli *cli : m_clis)
    {
        if (cli && cli->canGenImages())
        {
            m_comboImageCli->addItem(cli->getName(), QVariant::fromValue(cli));
        }
    }
    if (m_comboImageCli->count() == 0)
    {
        m_comboImageCli->addItem(tr("(No image-capable CLI detected)"), QVariant{});
        m_comboImageCli->setEnabled(false);
    }
    else
    {
        const QString savedImageCli = uiSettings.value(
            QStringLiteral("autoBatch/imageCli"),
            appSettings.value(QStringLiteral("generation/imageCli")).toString()).toString();
        const int iIdx = m_comboImageCli->findText(savedImageCli);
        if (iIdx >= 0)
        {
            m_comboImageCli->setCurrentIndex(iIdx);
        }
    }
    form->addRow(tr("Image generation CLI:"), m_comboImageCli);

    m_comboAspectFormat = new QComboBox(rightBox);
    m_comboAspectFormat->setObjectName(QStringLiteral("comboWorkflowAspectFormat"));
    m_comboAspectFormat->addItems({
        QStringLiteral("vertical (9:16)"),
        QStringLiteral("square (1:1)"),
        QStringLiteral("horizontal (16:9)")
    });
    const QString savedFmt = uiSettings.value(QStringLiteral("autoBatch/videoFormat"),
                                              QStringLiteral("vertical (9:16)")).toString();
    const int fmtIdx = m_comboAspectFormat->findText(savedFmt);
    if (fmtIdx >= 0) m_comboAspectFormat->setCurrentIndex(fmtIdx);
    form->addRow(tr("Aspect ratio:"), m_comboAspectFormat);

    auto *videoRow = new QHBoxLayout;
    m_chkFormatVideo = new QCheckBox(tr("Short-form Video (.mp4)"), rightBox);
    m_chkFormatVideo->setObjectName(QStringLiteral("checkWorkflowFormatVideo"));
    m_chkFormatVideo->setChecked(uiSettings.value(QStringLiteral("autoBatch/includeVideo"), true).toBool());
    m_comboVideoGen = new QComboBox(rightBox);
    m_comboVideoGen->setObjectName(QStringLiteral("comboWorkflowVideoGenerator"));
    const auto &allVidGens = AbstractVideoGenerator::ALL_VIDEO_GENERATORS();
    for (auto it = allVidGens.constBegin(); it != allVidGens.constEnd(); ++it)
    {
        if (it.value())
        {
            m_comboVideoGen->addItem(it.value()->getName(), it.key());
        }
    }
    const QString savedGenId = uiSettings.value(QStringLiteral("autoBatch/videoGeneratorId"),
                                                QStringLiteral("gemini-browser")).toString();
    const int genIdx = m_comboVideoGen->findData(savedGenId);
    if (genIdx >= 0) m_comboVideoGen->setCurrentIndex(genIdx);
    m_comboVideoGen->setEnabled(m_chkFormatVideo->isChecked() && m_comboVideoGen->count() > 0);
    connect(m_chkFormatVideo, &QCheckBox::toggled, m_comboVideoGen, &QWidget::setEnabled);
    videoRow->addWidget(m_chkFormatVideo);
    videoRow->addWidget(m_comboVideoGen, 1);
    form->addRow(tr("Formats:"), videoRow);

    m_chkFormatCarousel = new QCheckBox(tr("4-Slide Carousel (TikTok Photo Slideshow)"), rightBox);
    m_chkFormatCarousel->setObjectName(QStringLiteral("checkWorkflowFormatCarousel"));
    m_chkFormatCarousel->setChecked(uiSettings.value(QStringLiteral("autoBatch/includeCarousel"), true).toBool());
    form->addRow(QString{}, m_chkFormatCarousel);

    m_chkCarouselOverlay = new QCheckBox(tr("Overlay catchy hook text onto carousel slides"), rightBox);
    m_chkCarouselOverlay->setObjectName(QStringLiteral("checkWorkflowCarouselOverlay"));
    m_chkCarouselOverlay->setChecked(uiSettings.value(QStringLiteral("autoBatch/carouselHookOverlay"), true).toBool());
    m_chkCarouselOverlay->setEnabled(m_chkFormatCarousel->isChecked());
    connect(m_chkFormatCarousel, &QCheckBox::toggled, m_chkCarouselOverlay, &QWidget::setEnabled);
    form->addRow(QString{}, m_chkCarouselOverlay);

    m_chkFormatImage = new QCheckBox(tr("Single Editorial Image (Pinterest / Instagram)"), rightBox);
    m_chkFormatImage->setObjectName(QStringLiteral("checkWorkflowFormatImage"));
    m_chkFormatImage->setChecked(uiSettings.value(QStringLiteral("autoBatch/includeImage"), false).toBool());
    form->addRow(QString{}, m_chkFormatImage);

    rightLayout->addLayout(form);

    // Inline generation progress box
    m_genProgressBox = new QWidget(rightBox);
    m_genProgressBox->setObjectName(QStringLiteral("widgetWorkflowGenProgress"));
    auto *progLayout = new QVBoxLayout(m_genProgressBox);
    progLayout->setContentsMargins(0, 6, 0, 0);

    auto *progHeader = new QHBoxLayout;
    m_genStatusLabel = new QLabel(tr("Ready to generate."), m_genProgressBox);
    m_genStatusLabel->setObjectName(QStringLiteral("labelWorkflowGenStatus"));
    m_genStatusLabel->setWordWrap(true);
    m_genStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_btnCancelGen = new QPushButton(tr("Cancel Generation"), m_genProgressBox);
    m_btnCancelGen->setObjectName(QStringLiteral("buttonWorkflowCancelGen"));
    m_btnCancelGen->setStyleSheet(QStringLiteral("color: #e89774; font-weight: 600;"));
    m_btnCancelGen->setVisible(false);
    progHeader->addWidget(m_genStatusLabel, 1);
    progHeader->addWidget(m_btnCancelGen);
    progLayout->addLayout(progHeader);

    m_genProgressBar = new QProgressBar(m_genProgressBox);
    m_genProgressBar->setObjectName(QStringLiteral("progressWorkflowGeneration"));
    m_genProgressBar->setRange(0, 1);
    m_genProgressBar->setValue(0);
    progLayout->addWidget(m_genProgressBar);

    m_genLogConsole = new QPlainTextEdit(m_genProgressBox);
    m_genLogConsole->setObjectName(QStringLiteral("textWorkflowGenLog"));
    m_genLogConsole->setReadOnly(true);
    m_genLogConsole->setMaximumBlockCount(3000);
    m_genLogConsole->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: monospace; font-size: 11px; }"));
    progLayout->addWidget(m_genLogConsole, 1);

    rightLayout->addWidget(m_genProgressBox, 1);
    splitter->addWidget(rightBox);
    splitter->setSizes({620, 500});

    // Bottom action bar of Step 1
    auto *bottomBar = new QHBoxLayout;
    m_btnSkipToStep2 = new QPushButton(tr("Skip to Step 2: Review Existing Content →"), page);
    m_btnSkipToStep2->setObjectName(QStringLiteral("buttonWorkflowSkipToStep2"));
    m_btnSkipToStep2->setMinimumHeight(36);

    m_btnGenerateAndNext = new QPushButton(tr("✨ Generate Selected Products && Continue to Step 2 →"), page);
    m_btnGenerateAndNext->setObjectName(QStringLiteral("buttonWorkflowGenerate"));
    m_btnGenerateAndNext->setMinimumHeight(36);
    m_btnGenerateAndNext->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: white; font-weight: 700; padding: 6px 16px; border-radius: 6px; }"
        "QPushButton:disabled { background: #374151; color: #9ca3af; }"));

    bottomBar->addStretch();
    bottomBar->addWidget(m_btnSkipToStep2);
    bottomBar->addWidget(m_btnGenerateAndNext);
    pageLayout->addLayout(bottomBar);

    // Step 1 signal connections
    connect(m_prodSearchEdit, &QLineEdit::textChanged, this, [this]() { refreshStep1Products(); });
    connect(m_chkOnlySelectedProducts, &QCheckBox::toggled, this, [this]() { refreshStep1Products(); });
    connect(m_btnCheckAllProducts, &QPushButton::clicked, this, [this]() {
        m_updatingUi = true;
        for (int i = 0; i < m_productList->count(); ++i)
        {
            auto *it = m_productList->item(i);
            it->setCheckState(Qt::Checked);
            m_checkedProductIds.insert(it->data(Qt::UserRole).toString());
        }
        m_updatingUi = false;
        if (m_chkOnlySelectedProducts->isChecked())
        {
            refreshStep1Products();
        }
        else
        {
            _updateStep1SummaryAndButtons();
        }
    });
    connect(m_btnClearProducts, &QPushButton::clicked, this, [this]() {
        m_checkedProductIds.clear();
        m_updatingUi = true;
        for (int i = 0; i < m_productList->count(); ++i)
        {
            m_productList->item(i)->setCheckState(Qt::Unchecked);
        }
        m_updatingUi = false;
        if (m_chkOnlySelectedProducts->isChecked())
        {
            refreshStep1Products();
        }
        else
        {
            _updateStep1SummaryAndButtons();
        }
    });
    connect(m_productList, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_updatingUi || !it)
        {
            return;
        }
        const QString pid = it->data(Qt::UserRole).toString();
        if (it->checkState() == Qt::Checked)
        {
            m_checkedProductIds.insert(pid);
        }
        else
        {
            m_checkedProductIds.remove(pid);
        }
        _updateStep1SummaryAndButtons();
    });
    connect(m_productList, &QListWidget::currentRowChanged, this, [this](int) {
        _updateStep1ProductPreview();
    });

    connect(m_spinGenPerProduct, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        _updateStep1SummaryAndButtons();
    });
    connect(m_comboPromptCli, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (auto *cli = m_comboPromptCli->currentData().value<AbstractCli *>())
        {
            QSettings().setValue(QStringLiteral("generation/promptCli"), cli->getName());
            if (m_paneGeneration)
            {
                m_paneGeneration->setSelectedPromptCli(cli->getName());
            }
            if (m_store)
            {
                QSettings s(m_store->directory().filePath(QStringLiteral("ui.ini")), QSettings::IniFormat);
                s.setValue(QStringLiteral("autoBatch/promptCli"), cli->getName());
            }
        }
        _updateStep1SummaryAndButtons();
    });
    connect(m_comboImageCli, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (auto *cli = m_comboImageCli->currentData().value<AbstractCli *>())
        {
            QSettings().setValue(QStringLiteral("generation/imageCli"), cli->getName());
            if (m_store)
            {
                QSettings s(m_store->directory().filePath(QStringLiteral("ui.ini")), QSettings::IniFormat);
                s.setValue(QStringLiteral("autoBatch/imageCli"), cli->getName());
            }
        }
        _updateStep1SummaryAndButtons();
    });
    connect(m_chkFormatVideo, &QCheckBox::toggled, this, [this](bool) { _updateStep1SummaryAndButtons(); });
    connect(m_chkFormatCarousel, &QCheckBox::toggled, this, [this](bool) { _updateStep1SummaryAndButtons(); });
    connect(m_chkFormatImage, &QCheckBox::toggled, this, [this](bool) { _updateStep1SummaryAndButtons(); });

    connect(m_btnCancelGen, &QPushButton::clicked, this, [this]() {
        if (m_paneGeneration)
        {
            m_paneGeneration->cancelAutoBatch();
        }
    });
    connect(m_btnSkipToStep2, &QPushButton::clicked, this, [this]() {
        refreshStep2Candidates(true);
        setCurrentStep(1);
    });
    connect(m_btnGenerateAndNext, &QPushButton::clicked, this, &PaneWorkflow::startStep1Generation);

    return page;
}

QWidget *PaneWorkflow::_buildStep2Page()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageWorkflowStep2"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    QSettings settings;

    // Top filter + AI select bar
    auto *topBar = new QHBoxLayout;
    topBar->addWidget(new QLabel(tr("Max age:"), page));
    m_spinMaxDaysOld = new QSpinBox(page);
    m_spinMaxDaysOld->setObjectName(QStringLiteral("spinWorkflowMaxDaysOld"));
    m_spinMaxDaysOld->setRange(0, 365);
    m_spinMaxDaysOld->setSpecialValueText(tr("All ages"));
    m_spinMaxDaysOld->setSuffix(tr(" day(s)"));
    m_spinMaxDaysOld->setValue(settings.value(QStringLiteral("publishing/maxGenerationAgeDays"), 10).toInt());
    topBar->addWidget(m_spinMaxDaysOld);

    topBar->addSpacing(8);
    topBar->addWidget(new QLabel(tr("Posts to select:"), page));
    m_spinTargetPosts = new QSpinBox(page);
    m_spinTargetPosts->setObjectName(QStringLiteral("spinWorkflowTargetPosts"));
    m_spinTargetPosts->setRange(1, 100);
    m_spinTargetPosts->setValue(settings.value(QStringLiteral("publishing/totalPosts"), 6).toInt());
    topBar->addWidget(m_spinTargetPosts);

    topBar->addSpacing(8);
    topBar->addWidget(new QLabel(tr("AI Planner:"), page));
    m_comboPlannerCli = new QComboBox(page);
    m_comboPlannerCli->setObjectName(QStringLiteral("comboWorkflowPlannerCli"));
    for (AbstractCli *cli : m_clis)
    {
        if (cli)
        {
            m_comboPlannerCli->addItem(cli->getName(), QVariant::fromValue(cli));
        }
    }
    if (m_comboPlannerCli->count() == 0)
    {
        m_comboPlannerCli->addItem(tr("(Built-in diversity planner)"), QVariant{});
    }
    else
    {
        const QString lastCli = settings.value(QStringLiteral("publishing/lastPlannerCli")).toString();
        const int idx = m_comboPlannerCli->findText(lastCli);
        if (idx >= 0) m_comboPlannerCli->setCurrentIndex(idx);
    }
    topBar->addWidget(m_comboPlannerCli);

    m_btnAiSelect = new QPushButton(tr("✨ AI Select && Order Best"), page);
    m_btnAiSelect->setObjectName(QStringLiteral("buttonWorkflowAiSelect"));
    m_btnAiSelect->setStyleSheet(QStringLiteral("font-weight: 600; padding: 4px 10px;"));
    topBar->addWidget(m_btnAiSelect);

    topBar->addStretch();

    m_btnRefreshCandidates = new QPushButton(tr("↻ Refresh Disk"), page);
    m_btnRefreshCandidates->setObjectName(QStringLiteral("buttonWorkflowRefreshCandidates"));
    topBar->addWidget(m_btnRefreshCandidates);

    pageLayout->addLayout(topBar);

    // Embedded creative review widget (shared with PanePublishing!)
    m_reviewWidget = new WidgetCreativeReview(page);
    m_reviewWidget->setObjectName(QStringLiteral("widgetWorkflowCreativeReview"));
    m_reviewWidget->setDeleteButtonVisible(true);
    pageLayout->addWidget(m_reviewWidget, 1);

    // Bottom navigation bar
    auto *bottomBar = new QHBoxLayout;
    m_btnBackToStep1 = new QPushButton(tr("← Back to Step 1: Pick Products"), page);
    m_btnBackToStep1->setObjectName(QStringLiteral("buttonWorkflowBackToStep1"));
    m_btnBackToStep1->setMinimumHeight(36);

    m_btnContinueToStep3 = new QPushButton(tr("Continue to Step 3: Auto-Publish →"), page);
    m_btnContinueToStep3->setObjectName(QStringLiteral("buttonWorkflowToStep3"));
    m_btnContinueToStep3->setMinimumHeight(36);
    m_btnContinueToStep3->setStyleSheet(QStringLiteral(
        "QPushButton { background: #16a34a; color: white; font-weight: 700; padding: 6px 16px; border-radius: 6px; }"
        "QPushButton:disabled { background: #374151; color: #9ca3af; }"));

    bottomBar->addWidget(m_btnBackToStep1);
    bottomBar->addStretch();
    bottomBar->addWidget(m_btnContinueToStep3);
    pageLayout->addLayout(bottomBar);

    connect(m_spinMaxDaysOld, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        if (!m_updatingUi)
        {
            refreshStep2Candidates(false);
        }
    });
    connect(m_spinTargetPosts, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
        if (!m_updatingUi)
        {
            refreshStep2Candidates(false);
        }
    });
    connect(m_btnRefreshCandidates, &QPushButton::clicked, this, [this]() {
        refreshStep2Candidates(false);
    });
    connect(m_btnAiSelect, &QPushButton::clicked, this, &PaneWorkflow::runAiSelectInStep2);
    connect(m_reviewWidget, &WidgetCreativeReview::selectionChanged, this,
            [this](int checked, int) {
        m_showingCompletedPublishQueue = false;
        m_btnContinueToStep3->setEnabled(checked > 0);
        m_btnContinueToStep3->setText(checked > 0
            ? tr("Continue to Step 3: Auto-Publish (%1 selected) →").arg(checked)
            : tr("Select at least 1 creative to continue"));
        _updateHeaderAndStepBadges();
        if (currentStep() == 2)
        {
            _refreshStep3QueueTable();
            _updateStep3Buttons();
        }
    });
    connect(m_reviewWidget, &WidgetCreativeReview::deleteCandidateRequested, this,
            [this](const QString &shortCode) {
        deleteCreativeByShortCode(shortCode, true);
    });
    connect(m_btnBackToStep1, &QPushButton::clicked, this, [this]() {
        m_reviewWidget->stopPlayback();
        setCurrentStep(0);
    });
    connect(m_btnContinueToStep3, &QPushButton::clicked, this, [this]() {
        m_reviewWidget->stopPlayback();
        setCurrentStep(2);
    });

    return page;
}

QWidget *PaneWorkflow::_buildStep3Page()
{
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("pageWorkflowStep3"));
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    QSettings settings;

    auto *mainSplitter = new QSplitter(Qt::Horizontal, page);
    pageLayout->addWidget(mainSplitter, 1);

    // Left: Target accounts + Spacing mode radio buttons + Start / Stop controls
    auto *leftBox = new QGroupBox(tr("Step 3A — Target Accounts && Wait Spacing"), mainSplitter);
    auto *leftLayout = new QVBoxLayout(leftBox);

    auto *accRow = new QHBoxLayout;
    accRow->addWidget(new QLabel(tr("Publish to social account(s):"), leftBox));
    accRow->addStretch();
    m_btnReconnectBrowser = new QPushButton(tr("Reconnect…"), leftBox);
    m_btnReconnectBrowser->setObjectName(QStringLiteral("buttonWorkflowReconnectBrowser"));
    m_btnReconnectBrowser->setToolTip(tr("Open Chrome on the selected account's profile to log in or verify the active account."));
    accRow->addWidget(m_btnReconnectBrowser);
    leftLayout->addLayout(accRow);

    m_publishAccountsList = new QListWidget(leftBox);
    m_publishAccountsList->setObjectName(QStringLiteral("listWorkflowPublishAccounts"));
    m_publishAccountsList->setMaximumHeight(115);
    leftLayout->addWidget(m_publishAccountsList);

    auto *form = new QFormLayout;
    m_spinPostsPerDay = new QSpinBox(leftBox);
    m_spinPostsPerDay->setObjectName(QStringLiteral("spinWorkflowPostsPerDay"));
    m_spinPostsPerDay->setRange(1, 24);
    m_spinPostsPerDay->setValue(settings.value(QStringLiteral("publishing/postsPerDay"), 3).toInt());
    form->addRow(tr("Max posts per day:"), m_spinPostsPerDay);

    auto *spacingBox = new QWidget(leftBox);
    auto *spacingLayout = new QVBoxLayout(spacingBox);
    spacingLayout->setContentsMargins(0, 0, 0, 0);
    spacingLayout->setSpacing(4);

    m_radioDaytime12h = new QRadioButton(spacingBox);
    m_radioDaytime12h->setObjectName(QStringLiteral("radioWorkflowSpacingDaytime12h"));
    m_radioEven24h = new QRadioButton(spacingBox);
    m_radioEven24h->setObjectName(QStringLiteral("radioWorkflowSpacingEven24h"));

    auto *fixedRow = new QHBoxLayout;
    fixedRow->setContentsMargins(0, 0, 0, 0);
    m_radioFixedInterval = new QRadioButton(tr("Fixed interval per account:"), spacingBox);
    m_radioFixedInterval->setObjectName(QStringLiteral("radioWorkflowSpacingFixed"));
    m_spinFixedMinutes = new QSpinBox(spacingBox);
    m_spinFixedMinutes->setObjectName(QStringLiteral("spinWorkflowFixedMinutes"));
    m_spinFixedMinutes->setRange(5, 1440);
    m_spinFixedMinutes->setSingleStep(15);
    m_spinFixedMinutes->setSuffix(tr(" min"));
    m_spinFixedMinutes->setValue(settings.value(QStringLiteral("publishing/fixedIntervalMinutes"), 60).toInt());
    fixedRow->addWidget(m_radioFixedInterval);
    fixedRow->addWidget(m_spinFixedMinutes);
    fixedRow->addStretch();

    const QString savedSpacing = settings.value(QStringLiteral("publishing/spacingMode"),
                                                QStringLiteral("daytime12h")).toString();
    if (savedSpacing == QLatin1String("even24h"))
    {
        m_radioEven24h->setChecked(true);
    }
    else if (savedSpacing == QLatin1String("fixed"))
    {
        m_radioFixedInterval->setChecked(true);
    }
    else
    {
        m_radioDaytime12h->setChecked(true);
    }
    m_spinFixedMinutes->setEnabled(m_radioFixedInterval->isChecked());
    connect(m_radioFixedInterval, &QRadioButton::toggled, m_spinFixedMinutes, &QWidget::setEnabled);

    const auto updateSpacingLabels = [this]() {
        const int n = qMax(1, m_spinPostsPerDay->value());
        const int dayMins = qMax(15, 720 / n);
        const int evenMins = qMax(15, 1440 / n);
        m_radioDaytime12h->setText(
            tr("Active daytime spread (12h / %1 = ~%2 min apart + overnight pause)")
                .arg(n).arg(dayMins));
        const double evenHours = double(evenMins) / 60.0;
        m_radioEven24h->setText(
            tr("Even 24h spread (24h / %1 = %2 min / %3h apart)")
                .arg(n).arg(evenMins).arg(QString::number(evenHours, 'f', (evenMins % 60 == 0) ? 0 : 1)));
    };
    updateSpacingLabels();

    spacingLayout->addWidget(m_radioDaytime12h);
    spacingLayout->addWidget(m_radioEven24h);
    spacingLayout->addLayout(fixedRow);
    form->addRow(tr("Wait between posts:"), spacingBox);

    m_chkSuggestedSound = new QCheckBox(
        tr("Mute original video audio && add #1 suggested TikTok sound"), leftBox);
    m_chkSuggestedSound->setObjectName(QStringLiteral("checkWorkflowSuggestedSound"));
    m_chkSuggestedSound->setChecked(settings.value(QStringLiteral("publishing/useSuggestedSound"), true).toBool());
    form->addRow(QString{}, m_chkSuggestedSound);

    m_chkDryRun = new QCheckBox(
        tr("Dry-run test mode (fill Chrome upload form, stop before clicking Post)"), leftBox);
    m_chkDryRun->setObjectName(QStringLiteral("checkWorkflowDryRun"));
    m_chkDryRun->setChecked(settings.value(QStringLiteral("publishing/dryRun"), false).toBool());
    form->addRow(QString{}, m_chkDryRun);

    leftLayout->addLayout(form);
    leftLayout->addStretch();

    m_btnStartPublish = new QPushButton(tr("🚀 Start Auto-Publish Job"), leftBox);
    m_btnStartPublish->setObjectName(QStringLiteral("buttonWorkflowStartPublish"));
    m_btnStartPublish->setMinimumHeight(38);
    m_btnStartPublish->setStyleSheet(QStringLiteral(
        "QPushButton { background: #16a34a; color: white; font-weight: 700; padding: 8px 14px; border-radius: 6px; }"
        "QPushButton:disabled { background: #374151; color: #9ca3af; }"));
    leftLayout->addWidget(m_btnStartPublish);

    auto *jobCtlRow = new QHBoxLayout;
    m_btnPublishNextNow = new QPushButton(tr("Publish Next Now"), leftBox);
    m_btnPublishNextNow->setObjectName(QStringLiteral("buttonWorkflowPublishNextNow"));
    m_btnStopPublish = new QPushButton(tr("Stop Job"), leftBox);
    m_btnStopPublish->setObjectName(QStringLiteral("buttonWorkflowStopPublish"));
    m_btnStopPublish->setStyleSheet(QStringLiteral("color: #e89774; font-weight: 600;"));
    auto *btnDebugDir = new QPushButton(tr("Open Debug Folder"), leftBox);
    jobCtlRow->addWidget(m_btnPublishNextNow);
    jobCtlRow->addWidget(m_btnStopPublish);
    jobCtlRow->addWidget(btnDebugDir);
    leftLayout->addLayout(jobCtlRow);

    mainSplitter->addWidget(leftBox);

    // Right: Live schedule preview / running queue + diagnostic console
    auto *rightBox = new QGroupBox(tr("Step 3B — Scheduled Queue && Live Diagnostic Logs"), mainSplitter);
    auto *rightLayout = new QVBoxLayout(rightBox);

    m_publishStatusLabel = new QLabel(tr("Ready — review your scheduled queue below and click Start Auto-Publish Job."), rightBox);
    m_publishStatusLabel->setObjectName(QStringLiteral("labelWorkflowPublishStatus"));
    m_publishStatusLabel->setWordWrap(true);
    m_publishStatusLabel->setStyleSheet(QStringLiteral(
        "background: rgba(128, 140, 160, 0.12); border: 1px solid rgba(128, 140, 160, 0.28); "
        "border-radius: 5px; padding: 6px 8px;"));
    rightLayout->addWidget(m_publishStatusLabel);

    auto *queueLogSplitter = new QSplitter(Qt::Vertical, rightBox);
    m_publishQueueTable = new QTableWidget(0, 7, queueLogSplitter);
    m_publishQueueTable->setObjectName(QStringLiteral("tableWorkflowPublishQueue"));
    m_publishQueueTable->setHorizontalHeaderLabels({
        tr("#"), tr("Code"), tr("Format"), tr("Product"),
        tr("Target Account"), tr("Scheduled Time"), tr("Status / Overlay")
    });
    m_publishQueueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_publishQueueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_publishQueueTable->verticalHeader()->setVisible(false);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_publishQueueTable->horizontalHeader()->resizeSection(3, 190);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    m_publishQueueTable->horizontalHeader()->resizeSection(4, 190);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_publishQueueTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    queueLogSplitter->addWidget(m_publishQueueTable);

    m_publishLogConsole = new QPlainTextEdit(queueLogSplitter);
    m_publishLogConsole->setObjectName(QStringLiteral("textWorkflowPublishLogs"));
    m_publishLogConsole->setReadOnly(true);
    m_publishLogConsole->setMaximumBlockCount(5000);
    m_publishLogConsole->setPlaceholderText(tr("Browser upload progress, TikTok text overlay, sound attachment, and live diagnostic logs stream here…"));
    m_publishLogConsole->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: monospace; font-size: 11px; }"));
    queueLogSplitter->addWidget(m_publishLogConsole);
    queueLogSplitter->setSizes({260, 220});

    rightLayout->addWidget(queueLogSplitter, 1);
    mainSplitter->addWidget(rightBox);
    mainSplitter->setSizes({440, 680});

    // Bottom navigation bar
    auto *bottomBar = new QHBoxLayout;
    m_btnBackToStep2 = new QPushButton(tr("← Back to Step 2: Review Content"), page);
    m_btnBackToStep2->setObjectName(QStringLiteral("buttonWorkflowBackToStep2"));
    m_btnBackToStep2->setMinimumHeight(36);
    bottomBar->addWidget(m_btnBackToStep2);
    bottomBar->addStretch();
    pageLayout->addLayout(bottomBar);

    const auto onScheduleParamEdited = [this, updateSpacingLabels]() {
        if (m_updatingUi)
        {
            return;
        }
        m_showingCompletedPublishQueue = false;
        updateSpacingLabels();
        if (m_reviewWidget)
        {
            m_reviewWidget->updateScheduleConfig(_currentPublishConfig());
        }
        _refreshStep3QueueTable();
        _updateStep3Buttons();
    };

    connect(m_publishAccountsList, &QListWidget::itemChanged, this, [onScheduleParamEdited](QListWidgetItem *) {
        onScheduleParamEdited();
    });
    connect(m_spinPostsPerDay, QOverload<int>::of(&QSpinBox::valueChanged), this, [onScheduleParamEdited](int) {
        onScheduleParamEdited();
    });
    connect(m_radioDaytime12h, &QRadioButton::toggled, this, [onScheduleParamEdited](bool) {
        onScheduleParamEdited();
    });
    connect(m_radioEven24h, &QRadioButton::toggled, this, [onScheduleParamEdited](bool) {
        onScheduleParamEdited();
    });
    connect(m_radioFixedInterval, &QRadioButton::toggled, this, [onScheduleParamEdited](bool) {
        onScheduleParamEdited();
    });
    connect(m_spinFixedMinutes, QOverload<int>::of(&QSpinBox::valueChanged), this, [onScheduleParamEdited](int) {
        onScheduleParamEdited();
    });

    connect(m_btnReconnectBrowser, &QPushButton::clicked, this, [this]() {
        if (!m_panePublishing)
        {
            return;
        }
        QString targetUrl;
        if (auto *cur = m_publishAccountsList->currentItem())
        {
            targetUrl = cur->data(Qt::UserRole + 2).toString();
        }
        if (targetUrl.isEmpty())
        {
            for (int i = 0; i < m_publishAccountsList->count(); ++i)
            {
                if (m_publishAccountsList->item(i)->checkState() == Qt::Checked)
                {
                    targetUrl = m_publishAccountsList->item(i)->data(Qt::UserRole + 2).toString();
                    break;
                }
            }
        }
        m_panePublishing->reconnectBrowserSession(targetUrl);
    });
    connect(m_btnStartPublish, &QPushButton::clicked, this, &PaneWorkflow::startStep3PublishJob);
    connect(m_btnPublishNextNow, &QPushButton::clicked, this, [this]() {
        if (m_panePublishing) m_panePublishing->publishNextNow();
    });
    connect(m_btnStopPublish, &QPushButton::clicked, this, [this]() {
        if (m_panePublishing) m_panePublishing->stopJob();
    });
    connect(btnDebugDir, &QPushButton::clicked, this, [this]() {
        if (m_panePublishing) m_panePublishing->openDebugLogsFolder();
    });
    connect(m_btnBackToStep2, &QPushButton::clicked, this, [this]() {
        setCurrentStep(1);
    });

    return page;
}

int PaneWorkflow::currentStep() const
{
    return m_stepStack ? m_stepStack->currentIndex() : 0;
}

void PaneWorkflow::setCurrentStep(int stepIndex)
{
    const int clamped = qBound(0, stepIndex, 2);
    if (clamped != 1 && m_reviewWidget)
    {
        m_reviewWidget->stopPlayback();
    }
    if (clamped == 1)
    {
        refreshStep2Candidates(m_step2Initialized);
    }
    else if (clamped == 2)
    {
        if (!m_step2Initialized)
        {
            refreshStep2Candidates(false);
        }
        refreshStep3AccountsAndPreview();
    }

    m_stepStack->setCurrentIndex(clamped);

    const auto styleStep = [](QPushButton *btn, bool active) {
        if (!btn) return;
        btn->setChecked(active);
        btn->setStyleSheet(active
            ? QStringLiteral(
                  "QPushButton { background: #2563eb; color: white; font-weight: 700; "
                  "border: 1px solid #60a5fa; border-radius: 6px; padding: 6px 12px; }")
            : QStringLiteral(
                  "QPushButton { background: rgba(55, 65, 81, 0.55); color: #e5e7eb; font-weight: 600; "
                  "border: 1px solid rgba(107, 114, 128, 0.45); border-radius: 6px; padding: 6px 12px; }"));
    };
    styleStep(m_btnStep1, clamped == 0);
    styleStep(m_btnStep2, clamped == 1);
    styleStep(m_btnStep3, clamped == 2);
    _updateHeaderAndStepBadges();
}

void PaneWorkflow::refreshAll()
{
    const QString prevAccountId = _selectedAccountId();
    m_updatingUi = true;
    m_accountCombo->clear();
    if (m_accounts)
    {
        for (int i = 0; i < m_accounts->rowCount(); ++i)
        {
            const QString label = m_accounts->data(m_accounts->index(i, TableAccounts::IND_TYPE)).toString()
                + QStringLiteral(" — ")
                + m_accounts->data(m_accounts->index(i, TableAccounts::IND_URL)).toString();
            m_accountCombo->addItem(label, m_accounts->accountId(i).toString(QUuid::WithoutBraces));
        }
    }
    const int idx = m_accountCombo->findData(prevAccountId);
    if (idx >= 0)
    {
        m_accountCombo->setCurrentIndex(idx);
    }
    m_updatingUi = false;

    _syncResearchAccountSelection();
    refreshStep1Products();
    refreshStep3AccountsAndPreview();
    if (m_step2Initialized || currentStep() == 1)
    {
        refreshStep2Candidates(m_step2Initialized);
    }
    _updateHeaderAndStepBadges();
}

QString PaneWorkflow::_selectedAccountId() const
{
    return m_accountCombo ? m_accountCombo->currentData().toString() : QString{};
}

void PaneWorkflow::_syncResearchAccountSelection()
{
    if (!m_paneResearch)
    {
        return;
    }
    if (auto *resCombo = m_paneResearch->findChild<QComboBox *>(QStringLiteral("researchAccount")))
    {
        const int idx = resCombo->findData(_selectedAccountId());
        if (idx >= 0 && resCombo->currentIndex() != idx)
        {
            resCombo->setCurrentIndex(idx);
        }
    }
}

void PaneWorkflow::refreshStep1Products()
{
    if (!m_store || !m_productList)
    {
        return;
    }

    const QString curAccount = _selectedAccountId();
    const QJsonObject profile = m_store->profile(curAccount);
    const QJsonArray allProducts = m_store->products();

    QList<QJsonObject> eligible;
    for (const QJsonValue &v : allProducts)
    {
        const QJsonObject p = v.toObject();
        if (ResearchStore::productRestriction(p, profile).isEmpty()
            && !jsonText(p, "localImage").isEmpty()
            && QFileInfo::exists(jsonText(p, "localImage")))
        {
            eligible.append(p);
        }
    }

    QSet<QString> validEligibleIds;
    for (const auto &p : eligible)
    {
        validEligibleIds.insert(jsonText(p, "id"));
    }
    m_checkedProductIds.intersect(validEligibleIds);
    if (m_checkedProductIds.isEmpty() && !eligible.isEmpty())
    {
        m_checkedProductIds.insert(jsonText(eligible.first(), "id"));
    }

    int approvedRefCount = 0;
    for (const QJsonValue &v : m_store->references())
    {
        const QJsonObject r = v.toObject();
        if (jsonText(r, "accountId") == curAccount && jsonText(r, "status") == QLatin1String("approved"))
        {
            ++approvedRefCount;
        }
    }

    if (curAccount.isEmpty())
    {
        m_inspirationsInfoLabel->setText(
            tr("Add a social account first (click \"Accounts…\" at top right)."));
    }
    else if (allProducts.isEmpty())
    {
        m_inspirationsInfoLabel->setText(
            tr("No product catalog imported yet — click \"Open Research…\" above to import your products."));
    }
    else if (eligible.isEmpty())
    {
        m_inspirationsInfoLabel->setText(
            tr("0 products match this account's brand/category rules — click \"Open Research…\" → \"Account rules…\" to allow your products."));
    }
    else if (approvedRefCount > 0)
    {
        m_inspirationsInfoLabel->setText(
            tr("✓ %1 approved inspiration(s) ready for this account — QuantumSocial will automatically blend "
               "proven inspirations & measured Recorder adaptation methods (80% winners, 20% exploration).")
                .arg(approvedRefCount));
    }
    else
    {
        m_inspirationsInfoLabel->setText(
            tr("0 approved inspirations yet — will generate fresh editorial A/B creatives from your product photos "
               "(open Research anytime to approve Pinterest/TikTok inspirations!)."));
    }

    const QString needle = m_prodSearchEdit ? m_prodSearchEdit->text().trimmed() : QString{};
    const bool onlyChecked = m_chkOnlySelectedProducts && m_chkOnlySelectedProducts->isChecked();

    std::stable_sort(eligible.begin(), eligible.end(),
                     [this](const QJsonObject &a, const QJsonObject &b) {
                         const bool ca = m_checkedProductIds.contains(jsonText(a, "id"));
                         const bool cb = m_checkedProductIds.contains(jsonText(b, "id"));
                         return ca != cb ? ca : false;
                     });

    m_updatingUi = true;
    const QString prevSelectedPid = m_productList->currentItem()
        ? m_productList->currentItem()->data(Qt::UserRole).toString()
        : QString{};
    m_productList->clear();

    int restoreRow = 0;
    for (const QJsonObject &p : eligible)
    {
        const QString pid = jsonText(p, "id");
        const bool isChecked = m_checkedProductIds.contains(pid);
        if (onlyChecked && !isChecked)
        {
            continue;
        }
        const QString color = jsonText(p, "color");
        const QString title = jsonText(p, "title");
        const QString asin = jsonText(p, "asin");
        QStringList catParts;
        for (const QJsonValue &cv : p.value(QStringLiteral("categories")).toArray())
        {
            catParts << cv.toString();
        }
        if (!jsonText(p, "category").isEmpty())
        {
            catParts << jsonText(p, "category");
        }
        const QString category = catParts.join(QLatin1Char(' '));
        const QString label = (!color.isEmpty() ? color + QStringLiteral(" — ") : QString{}) + title;

        if (!needle.isEmpty())
        {
            const QString hay = label + QLatin1Char(' ') + asin + QLatin1Char(' ') + category;
            if (!hay.contains(needle, Qt::CaseInsensitive))
            {
                continue;
            }
        }

        const QString imgPath = jsonText(p, "localImage");
        auto *item = new QListWidgetItem(label, m_productList);
        item->setData(Qt::UserRole, pid);
        item->setData(Qt::UserRole + 1, imgPath);
        const QPixmap thumb = loadFastProductThumb(imgPath, 48);
        if (!thumb.isNull())
        {
            item->setIcon(QIcon(thumb));
        }
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(isChecked ? Qt::Checked : Qt::Unchecked);
        if (pid == prevSelectedPid)
        {
            restoreRow = m_productList->count() - 1;
        }
    }
    if (m_productList->count() > 0)
    {
        m_productList->setCurrentRow(restoreRow);
    }
    m_btnCheckAllProducts->setText(tr("Check all (%1)").arg(eligible.size()));
    m_updatingUi = false;

    _updateStep1ProductPreview();
    _updateStep1SummaryAndButtons();
}

void PaneWorkflow::_updateStep1ProductPreview()
{
    auto *cur = m_productList ? m_productList->currentItem() : nullptr;
    if (!cur)
    {
        m_prodPreviewImg->setPixmap(QPixmap{});
        m_prodPreviewImg->setText(tr("No product"));
        m_prodPreviewCaption->clear();
        return;
    }
    const QString imgPath = cur->data(Qt::UserRole + 1).toString();
    const QPixmap big = loadFastProductThumb(imgPath, 168);
    if (!big.isNull())
    {
        m_prodPreviewImg->setText(QString{});
        m_prodPreviewImg->setPixmap(big);
    }
    else
    {
        m_prodPreviewImg->setPixmap(QPixmap{});
        m_prodPreviewImg->setText(tr("No preview"));
    }
    m_prodPreviewCaption->setText(cur->text().left(95));
}

QStringList PaneWorkflow::_checkedStep1ProductIds() const
{
    QStringList ids;
    for (const QString &pid : m_checkedProductIds)
    {
        if (!pid.trimmed().isEmpty())
        {
            ids << pid;
        }
    }
    ids.sort();
    return ids;
}

void PaneWorkflow::_updateStep1SummaryAndButtons()
{
    const int checkedCount = m_checkedProductIds.size();
    const bool anyFormat = (m_chkFormatVideo && m_chkFormatVideo->isChecked())
        || (m_chkFormatCarousel && m_chkFormatCarousel->isChecked())
        || (m_chkFormatImage && m_chkFormatImage->isChecked());
    const bool hasPromptCli = m_comboPromptCli
        && m_comboPromptCli->currentData().value<AbstractCli *>() != nullptr;
    const bool hasImageCli = m_comboImageCli
        && m_comboImageCli->currentData().value<AbstractCli *>() != nullptr;
    const int perProd = m_spinGenPerProduct ? m_spinGenPerProduct->value() : 1;
    const int totalToGen = checkedCount * perProd;
    const bool batchRunning = m_paneGeneration && m_paneGeneration->isAutoBatchRunning();

    if (m_selectedProductsLabel)
    {
        m_selectedProductsLabel->setText(
            tr("Selected: %1 product(s) × %2 generation(s) = %3 creative(s) to generate")
                .arg(checkedCount).arg(perProd).arg(totalToGen));
        m_selectedProductsLabel->setStyleSheet(checkedCount > 0
            ? QStringLiteral("color: #8fd19e; font-weight: 600;")
            : QStringLiteral("color: #e89774; font-weight: 600;"));
    }
    if (m_btnGenerateAndNext)
    {
        m_btnGenerateAndNext->setEnabled(
            !batchRunning && checkedCount > 0 && anyFormat
            && hasPromptCli && hasImageCli && !_selectedAccountId().isEmpty());
        if (!hasPromptCli || !hasImageCli)
        {
            m_btnGenerateAndNext->setText(tr("Select valid Content && Image CLIs above"));
        }
        else
        {
            m_btnGenerateAndNext->setText(checkedCount > 0
                ? tr("✨ Generate %1 Creative(s) && Continue to Step 2 →").arg(totalToGen)
                : tr("Select at least 1 product to generate"));
        }
    }

    if (m_panePublishing && m_btnSkipToStep2)
    {
        const auto existing = m_panePublishing->collectEligibleCreatives(
            m_spinMaxDaysOld ? m_spinMaxDaysOld->value() : 10, true);
        m_btnSkipToStep2->setText(existing.isEmpty()
            ? tr("Go to Step 2: Review Content →")
            : tr("Skip to Step 2: Review %1 Existing Creative(s) →").arg(existing.size()));
    }
}

void PaneWorkflow::_saveStep1Settings() const
{
    if (!m_store)
    {
        return;
    }
    QSettings settings(m_store->directory().filePath(QStringLiteral("ui.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("autoBatch/generationsPerProduct"), m_spinGenPerProduct->value());
    if (auto *pCli = m_comboPromptCli ? m_comboPromptCli->currentData().value<AbstractCli *>() : nullptr)
    {
        settings.setValue(QStringLiteral("autoBatch/promptCli"), pCli->getName());
        QSettings().setValue(QStringLiteral("generation/promptCli"), pCli->getName());
    }
    if (auto *iCli = m_comboImageCli ? m_comboImageCli->currentData().value<AbstractCli *>() : nullptr)
    {
        settings.setValue(QStringLiteral("autoBatch/imageCli"), iCli->getName());
        QSettings().setValue(QStringLiteral("generation/imageCli"), iCli->getName());
    }
    settings.setValue(QStringLiteral("autoBatch/includeVideo"), m_chkFormatVideo->isChecked());
    settings.setValue(QStringLiteral("autoBatch/includeCarousel"), m_chkFormatCarousel->isChecked());
    settings.setValue(QStringLiteral("autoBatch/carouselHookOverlay"), m_chkCarouselOverlay->isChecked());
    settings.setValue(QStringLiteral("autoBatch/includeImage"), m_chkFormatImage->isChecked());
    settings.setValue(QStringLiteral("autoBatch/videoFormat"), m_comboAspectFormat->currentText());
    settings.setValue(QStringLiteral("autoBatch/videoGeneratorId"), m_comboVideoGen->currentData().toString());
}

void PaneWorkflow::startStep1Generation()
{
    if (!m_paneResearch || !m_paneGeneration)
    {
        return;
    }
    if (m_paneGeneration->isAutoBatchRunning())
    {
        return;
    }
    _syncResearchAccountSelection();
    _saveStep1Settings();

    const QStringList chosenIds = _checkedStep1ProductIds();
    if (chosenIds.isEmpty())
    {
        return;
    }

    QJsonObject spec = m_paneResearch->buildAutoBatchSpec(
        chosenIds,
        m_spinGenPerProduct->value(),
        m_chkFormatImage->isChecked(),
        m_chkFormatCarousel->isChecked(),
        m_chkFormatVideo->isChecked(),
        m_chkCarouselOverlay->isChecked(),
        m_comboAspectFormat->currentText(),
        m_comboVideoGen->currentData().toString());
    if (auto *pCli = m_comboPromptCli ? m_comboPromptCli->currentData().value<AbstractCli *>() : nullptr)
    {
        spec.insert(QStringLiteral("promptCli"), pCli->getName());
    }
    if (auto *iCli = m_comboImageCli ? m_comboImageCli->currentData().value<AbstractCli *>() : nullptr)
    {
        spec.insert(QStringLiteral("imageCli"), iCli->getName());
    }
    spec.insert(QStringLiteral("suppressModalProgressDialog"), true);

    m_genLogConsole->clear();
    m_triggeredBatchFromWorkflow = true;
    if (!m_paneGeneration->startAutoBatch(spec))
    {
        m_triggeredBatchFromWorkflow = false;
    }
}

void PaneWorkflow::refreshStep2Candidates(bool preserveExistingSelection)
{
    if (!m_panePublishing || !m_reviewWidget)
    {
        return;
    }

    const auto cfg = _currentPublishConfig();
    bool skipSingleImages = cfg.targets.isEmpty();
    for (const auto &t : cfg.targets)
    {
        if (t.platformId == QLatin1String("tiktok"))
        {
            skipSingleImages = true;
            break;
        }
    }

    int olderExcluded = 0;
    int singleExcluded = 0;
    const QList<PanePublishing::EligibleCreative> candidates =
        m_panePublishing->collectEligibleCreatives(
            cfg.maxDaysOld, skipSingleImages, &olderExcluded, &singleExcluded);

    // If preserveExistingSelection is requested and the candidate count matches what's already loaded,
    // simply refresh the target/timing config without wiping the user's manual ordering or video text edits!
    if (preserveExistingSelection && m_step2Initialized
        && m_reviewWidget->totalCount() == candidates.size() && !candidates.isEmpty())
    {
        m_reviewWidget->updateScheduleConfig(cfg);
        return;
    }

    QString expSummary;
    const QList<PanePublishing::ScheduledItem> initialQueue =
        PanePublishing::buildDeterministicDefaultQueue(
            cfg, candidates, m_videos, m_favoriteHooks, &expSummary);
    const int baseMins = PanePublishing::baseIntervalMinutesForConfig(cfg);

    m_reviewWidget->setCandidates(
        cfg,
        candidates,
        initialQueue,
        baseMins,
        expSummary,
        m_panePublishing->projectsModel());

    if (candidates.isEmpty())
    {
        QString emptyHtml = tr("<b>0 unpublished creatives</b> match the current age filter (≤ %1 day(s) old).")
            .arg(cfg.maxDaysOld);
        if (olderExcluded > 0)
        {
            emptyHtml += tr(" %1 older creative(s) were skipped — set <b>Max age</b> to 0 to include all ages.")
                .arg(olderExcluded);
        }
        else
        {
            emptyHtml += tr(" Go to <b>Step 1: Pick Products</b> to generate a fresh batch!");
        }
        m_reviewWidget->setBannerText(emptyHtml);
    }

    m_step2Initialized = true;
    _updateHeaderAndStepBadges();
}

void PaneWorkflow::runAiSelectInStep2()
{
    if (!m_panePublishing || m_aiPlanningInProgress)
    {
        return;
    }
    m_aiPlanTask = std::make_unique<QCoro::Task<void>>(_runAiSelectCoroutine());
}

QCoro::Task<void> PaneWorkflow::_runAiSelectCoroutine()
{
    if (!m_panePublishing || !m_reviewWidget)
    {
        co_return;
    }
    const auto cfg = _currentPublishConfig();
    bool skipSingle = cfg.targets.isEmpty();
    for (const auto &t : cfg.targets)
    {
        if (t.platformId == QLatin1String("tiktok")) skipSingle = true;
    }
    const auto candidates = m_panePublishing->collectEligibleCreatives(cfg.maxDaysOld, skipSingle);
    if (candidates.isEmpty())
    {
        co_return;
    }

    m_aiPlanningInProgress = true;
    m_aiPlanCancelFlag = std::make_shared<bool>(false);
    const auto cancelToken = m_aiPlanCancelFlag;
    m_btnAiSelect->setEnabled(false);
    m_btnAiSelect->setText(tr("Planning with AI…"));

    const QPointer<PaneWorkflow> self(this);
    QString expSummary;
    const QList<PanePublishing::ScheduledItem> aiQueue =
        co_await m_panePublishing->buildAiPlannedQueue(cfg, candidates, &expSummary, cancelToken);
    if (!self || (cancelToken && *cancelToken))
    {
        co_return;
    }

    m_aiPlanningInProgress = false;
    m_btnAiSelect->setEnabled(true);
    m_btnAiSelect->setText(tr("✨ AI Select && Order Best"));

    const int baseMins = PanePublishing::baseIntervalMinutesForConfig(cfg);
    m_reviewWidget->setCandidates(
        cfg,
        candidates,
        aiQueue,
        baseMins,
        expSummary,
        m_panePublishing->projectsModel());
    m_step2Initialized = true;
    _updateHeaderAndStepBadges();
}

bool PaneWorkflow::deleteCreativeByShortCode(const QString &shortCode, bool askConfirmation)
{
    if (!m_paneGeneration || shortCode.trimmed().isEmpty())
    {
        return false;
    }
    if (askConfirmation)
    {
        const auto ans = QMessageBox::question(
            this,
            tr("Delete Unpublished Creative"),
            tr("Permanently delete unpublished creative %1 from disk?").arg(shortCode),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ans != QMessageBox::Yes)
        {
            return false;
        }
    }

    QString err;
    if (m_reviewWidget)
    {
        m_reviewWidget->stopPlayback();
    }
    if (!m_paneGeneration->deleteUnpublishedCreative(shortCode, &err))
    {
        if (askConfirmation && !err.isEmpty())
        {
            QMessageBox::warning(this, tr("Cannot delete creative"), err);
        }
        return false;
    }
    if (m_reviewWidget)
    {
        m_reviewWidget->removeCandidateByShortCode(shortCode);
    }
    _updateStep1SummaryAndButtons();
    _updateHeaderAndStepBadges();
    return true;
}

void PaneWorkflow::refreshStep3AccountsAndPreview()
{
    if (!m_publishAccountsList)
    {
        return;
    }

    QSet<QString> prevCheckedUrls;
    bool hadPreviousItems = m_publishAccountsList->count() > 0;
    for (int i = 0; i < m_publishAccountsList->count(); ++i)
    {
        if (m_publishAccountsList->item(i)->checkState() == Qt::Checked)
        {
            prevCheckedUrls.insert(m_publishAccountsList->item(i)->data(Qt::UserRole + 2).toString());
        }
    }

    m_updatingUi = true;
    m_publishAccountsList->clear();
    if (m_accounts)
    {
        for (int r = 0; r < m_accounts->rowCount(); ++r)
        {
            const QString typeStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_TYPE)).toString();
            const QString urlStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_URL)).toString();
            const qint64 followers = m_accounts->data(m_accounts->index(r, TableAccounts::IND_FOLLOWERS)).toLongLong();
            AbstractSocialAccount *platform = SocialStatistics::resolveSocialAccount(typeStr);
            const bool supported = platform && platform->supportsAutoPublish();

            QString label = QStringLiteral("%1 — %2").arg(typeStr, urlStr);
            if (!supported)
            {
                label += tr(" (auto-publish coming soon)");
            }
            auto *item = new QListWidgetItem(label, m_publishAccountsList);
            item->setData(Qt::UserRole, platform ? platform->getId() : QString{});
            item->setData(Qt::UserRole + 1, platform ? platform->getName() : typeStr);
            item->setData(Qt::UserRole + 2, urlStr);
            item->setData(Qt::UserRole + 3, followers);
            if (supported)
            {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                const bool checkIt = !hadPreviousItems || prevCheckedUrls.contains(urlStr);
                item->setCheckState(checkIt ? Qt::Checked : Qt::Unchecked);
            }
            else
            {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                item->setCheckState(Qt::Unchecked);
            }
        }
    }
    m_updatingUi = false;

    if (m_reviewWidget && m_step2Initialized)
    {
        m_reviewWidget->updateScheduleConfig(_currentPublishConfig());
    }
    _refreshStep3QueueTable();
    _updateStep3Buttons();
}

PanePublishing::PublishJobConfig PaneWorkflow::_currentPublishConfig() const
{
    PanePublishing::PublishJobConfig cfg;
    cfg.maxDaysOld = m_spinMaxDaysOld ? m_spinMaxDaysOld->value() : 10;
    cfg.postsPerDay = m_spinPostsPerDay ? m_spinPostsPerDay->value() : 3;
    cfg.totalPosts = m_spinTargetPosts ? m_spinTargetPosts->value() : 6;
    if (m_radioEven24h && m_radioEven24h->isChecked())
    {
        cfg.spacingMode = PanePublishing::SpacingMode::EvenSpread24h;
    }
    else if (m_radioFixedInterval && m_radioFixedInterval->isChecked())
    {
        cfg.spacingMode = PanePublishing::SpacingMode::FixedInterval;
    }
    else
    {
        cfg.spacingMode = PanePublishing::SpacingMode::DaytimeSpread12h;
    }
    cfg.fixedIntervalMinutes = m_spinFixedMinutes ? m_spinFixedMinutes->value() : 60;
    cfg.muteOriginalAudio = !m_chkSuggestedSound || m_chkSuggestedSound->isChecked();
    cfg.useSuggestedSound = !m_chkSuggestedSound || m_chkSuggestedSound->isChecked();
    cfg.dryRun = m_chkDryRun && m_chkDryRun->isChecked();
    cfg.reviewBeforePublish = false; // Reviewed inline in Step 2!
    if (m_comboPlannerCli)
    {
        cfg.plannerCli = m_comboPlannerCli->currentData().value<AbstractCli *>();
    }
    cfg.uploadUrlOverride = m_uploadUrlOverride;
    if (!m_uploadUrlOverride.isEmpty())
    {
        cfg.useSystemChromeProfile = false;
    }

    if (m_publishAccountsList)
    {
        for (int i = 0; i < m_publishAccountsList->count(); ++i)
        {
            auto *it = m_publishAccountsList->item(i);
            if (it->checkState() == Qt::Checked)
            {
                cfg.targets.append(PanePublishing::AccountTarget{
                    it->data(Qt::UserRole).toString(),
                    it->data(Qt::UserRole + 1).toString(),
                    it->data(Qt::UserRole + 2).toString(),
                    it->data(Qt::UserRole + 3).toLongLong()
                });
            }
        }
    }
    return cfg;
}

void PaneWorkflow::_saveStep3Settings() const
{
    QSettings settings;
    const QString spacingKey = (m_radioEven24h && m_radioEven24h->isChecked())
        ? QStringLiteral("even24h")
        : ((m_radioFixedInterval && m_radioFixedInterval->isChecked())
               ? QStringLiteral("fixed")
               : QStringLiteral("daytime12h"));
    settings.setValue(QStringLiteral("publishing/maxGenerationAgeDays"), m_spinMaxDaysOld->value());
    settings.setValue(QStringLiteral("publishing/postsPerDay"), m_spinPostsPerDay->value());
    settings.setValue(QStringLiteral("publishing/totalPosts"), m_spinTargetPosts->value());
    settings.setValue(QStringLiteral("publishing/spacingMode"), spacingKey);
    settings.setValue(QStringLiteral("publishing/fixedIntervalMinutes"), m_spinFixedMinutes->value());
    settings.setValue(QStringLiteral("publishing/useSuggestedSound"), m_chkSuggestedSound->isChecked());
    settings.setValue(QStringLiteral("publishing/dryRun"), m_chkDryRun->isChecked());
    if (auto *cli = m_comboPlannerCli->currentData().value<AbstractCli *>())
    {
        settings.setValue(QStringLiteral("publishing/lastPlannerCli"), cli->getName());
    }
}

void PaneWorkflow::_refreshStep3QueueTable()
{
    if (!m_publishQueueTable)
    {
        return;
    }

    QList<PanePublishing::ScheduledItem> displayQueue;
    const bool jobRunningOrFinishedWithQueue =
        m_panePublishing && (m_panePublishing->isJobRunning() || !m_panePublishing->activeQueue().isEmpty());

    if (m_panePublishing
        && (m_panePublishing->isJobRunning()
            || (m_showingCompletedPublishQueue && !m_panePublishing->activeQueue().isEmpty())))
    {
        displayQueue = m_panePublishing->activeQueue();
    }
    else if (m_reviewWidget && m_reviewWidget->checkedCount() > 0)
    {
        const auto cfg = _currentPublishConfig();
        displayQueue = m_reviewWidget->buildConfirmedQueue();
        if (!cfg.targets.isEmpty())
        {
            for (int i = 0; i < displayQueue.size(); ++i)
            {
                if (displayQueue[i].target.accountUrl.startsWith(QLatin1Char('(')))
                {
                    displayQueue[i].target = cfg.targets[i % cfg.targets.size()];
                }
            }
            PanePublishing::applySpacingSchedule(displayQueue, cfg);
        }
        for (auto &it : displayQueue)
        {
            it.status = tr("Preview");
        }
    }
    else if (jobRunningOrFinishedWithQueue)
    {
        displayQueue = m_panePublishing->activeQueue();
    }

    m_publishQueueTable->setRowCount(displayQueue.size());
    for (int i = 0; i < displayQueue.size(); ++i)
    {
        const auto &it = displayQueue[i];
        const auto makeCell = [&](const QString &txt) {
            auto *cell = new QTableWidgetItem(txt);
            if (it.status == QLatin1String("Published") || it.status == QLatin1String("Dry-run OK"))
            {
                cell->setForeground(QColor(QStringLiteral("#8fd19e")));
            }
            else if (it.status == QLatin1String("Failed") || it.status == QLatin1String("Stopped"))
            {
                cell->setForeground(QColor(QStringLiteral("#e89774")));
            }
            else if (it.status == QLatin1String("Uploading"))
            {
                cell->setForeground(QColor(QStringLiteral("#58a6ff")));
            }
            return cell;
        };

        const QString timeStr = (i == 0 && it.status == tr("Preview"))
            ? tr("Immediate (+0m)")
            : QStringLiteral("%1 (+%2m)")
                  .arg(it.scheduledAt.toString(QStringLiteral("MM-dd HH:mm")))
                  .arg(it.delayMinutesFromPrevious);
        QString detailNote = it.status;
        if (it.creative.format == QLatin1String("video"))
        {
            detailNote += it.addVideoText
                ? tr(" · Aa [%1]: \"%2\"")
                      .arg(PanePublishing::videoTextStyleShortTag(it.videoTextStyle),
                           it.videoOverlayText.left(40))
                : tr(" · Clean video");
        }

        m_publishQueueTable->setItem(i, 0, makeCell(QString::number(i + 1)));
        m_publishQueueTable->setItem(i, 1, makeCell(it.creative.shortCode));
        m_publishQueueTable->setItem(i, 2, makeCell(it.creative.format));
        m_publishQueueTable->setItem(i, 3, makeCell(it.creative.productSummary));
        m_publishQueueTable->setItem(i, 4, makeCell(QStringLiteral("%1 — %2")
            .arg(it.target.platformName, it.target.accountUrl)));
        m_publishQueueTable->setItem(i, 5, makeCell(timeStr));
        m_publishQueueTable->setItem(i, 6, makeCell(detailNote));
    }
}

void PaneWorkflow::_updateStep3Buttons()
{
    const bool running = m_panePublishing && m_panePublishing->isJobRunning();
    const auto cfg = _currentPublishConfig();
    const int selectedPosts = m_reviewWidget ? m_reviewWidget->checkedCount() : 0;

    if (m_btnStartPublish)
    {
        m_btnStartPublish->setEnabled(!running && !cfg.targets.isEmpty() && selectedPosts > 0);
        m_btnStartPublish->setText(selectedPosts > 0
            ? tr("🚀 Start Auto-Publish Job (%1 post(s))").arg(selectedPosts)
            : tr("Select creatives in Step 2 first"));
    }
    if (m_btnStopPublish)
    {
        m_btnStopPublish->setEnabled(running);
    }
    if (m_btnPublishNextNow)
    {
        m_btnPublishNextNow->setEnabled(m_panePublishing && m_panePublishing->canPublishNextNow());
    }
}

void PaneWorkflow::startStep3PublishJob()
{
    if (!m_panePublishing || !m_reviewWidget || m_panePublishing->isJobRunning())
    {
        return;
    }
    const auto cfg = _currentPublishConfig();
    if (cfg.targets.isEmpty())
    {
        QMessageBox::information(this, tr("Select target account"),
            tr("Check at least one supported social media account (e.g. TikTok) first."));
        return;
    }

    QList<PanePublishing::ScheduledItem> confirmed = m_reviewWidget->buildConfirmedQueue();
    if (confirmed.isEmpty())
    {
        QMessageBox::information(this, tr("No creatives selected"),
            tr("Check at least one creative in Step 2 before starting the Auto-Publish job."));
        return;
    }
    for (int i = 0; i < confirmed.size(); ++i)
    {
        bool validTarget = false;
        for (const auto &t : cfg.targets)
        {
            if (t.accountUrl == confirmed[i].target.accountUrl && t.platformId == confirmed[i].target.platformId)
            {
                validTarget = true;
                break;
            }
        }
        if (!validTarget)
        {
            confirmed[i].target = cfg.targets[i % cfg.targets.size()];
        }
    }

    _saveStep3Settings();
    m_showingCompletedPublishQueue = true;
    m_publishLogConsole->clear();
    m_panePublishing->startPlannedPublishJob(cfg, confirmed);
    _refreshStep3QueueTable();
    _updateStep3Buttons();
    _updateHeaderAndStepBadges();
}

void PaneWorkflow::_updateHeaderAndStepBadges()
{
    int eligibleProducts = 0;
    int approvedRefs = 0;
    if (m_store)
    {
        const QString curAccount = _selectedAccountId();
        const QJsonObject profile = m_store->profile(curAccount);
        for (const QJsonValue &v : m_store->products())
        {
            const QJsonObject p = v.toObject();
            if (ResearchStore::productRestriction(p, profile).isEmpty()
                && !jsonText(p, "localImage").isEmpty()
                && QFileInfo::exists(jsonText(p, "localImage")))
            {
                ++eligibleProducts;
            }
        }
        for (const QJsonValue &v : m_store->references())
        {
            const QJsonObject r = v.toObject();
            if (jsonText(r, "accountId") == curAccount && jsonText(r, "status") == QLatin1String("approved"))
            {
                ++approvedRefs;
            }
        }
    }

    const int unpublishedCount = m_panePublishing
        ? m_panePublishing->collectEligibleCreatives(m_spinMaxDaysOld ? m_spinMaxDaysOld->value() : 10, true).size()
        : 0;
    const int reviewedChecked = m_reviewWidget ? m_reviewWidget->checkedCount() : 0;

    if (m_headerSummaryLabel)
    {
        m_headerSummaryLabel->setText(
            tr("%1 eligible product(s) · %2 approved inspiration(s) · %3 unpublished creative(s)")
                .arg(eligibleProducts).arg(approvedRefs).arg(unpublishedCount));
    }
    if (m_btnStep1)
    {
        m_btnStep1->setText(tr("1. Pick Products (%1 checked)").arg(m_checkedProductIds.size()));
    }
    if (m_btnStep2)
    {
        m_btnStep2->setText(m_step2Initialized
            ? tr("2. Review Generated Content (%1/%2 selected)").arg(reviewedChecked).arg(m_reviewWidget->totalCount())
            : tr("2. Review Generated Content (%1 ready)").arg(unpublishedCount));
    }
    if (m_btnStep3)
    {
        if (m_panePublishing && m_panePublishing->isJobRunning())
        {
            m_btnStep3->setText(tr("3. Auto-Publish Jobs (● Running)"));
        }
        else
        {
            m_btnStep3->setText(tr("3. Start Auto-Publish Jobs"));
        }
    }
    _updateStep1SummaryAndButtons();
}
