#ifndef RESEARCHSTORE_H
#define RESEARCHSTORE_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>

class ResearchStore : public QObject
{
    Q_OBJECT
public:
    explicit ResearchStore(const QString &workspace, QObject *parent = nullptr);
    QJsonObject profile(const QString &accountId) const;
    bool setProfile(const QString &accountId, const QJsonObject &profile);
    QJsonArray products() const { return m_data.value("products").toArray(); }
    QString lastProductDirectory() const { return m_data.value("lastProductDirectory").toString(); }
    QJsonArray references() const { return m_data.value("references").toArray(); }
    QJsonObject product(const QString &id) const;
    QJsonObject reference(const QString &id) const;
    bool replaceProducts(const QJsonArray &products, const QString &sourceDirectory);
    QString addReference(const QString &accountId, const QString &url, const QString &title,
                         const QString &localMedia = {});
    bool updateReference(const QString &id, const QJsonObject &fields);
    bool attachMedia(const QString &id, const QString &localMedia, bool resetStatus = true);
    bool removeReferences(const QStringList &ids);
    bool review(const QStringList &ids, const QString &status, const QString &reason);
    int autoDiscardReferences(const QHash<QString, QString> &reasonsByRefId);
    bool reloadIfChangedOnDisk();
    bool isRejectedUrl(const QString &accountId, const QString &url) const;
    QStringList excludedSpyUrls(const QString &accountId) const;
    QJsonObject spyContextPayload(const QString &accountId, const QString &platformId = {}) const;
    QJsonArray influencers(const QString &accountId = {}, const QString &platformId = {}) const;
    QJsonObject influencer(const QString &id) const;
    QString addInfluencer(const QString &accountId, const QString &platformId,
                          const QString &handleOrUrl, const QString &notes = {});
    bool removeInfluencer(const QString &id);
    bool recordInfluencerActivity(const QString &id, bool checked, bool picked,
                                  int newlyPickedCount = 0, const QString &timestampOverride = {});
    int recordInfluencerPickByAuthorOrUrl(const QString &accountId, const QString &platformId,
                                          const QString &authorHandle, const QString &postOrProfileUrl,
                                          bool checked = false, const QString &timestampOverride = {});
    bool recordAssessment(const QString &referenceId, const QJsonObject &assessment);
    bool recordSpySession(const QString &accountId, const QJsonObject &session);
    QJsonObject lastSpySession(const QString &accountId) const;
    QString mediaPath(const QJsonObject &reference) const;
    QString lastError() const { return m_error; }
    QDir directory() const { return m_dir; }
    static QString canonicalUrl(const QString &url);
    static QString normalizeInfluencerHandle(const QString &handleOrUrl, const QString &platformId = {});
    static QString buildInfluencerUrl(const QString &platformId, const QString &handleOrUrl);
    static QString productRestriction(const QJsonObject &product, const QJsonObject &profile);

signals:
    void changed();

private:
    QDir m_dir;
    QJsonObject m_data;
    QByteArray m_revision;
    QString m_error;
    bool m_readOnly = false;
    bool save(const QJsonObject &next);
};

#endif
