#include "AbstractSocialAccount.h"

AbstractSocialAccount::Recorder::Recorder(AbstractSocialAccount *account)
{
    getSocialAccounts().insert(account->getId(), account);
}

const QMap<QString, AbstractSocialAccount *> &AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS()
{
    return getSocialAccounts();
}

QMap<QString, AbstractSocialAccount *> &AbstractSocialAccount::getSocialAccounts()
{
    static QMap<QString, AbstractSocialAccount *> map;
    return map;
}
