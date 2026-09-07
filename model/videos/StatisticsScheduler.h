#ifndef STATISTICSSCHEDULER_H
#define STATISTICSSCHEDULER_H

#include <QObject>
#include <QTimer>

#include <QCoro/QCoroTask>

#include "TableVideos.h"

// Opportunistic snapshot fetcher: shortly after startup and then hourly, it
// asks TableVideos which publications are overdue and fetches their public
// stats. No fetch is ever "missed" — whenever a check runs, everything
// overdue is caught up, and comparison metrics interpolate between whatever
// snapshot ages exist.
//
// Platforms are resolved through ALL_SOCIAL_ACCOUNTS() and their declared
// capabilities only: a publication on an unknown platform (free-form label,
// or a platform whose class does not fetch per-post stats yet) is silently
// skipped and will be picked up once the platform class exists.
class StatisticsScheduler : public QObject
{
    Q_OBJECT

public:
    explicit StatisticsScheduler(TableVideos *videos, QObject *parent = nullptr);

    bool isRunning() const;

public slots:
    // Fetches everything currently due (public source only — automatic runs
    // must never pop a login browser window in the user's face).
    void checkNow();

    // Fetches every publication of one video right now, due or not; with
    // includeAnalytics, also queries the platforms' Analytics source (may
    // open a visible browser waiting for an interactive login).
    void fetchVideoNow(const QUuid &videoId, bool includeAnalytics);

signals:
    void runStarted();
    void runFinished(int snapshotsAdded, const QStringList &errors);

private:
    TableVideos *m_videos;
    QTimer m_timer;
    bool m_running = false;
    QCoro::Task<void> m_task;

    QCoro::Task<void> _fetchDue();
    QCoro::Task<void> _fetchVideo(QUuid videoId, bool includeAnalytics);
    // Fetches one publication from one source; returns true if a snapshot
    // was recorded. Appends to errors on failure.
    QCoro::Task<bool> _fetchOne(QUuid videoId, QString platformId, QString postUrl,
                                AbstractSocialAccount::StatSource source,
                                QStringList &errors);
};

#endif // STATISTICSSCHEDULER_H
