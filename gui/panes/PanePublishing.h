#ifndef PANEPUBLISHING_H
#define PANEPUBLISHING_H

#include <QCoro/QCoroTask>

#include <QDateTime>
#include <QList>
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

    struct PublishJobConfig
    {
        QList<AccountTarget> targets;
        int maxDaysOld = 10; // 0 = no limit; >0 = created at most maxDaysOld days ago (<= maxDaysOld)
        int postsPerDay = 3;
        int totalPosts = 6;
        bool useSystemChromeProfile = true;
        bool muteOriginalAudio = true;
        bool useSuggestedSound = true;
        bool dryRun = false;
        bool reviewBeforePublish = false; // Opens interactive candidate preview/selection/order dialog after AI planning
        AbstractCli *plannerCli = nullptr;
        QString uploadUrlOverride; // Offline HTML fixture override for automated tests
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
    bool startPublishJob(const PublishJobConfig &config);

public slots:
    void openAutoPublishDialog();
    void stopJob();
    void publishNextNow();
    void reconnectBrowserSession(const QString &accountUrl = QString{});
    void openDebugLogsFolder();

private:
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
        QString aiReason;
        QString status; // "Queued", "Uploading", "Published", "Dry-run OK", "Failed", "Stopped"
    };

    QList<EligibleCreative> _collectEligibleCreatives(int maxDaysOld,
                                                      bool skipSingleImages = true,
                                                      int *outOlderExcluded = nullptr,
                                                      int *outSingleImagesExcluded = nullptr) const;
    bool _reviewPlannedSelectionDialog(const PublishJobConfig &config,
                                       const QList<EligibleCreative> &allCandidates,
                                       int baseIntervalMins,
                                       const QString &experimentSummaryLine);
    QCoro::Task<void> _runJobCoroutine(PublishJobConfig config, QList<EligibleCreative> candidates);
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
