#ifndef ABSTRACTSOCIALACCOUNT_H
#define ABSTRACTSOCIALACCOUNT_H

#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>

#include <QCoro/QCoroTask>

// Base class for the social platforms supported by the app.
//
// Subclasses register themselves via DECLARE_SOCIAL_ACCOUNT() and are
// accessible through ALL_SOCIAL_ACCOUNTS(), keyed by getId(). Everything
// platform-specific (page structure, scraping scripts, which stats exist,
// which stat sources are supported) lives in the subclasses: generic code
// must only ever iterate ALL_SOCIAL_ACCOUNTS() and query capabilities, so
// adding a platform means adding one subclass + its script(s), nothing else.
class AbstractSocialAccount
{
public:
    // Where a statistic was read from. PublicView is what any anonymous
    // visitor sees on the post page; Analytics is the creator dashboard
    // (richer stats, requires being logged in as the owner). Numbers from
    // the two sources are NOT comparable with each other.
    enum class StatSource
    {
        PublicView,
        Analytics,
    };

    // Public stats retrieved for one profile URL. A negative value means the
    // fetch failed or the platform does not publicly expose that stat —
    // callers should leave the previously known value untouched rather than
    // overwrite it with 0.
    struct Statistics
    {
        qint64 followers = -1;
        qint64 views = -1;
        qint64 likes = -1;
        QString errorMessage;
    };

    // Stats retrieved for one published post/video URL. Same convention as
    // Statistics: negative means unavailable (either the fetch failed or the
    // platform/source does not expose that stat).
    struct VideoStatistics
    {
        // Available from the public view on most platforms:
        qint64 views = -1;
        qint64 likes = -1;
        qint64 comments = -1;
        qint64 shares = -1;             // shares/reposts; saves on Pinterest
        // Usually only available from the Analytics source:
        qint64 impressions = -1;        // times shown in feeds
        qint64 watchTimeSeconds = -1;
        double avgWatchPercent = -1.0;  // retention, 0-100
        qint64 followsFromVideo = -1;   // new followers attributed to the post
        QString errorMessage;
    };

    virtual ~AbstractSocialAccount() = default;

    // Unique id identifying the platform (e.g. "tiktok").
    virtual QString getId() const = 0;

    // Human-readable platform name (e.g. "TikTok").
    virtual QString getName() const = 0;

    // Fetches the public stats for one profile URL. Each platform has its
    // own page structure, bot-detection quirks, and set of publicly
    // available stats, so every subclass implements its own scraping logic
    // (typically via runStatsScript() with its own script).
    virtual QCoro::Task<Statistics> fetchStatistics(const QString &url) const = 0;

    // Which stat sources this platform can fetch per-post stats from.
    // Default: none — a platform without per-post support is silently
    // skipped by the snapshot scheduler instead of erroring.
    virtual QSet<StatSource> supportedVideoStatSources() const;

    // Fetches the stats of one published post/video URL from the given
    // source. Default implementation reports the source as unsupported;
    // subclasses override for the sources they declared above.
    virtual QCoro::Task<VideoStatistics> fetchVideoStatistics(
        const QString &postUrl, StatSource source) const;

    // Stable string form of a StatSource, used both in persisted snapshots
    // and as the argv[2] passed to the fetch scripts ("public"/"analytics").
    static QString statSourceKey(StatSource source);
    static StatSource statSourceFromKey(const QString &key);

    // Returns all registered platforms keyed by id.
    static const QMap<QString, AbstractSocialAccount *> &ALL_SOCIAL_ACCOUNTS();

    // Used by DECLARE_SOCIAL_ACCOUNT to register a platform instance at startup.
    class Recorder
    {
    public:
        explicit Recorder(AbstractSocialAccount *account);
    };

protected:
    // Shared infrastructure for subclasses: launches the given Python script
    // (must live next to this file, in model/accounts/) as `<script> <url>`,
    // waits for it to finish (headless-browser scripts can take a while),
    // and parses its stdout as a single JSON line:
    //     {"followers": <int|null>, "views": <int|null>, "likes": <int|null>, "error": <string|null>}
    // The script itself holds all platform-specific scraping knowledge
    // (what to look for on the page, cookies to set, etc.) — this helper
    // only knows how to run a script and read that fixed result shape.
    QCoro::Task<Statistics> runStatsScript(const QString &scriptFileName, const QString &url) const;

    // Same infrastructure for per-post stats: launches the given script as
    // `<script> <postUrl> <public|analytics>` and parses its stdout as one
    // JSON line:
    //     {"views": <int|null>, "likes": <int|null>, "comments": <int|null>,
    //      "shares": <int|null>, "impressions": <int|null>,
    //      "watch_time_seconds": <int|null>, "avg_watch_percent": <float|null>,
    //      "follows": <int|null>, "error": <string|null>}
    QCoro::Task<VideoStatistics> runVideoStatsScript(
        const QString &scriptFileName, const QString &postUrl, StatSource source) const;

private:
    // Runs `python3 <script> <args...>` and parses its stdout as one JSON
    // object. Infrastructure failures (python missing, timeout, bad output)
    // are reported through the same "error" key the scripts use, so callers
    // have a single error path. (Returning a plain QJsonObject rather than a
    // result struct also sidesteps a GCC 13 ICE on aggregate copy-init from
    // co_await expressions.)
    QCoro::Task<QJsonObject> _runJsonScript(
        const QString &scriptFileName, const QStringList &args) const;

    static QMap<QString, AbstractSocialAccount *> &getSocialAccounts();
};

#define DECLARE_SOCIAL_ACCOUNT(NEW_CLASS)                                        \
    NEW_CLASS instance##NEW_CLASS;                                               \
    AbstractSocialAccount::Recorder recorder##NEW_CLASS{&instance##NEW_CLASS};

#endif // ABSTRACTSOCIALACCOUNT_H
