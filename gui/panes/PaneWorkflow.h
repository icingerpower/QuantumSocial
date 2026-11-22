#ifndef PANEWORKFLOW_H
#define PANEWORKFLOW_H

#include "PanePublishing.h"

#include <QCoro/QCoroTask>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

#include <memory>

class AbstractCli;
class FavoriteHooks;
class PaneGeneration;
class PaneResearch;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class ResearchStore;
class TableAccounts;
class TableVideos;
class WidgetCreativeReview;

class PaneWorkflow : public QWidget
{
    Q_OBJECT

public:
    explicit PaneWorkflow(TableAccounts *accounts,
                          ResearchStore *researchStore,
                          PaneResearch *paneResearch,
                          PaneGeneration *paneGeneration,
                          PanePublishing *panePublishing,
                          TableVideos *videos,
                          const QList<AbstractCli *> &availableClis,
                          FavoriteHooks *favoriteHooks = nullptr,
                          QWidget *parent = nullptr);
    ~PaneWorkflow() override;

    int currentStep() const;
    void setCurrentStep(int stepIndex);

    void setUploadUrlOverride(const QString &url) { m_uploadUrlOverride = url; }
    bool deleteCreativeByShortCode(const QString &shortCode, bool askConfirmation = true);

signals:
    void openResearchTabRequested();
    void openAccountsTabRequested();

public slots:
    void refreshAll();
    void refreshStep1Products();
    void refreshStep2Candidates(bool preserveExistingSelection = false);
    void refreshStep3AccountsAndPreview();
    void runAiSelectInStep2();
    void startStep1Generation();
    void startStep3PublishJob();

private:
    QWidget *_buildStep1Page();
    QWidget *_buildStep2Page();
    QWidget *_buildStep3Page();

    QString _selectedAccountId() const;
    void _syncResearchAccountSelection();
    QStringList _checkedStep1ProductIds() const;
    void _updateStep1SummaryAndButtons();
    void _updateStep1ProductPreview();
    void _saveStep1Settings() const;

    PanePublishing::PublishJobConfig _currentPublishConfig() const;
    void _saveStep3Settings() const;
    void _refreshStep3QueueTable();
    void _updateStep3Buttons();
    void _updateHeaderAndStepBadges();
    QCoro::Task<void> _runAiSelectCoroutine();

    TableAccounts *m_accounts = nullptr;
    ResearchStore *m_store = nullptr;
    PaneResearch *m_paneResearch = nullptr;
    PaneGeneration *m_paneGeneration = nullptr;
    PanePublishing *m_panePublishing = nullptr;
    TableVideos *m_videos = nullptr;
    FavoriteHooks *m_favoriteHooks = nullptr;
    QList<AbstractCli *> m_clis;

    // Top persistent header & stepper
    QComboBox *m_accountCombo = nullptr;
    QLabel *m_headerSummaryLabel = nullptr;
    QPushButton *m_btnStep1 = nullptr;
    QPushButton *m_btnStep2 = nullptr;
    QPushButton *m_btnStep3 = nullptr;
    QStackedWidget *m_stepStack = nullptr;

    // Step 1: Pick Products & Generate
    QLineEdit *m_prodSearchEdit = nullptr;
    QCheckBox *m_chkOnlySelectedProducts = nullptr;
    QPushButton *m_btnCheckAllProducts = nullptr;
    QPushButton *m_btnClearProducts = nullptr;
    QLabel *m_selectedProductsLabel = nullptr;
    QListWidget *m_productList = nullptr;
    QLabel *m_prodPreviewImg = nullptr;
    QLabel *m_prodPreviewCaption = nullptr;
    QLabel *m_inspirationsInfoLabel = nullptr;
    QSpinBox *m_spinGenPerProduct = nullptr;
    QCheckBox *m_chkFormatVideo = nullptr;
    QComboBox *m_comboVideoGen = nullptr;
    QCheckBox *m_chkFormatCarousel = nullptr;
    QCheckBox *m_chkCarouselOverlay = nullptr;
    QCheckBox *m_chkFormatImage = nullptr;
    QComboBox *m_comboAspectFormat = nullptr;
    QWidget *m_genProgressBox = nullptr;
    QLabel *m_genStatusLabel = nullptr;
    QProgressBar *m_genProgressBar = nullptr;
    QPushButton *m_btnCancelGen = nullptr;
    QPlainTextEdit *m_genLogConsole = nullptr;
    QPushButton *m_btnSkipToStep2 = nullptr;
    QPushButton *m_btnGenerateAndNext = nullptr;
    QSet<QString> m_checkedProductIds;
    bool m_triggeredBatchFromWorkflow = false;

    // Step 2: Review Generated Content
    QSpinBox *m_spinMaxDaysOld = nullptr;
    QSpinBox *m_spinTargetPosts = nullptr;
    QComboBox *m_comboPlannerCli = nullptr;
    QPushButton *m_btnAiSelect = nullptr;
    QPushButton *m_btnRefreshCandidates = nullptr;
    WidgetCreativeReview *m_reviewWidget = nullptr;
    QPushButton *m_btnBackToStep1 = nullptr;
    QPushButton *m_btnContinueToStep3 = nullptr;
    bool m_step2Initialized = false;
    bool m_aiPlanningInProgress = false;
    std::shared_ptr<bool> m_aiPlanCancelFlag;
    std::unique_ptr<QCoro::Task<void>> m_aiPlanTask;

    // Step 3: Start Auto-Publish Jobs
    QListWidget *m_publishAccountsList = nullptr;
    QPushButton *m_btnReconnectBrowser = nullptr;
    QSpinBox *m_spinPostsPerDay = nullptr;
    QRadioButton *m_radioDaytime12h = nullptr;
    QRadioButton *m_radioEven24h = nullptr;
    QRadioButton *m_radioFixedInterval = nullptr;
    QSpinBox *m_spinFixedMinutes = nullptr;
    QCheckBox *m_chkSuggestedSound = nullptr;
    QCheckBox *m_chkDryRun = nullptr;
    QPushButton *m_btnStartPublish = nullptr;
    QPushButton *m_btnPublishNextNow = nullptr;
    QPushButton *m_btnStopPublish = nullptr;
    QLabel *m_publishStatusLabel = nullptr;
    QTableWidget *m_publishQueueTable = nullptr;
    QPlainTextEdit *m_publishLogConsole = nullptr;
    QPushButton *m_btnBackToStep2 = nullptr;

    QString m_uploadUrlOverride;
    bool m_showingCompletedPublishQueue = false;
    bool m_updatingUi = false;
};

#endif // PANEWORKFLOW_H
