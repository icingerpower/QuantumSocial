#ifndef ABSTRACTSOCIALACCOUNT_H
#define ABSTRACTSOCIALACCOUNT_H

#include <QMap>
#include <QString>

// Base class for the social platforms supported by the app.
//
// Subclasses register themselves via DECLARE_SOCIAL_ACCOUNT() and are
// accessible through ALL_SOCIAL_ACCOUNTS(), keyed by getId().
class AbstractSocialAccount
{
public:
    virtual ~AbstractSocialAccount() = default;

    // Unique id identifying the platform (e.g. "tiktok").
    virtual QString getId() const = 0;

    // Human-readable platform name (e.g. "TikTok").
    virtual QString getName() const = 0;

    // Returns all registered platforms keyed by id.
    static const QMap<QString, AbstractSocialAccount *> &ALL_SOCIAL_ACCOUNTS();

    // Used by DECLARE_SOCIAL_ACCOUNT to register a platform instance at startup.
    class Recorder
    {
    public:
        explicit Recorder(AbstractSocialAccount *account);
    };

private:
    static QMap<QString, AbstractSocialAccount *> &getSocialAccounts();
};

#define DECLARE_SOCIAL_ACCOUNT(NEW_CLASS)                                        \
    NEW_CLASS instance##NEW_CLASS;                                               \
    AbstractSocialAccount::Recorder recorder##NEW_CLASS{&instance##NEW_CLASS};

#endif // ABSTRACTSOCIALACCOUNT_H
