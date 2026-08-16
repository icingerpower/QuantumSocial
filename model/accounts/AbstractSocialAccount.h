#ifndef ABSTRACTSOCIALACCOUNT_H
#define ABSTRACTSOCIALACCOUNT_H

#include <QMap>
#include <QString>

#include <QCoro/QCoroTask>

// Base class for the social platforms supported by the app.
//
// Subclasses register themselves via DECLARE_SOCIAL_ACCOUNT() and are
// accessible through ALL_SOCIAL_ACCOUNTS(), keyed by getId().
class AbstractSocialAccount
{
public:
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

private:
    static QMap<QString, AbstractSocialAccount *> &getSocialAccounts();
};

#define DECLARE_SOCIAL_ACCOUNT(NEW_CLASS)                                        \
    NEW_CLASS instance##NEW_CLASS;                                               \
    AbstractSocialAccount::Recorder recorder##NEW_CLASS{&instance##NEW_CLASS};

#endif // ABSTRACTSOCIALACCOUNT_H
