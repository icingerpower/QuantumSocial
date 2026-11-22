#ifndef PANEPUBLISHING_H
#define PANEPUBLISHING_H

#include <QCoro/QCoroTask>

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QList>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QWidget>

#include <memory>

class AbstractCli;
class FavoriteHooks;
class PreferredHashtags;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;
class TableAccounts;
class TableProjects;
class TableVideos;

class PanePublishing : public QWidget
{
    Q_OBJECT

public:
    struct AccountTarget
    {
        QString platformId;
        QString platformName;
        QString accountUrl;
        qint64 followers = -1;
    };

    enum class SpacingMode
    {
        DaytimeSpread12h, // 12h active daytime spread (720 / postsPerDay min) + ~12h overnight pause
        EvenSpread24h,    // Even 24h spread (1440 / postsPerDay min around the clock)
        FixedInterval     // Fixed interval in minutes between posts for the same account (e.g. 60 min)
    };

    struct PublishJobConfig
    {
        QList<AccountTarget> targets;
        int maxDaysOld = 10; // 0 = no limit; >0 = created at most maxDaysOld days ago (<= maxDaysOld)
        int postsPerDay = 3;
        int totalPosts = 6;
        SpacingMode spacingMode = SpacingMode::DaytimeSpread12h;
        int fixedIntervalMinutes = 60;
        bool useSystemChromeProfile = true;
        bool muteOriginalAudio = true;
        bool useSuggestedSound = true;
        bool dryRun = false;
        bool reviewBeforePublish = false; // Opens interactive candidate preview/selection/order dialog after AI planning
        AbstractCli *plannerCli = nullptr;
        QString uploadUrlOverride; // Offline HTML fixture override for automated tests
    };

    struct EligibleCreative
    {
        QUuid videoId;
        QUuid projectId;
        int projectRow = -1;
        QString shortCode;
        int ageDays = 0;
        QString format; // "video", "carousel", "image"
        QStringList mediaPaths;
        QString previewImagePath;
        QString projectName;
        QString productSummary;
        QString hook;
        QString caption;
        QString hookCategory;
        QString strategy;
        QString referenceUrl;
        QString dnaAccountId;
    };

    struct ScheduledItem
    {
        EligibleCreative creative;
        AccountTarget target;
        int delayMinutesFromPrevious = 0;
        QDateTime scheduledAt;
        bool addVideoText = false;
        bool aiSuggestedVideoText = false;
        QString videoOverlayText;
        QString videoTextStyle;
        QString aiReason;
        QString status; // "Queued", "Uploading", "Published", "Dry-run OK", "Failed", "Stopped"
    };

    explicit PanePublishing(TableAccounts *accounts,
                            TableProjects *projects,
                            TableVideos *videos,
                            const QList<AbstractCli *> &availableClis,
                            QWidget *parent = nullptr,
                            PreferredHashtags *hashtags = nullptr,
                            FavoriteHooks *favoriteHooks = nullptr);
    ~PanePublishing() override;

    bool isJobRunning() const { return m_jobActive; }
    bool canPublishNextNow() const;
    const QList<ScheduledItem> &activeQueue() const { return m_queue; }
    QString statusText() const;
    QString logText() const;
    TableProjects *projectsModel() const { return m_projects; }

    bool startPublishJob(const PublishJobConfig &config);
    bool startPlannedPublishJob(const PublishJobConfig &config,
                                const QList<ScheduledItem> &confirmedQueue);

    QList<EligibleCreative> collectEligibleCreatives(int maxDaysOld,
                                                     bool skipSingleImages = true,
                                                     int *outOlderExcluded = nullptr,
                                                     int *outSingleImagesExcluded = nullptr) const;
    QCoro::Task<QList<ScheduledItem>> buildAiPlannedQueue(
        PublishJobConfig config,
        QList<EligibleCreative> candidates,
        QString *outExperimentSummary = nullptr,
        std::shared_ptr<bool> cancelFlag = nullptr);

    static QList<ScheduledItem> buildDeterministicDefaultQueue(
        const PublishJobConfig &config,
        const QList<EligibleCreative> &candidates,
        TableVideos *videos = nullptr,
        FavoriteHooks *favoriteHooks = nullptr,
        QString *outExperimentSummary = nullptr);
    static int baseIntervalMinutesForConfig(const PublishJobConfig &config);
    static void applySpacingSchedule(QList<ScheduledItem> &queue, const PublishJobConfig &config);

    static const QStringList &curatedVideoTextStyleIds();
    static QString normalizeVideoTextStyleId(const QString &raw);
    static QString videoTextStyleDisplayName(const QString &styleId);
    static QString videoTextStyleShortTag(const QString &styleId);
    static QString cleanShortHookForOverlay(const QString &rawHook, const QString &productSummary);
    static QString codeTagFromShortCode(const QString &shortCode);
    static QString resolveCreativePreviewImagePath(const QDir &genDir,
                                                   const QDir &tempDir,
                                                   const QString &format,
                                                   const QStringList &mediaPaths,
                                                   bool allowFfmpegExtract);
    static QPixmap renderCreativeCardPixmap(const QString &imagePath,
                                            const QString &shortCode,
                                            const QString &productSummary,
                                            const QString &badgeText,
                                            const QColor &badgeColor,
                                            const QSize &targetSize,
                                            const QString &overlayPreviewText = QString{},
                                            const QString &overlayStyleId = QString{});

signals:
    void jobStateChanged(bool running);
    void queueUpdated();
    void statusTextChanged(const QString &text);
    void logLineAppended(const QString &line);

public slots:
    void openAutoPublishDialog();
    void stopJob();
    void publishNextNow();
    void reconnectBrowserSession(const QString &accountUrl = QString{});
    void openDebugLogsFolder();

private:
    QList<EligibleCreative> _collectEligibleCreatives(int maxDaysOld,
                                                      bool skipSingleImages = true,
                                                      int *outOlderExcluded = nullptr,
                                                      int *outSingleImagesExcluded = nullptr) const;
    bool _reviewPlannedSelectionDialog(const PublishJobConfig &config,
                                       const QList<EligibleCreative> &allCandidates,
                                       int baseIntervalMins,
                                       const QString &experimentSummaryLine);
    QCoro::Task<int> _syncLivePublicationsForTargets(const PublishJobConfig &config,
                                                     std::shared_ptr<bool> cancelToken);
    QCoro::Task<void> _runJobCoroutine(PublishJobConfig config, QList<EligibleCreative> candidates);
    QCoro::Task<void> _runPlannedJobCoroutine(PublishJobConfig config, QList<ScheduledItem> confirmedQueue);
    void _appendLog(const QString &line);
    void _refreshQueueTable();
    void _updateActionButtons();
    void _scheduleNextTimer();
    void _launchNextQueuedItem();
    QCoro::Task<void> _executeItemCoroutine(int itemIndex);

    TableAccounts *m_accounts = nullptr;
    TableProjects *m_projects = nullptr;
    TableVideos *m_videos = nullptr;
    PreferredHashtags *m_hashtags = nullptr;
    FavoriteHooks *m_favoriteHooks = nullptr;
    QList<AbstractCli *> m_clis;

    QLabel *m_statusLabel = nullptr;
    QPushButton *m_btnNewJob = nullptr;
    QPushButton *m_btnPublishNextNow = nullptr;
    QPushButton *m_btnStopJob = nullptr;
    QPushButton *m_btnReconnect = nullptr;
    QPushButton *m_btnOpenDebugDir = nullptr;
    QPushButton *m_btnClearLogs = nullptr;
    QTableWidget *m_queueTable = nullptr;
    QPlainTextEdit *m_logConsole = nullptr;

    PublishJobConfig m_activeConfig;
    QList<ScheduledItem> m_queue;
    int m_currentIndex = -1;
    bool m_jobActive = false;
    bool m_uploadingNow = false;
    std::shared_ptr<bool> m_cancelFlag;
    QTimer *m_waitTimer = nullptr;
    QTimer *m_countdownUiTimer = nullptr;
    std::unique_ptr<QCoro::Task<void>> m_planningTask;
    std::unique_ptr<QCoro::Task<void>> m_uploadTask;
};

#endif // PANEPUBLISHING_H
