#include "ResearchStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>

namespace {
QByteArray digest(const QByteArray &data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool includes(const QJsonArray &values, const QString &text)
{
    for (const auto &v : values)
        if (!text.isEmpty() && v.toString().trimmed().compare(text.trimmed(), Qt::CaseInsensitive) == 0) return true;
    return false;
}
}

ResearchStore::ResearchStore(const QString &workspace, QObject *parent)
    : QObject(parent), m_dir(QDir(workspace).filePath("research"))
{
    m_data = {{"schemaVersion", 1}, {"profiles", QJsonObject{}}, {"products", QJsonArray{}}, {"references", QJsonArray{}}};
    QFile file(m_dir.filePath("research.json"));
    if (!file.exists()) { m_revision = digest({}); return; }
    if (!file.open(QIODevice::ReadOnly)) { m_readOnly = true; m_error = file.errorString(); return; }
    const auto bytes = file.readAll();
    m_revision = digest(bytes);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    const auto object = doc.object();
    if (error.error != QJsonParseError::NoError || !doc.isObject()
        || object.value("schemaVersion").toInt() != 1 || !object.value("profiles").isObject()
        || !object.value("products").isArray() || !object.value("references").isArray())
    {
        m_readOnly = true;
        m_error = tr("Research data is invalid or uses an unsupported version. The file has been left untouched.");
        return;
    }
    m_data = object;
}

bool ResearchStore::save(const QJsonObject &next)
{
    if (m_readOnly) return false;
    if (!m_dir.mkpath(".")) { m_error = tr("Cannot create research directory."); return false; }
    QLockFile lock(m_dir.filePath("research.lock"));
    if (!lock.tryLock(0)) { m_error = tr("Another process is saving research. Try again."); return false; }
    QFile existing(m_dir.filePath("research.json"));
    if (existing.exists() && !existing.open(QIODevice::ReadOnly)) { m_error = existing.errorString(); return false; }
    const auto current = existing.isOpen() ? existing.readAll() : QByteArray{};
    existing.close();
    if (digest(current) != m_revision)
    {
        m_error = tr("Research changed in another application instance. Reopen this workspace before saving.");
        return false;
    }
    const auto bytes = QJsonDocument(next).toJson();
    QSaveFile file(m_dir.filePath("research.json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    { m_error = file.errorString(); return false; }
    m_data = next;
    m_revision = digest(bytes);
    m_error.clear();
    emit changed();
    return true;
}

QJsonObject ResearchStore::profile(const QString &id) const { return m_data.value("profiles").toObject().value(id).toObject(); }
bool ResearchStore::setProfile(const QString &id, const QJsonObject &profile)
{
    if (QUuid(id).isNull()) { m_error = tr("Choose an account first."); return false; }
    auto next = m_data;
    auto profiles = next.value("profiles").toObject();
    auto copy = profile;
    copy.insert("updatedAt", now());
    profiles.insert(id, copy);
    next.insert("profiles", profiles);
    return save(next);
}
QJsonObject ResearchStore::product(const QString &id) const
{
    for (const auto &v : products()) if (v.toObject().value("id").toString() == id) return v.toObject();
    return {};
}
QJsonObject ResearchStore::reference(const QString &id) const
{
    for (const auto &v : references()) if (v.toObject().value("id").toString() == id) return v.toObject();
    return {};
}
bool ResearchStore::replaceProducts(const QJsonArray &incoming, const QString &sourceDirectory)
{
    auto next = m_data;
    QJsonArray merged;
    for (const auto &v : products())
        if (v.toObject().value("sourceDirectory").toString() != sourceDirectory) merged.append(v);
    for (const auto &v : incoming) merged.append(v);
    next.insert("products", merged);
    next.insert("lastProductDirectory", sourceDirectory);
    return save(next);
}

QString ResearchStore::canonicalUrl(const QString &text)
{
    QUrl url(text.trimmed());
    if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") || url.host().isEmpty()
        || !url.userInfo().isEmpty()) return {};
    QString host = url.host().toLower();
    // Normalize regional/mobile subdomains (e.g. fr.pinterest.com, de.pinterest.com,
    // pinterest.fr, m.tiktok.com, m.youtube.com) so duplicate & rejected checks
    // match across locales and devices.
    static const QRegularExpression pinterestHostRe(
        QStringLiteral("^(?:[a-z0-9-]+\\.)?pinterest\\.[a-z.]+$"));
    if (pinterestHostRe.match(host).hasMatch())
    {
        host = QStringLiteral("www.pinterest.com");
    }
    else if (host == QLatin1String("tiktok.com") || host == QLatin1String("m.tiktok.com"))
    {
        host = QStringLiteral("www.tiktok.com");
    }
    else if (host == QLatin1String("instagram.com") || host == QLatin1String("m.instagram.com"))
    {
        host = QStringLiteral("www.instagram.com");
    }
    else if (host == QLatin1String("youtube.com") || host == QLatin1String("m.youtube.com"))
    {
        host = QStringLiteral("www.youtube.com");
    }
    url.setScheme(QStringLiteral("https"));
    url.setHost(host);
    url.setFragment({});
    QUrlQuery query(url);
    const auto items = query.queryItems();
    for (const auto &item : items)
        if (item.first.startsWith("utm_") || item.first == "igsh" || item.first == "_t" || item.first == "_r") query.removeAllQueryItems(item.first);
    url.setQuery(query);
    QString path = url.path();
    while (path.endsWith('/') && path.size() > 1) path.chop(1);
    url.setPath(path);
    return url.toString(QUrl::FullyEncoded);
}

QString ResearchStore::addReference(const QString &accountId, const QString &url, const QString &title, const QString &media)
{
    if (QUuid(accountId).isNull()) { m_error = tr("Choose an account first."); return {}; }
    const QString normalized = canonicalUrl(url);
    if (normalized.isEmpty() && media.isEmpty()) { m_error = tr("Enter a valid http(s) post URL or select a local image/video."); return {}; }
    if (!url.trimmed().isEmpty() && normalized.isEmpty()) { m_error = tr("The post URL is invalid."); return {}; }
    for (const auto &v : references())
    {
        const auto r = v.toObject();
        if (!normalized.isEmpty()
            && canonicalUrl(r.value("url").toString()) == normalized
            && r.value("accountId").toString() == accountId)
        { m_error.clear(); return r.value("id").toString(); }
    }
    const QString id = uuid();
    QString relative;
    if (!media.isEmpty())
    {
        const QString suffix = QFileInfo(media).suffix().toLower();
        if (!QStringList{"jpg", "jpeg", "png", "webp", "mp4", "mov", "webm"}.contains(suffix))
        { m_error = tr("Choose a JPG, PNG, WebP, MP4, MOV, or WebM file."); return {}; }
        relative = "media/" + id + "." + suffix;
        if (!m_dir.mkpath("media") || !QFile::copy(media, m_dir.filePath(relative)))
        { m_error = tr("Could not copy the reference media into the workspace."); return {}; }
    }
    const QString host = QUrl(normalized).host();
    const QString platform = (host == "tiktok.com" || host.endsWith(".tiktok.com")) ? "tiktok"
        : (host == "pin.it" || host == "pinterest.com" || host.endsWith(".pinterest.com")) ? "pinterest"
        : (host == "instagram.com" || host.endsWith(".instagram.com")) ? "instagram"
        : (host == "youtu.be" || host == "youtube.com" || host.endsWith(".youtube.com")) ? "youtube"
        : "other";
    QJsonObject record{{"id", id}, {"accountId", accountId}, {"url", normalized}, {"title", title.trimmed()},
                       {"platform", platform}, {"media", relative}, {"status", "pending"}, {"createdAt", now()},
                       {"history", QJsonArray{}}, {"assessments", QJsonArray{}}};
    auto next = m_data;
    auto refs = references();
    refs.append(record);
    next.insert("references", refs);
    if (!save(next)) { if (!relative.isEmpty()) QFile::remove(m_dir.filePath(relative)); return {}; }
    return id;
}

bool ResearchStore::updateReference(const QString &id, const QJsonObject &fields)
{
    auto next = m_data;
    auto refs = references();
    for (int i = 0; i < refs.size(); ++i)
    {
        auto r = refs[i].toObject();
        if (r.value("id").toString() != id) continue;
        if (fields.contains("productId") && !fields.value("productId").toString().isEmpty())
        {
            const QString restriction = productRestriction(product(fields.value("productId").toString()), profile(r.value("accountId").toString()));
            if (!restriction.isEmpty()) { m_error = restriction; return false; }
        }
        bool changed = false;
        for (const QString key : {"title", "notes", "productId", "platform", "stats", "usedProducts"})
        {
            if (fields.contains(key) && fields.value(key) != r.value(key))
            {
                r.insert(key, fields.value(key));
                changed = true;
            }
        }
        if (!changed) return true;
        r.insert("updatedAt", now());
        refs[i] = r;
        next.insert("references", refs);
        return save(next);
    }
    m_error = tr("Reference no longer exists."); return false;
}

bool ResearchStore::attachMedia(const QString &id, const QString &localMedia, bool resetStatus)
{
    if (reference(id).isEmpty()) { m_error = tr("Select a reference first."); return false; }
    const QString suffix = QFileInfo(localMedia).suffix().toLower();
    if (!QStringList{"jpg", "jpeg", "png", "webp", "mp4", "mov", "webm"}.contains(suffix))
    { m_error = tr("Choose a supported image or video."); return false; }
    const QString relative = "media/" + uuid() + "." + suffix;
    if (!m_dir.mkpath("media") || !QFile::copy(localMedia, m_dir.filePath(relative)))
    { m_error = tr("Could not copy reference media."); return false; }
    auto refs = references();
    for (int i = 0; i < refs.size(); ++i)
    {
        auto r = refs[i].toObject();
        if (r.value("id").toString() != id) continue;
        if (resetStatus)
        {
            auto history = r.value("history").toArray();
            history.append(QJsonObject{{"status", "pending"}, {"reason", "Reference media changed"},
                {"previousMedia", r.value("media")}, {"at", now()}, {"actor", "human"}});
            r.insert("history", history);
            r.insert("status", "pending");
        }
        r.insert("media", relative);
        r.insert("updatedAt", now());
        refs[i] = r;
        break;
    }
    auto next = m_data;
    next.insert("references", refs);
    if (save(next)) return true;
    QFile::remove(m_dir.filePath(relative));
    return false;
}

bool ResearchStore::removeReferences(const QStringList &ids)
{
    if (ids.isEmpty())
    {
        m_error = tr("Choose references to delete.");
        return false;
    }

    QSet<QString> targetIds(ids.cbegin(), ids.cend());
    QSet<QString> affectedAccounts;
    QSet<QString> remainingAccountIds;
    QSet<QString> removedMediaPaths;
    QSet<QString> retainedMediaPaths;

    QJsonArray remaining;
    int removedCount = 0;
    for (const auto &v : references())
    {
        const auto r = v.toObject();
        const QString id = r.value("id").toString();
        const QString acc = r.value("accountId").toString();
        const QString path = mediaPath(r);
        if (targetIds.contains(id))
        {
            ++removedCount;
            if (!acc.isEmpty())
            {
                affectedAccounts.insert(acc);
            }
            if (!path.isEmpty())
            {
                removedMediaPaths.insert(path);
            }
        }
        else
        {
            remaining.append(r);
            if (!acc.isEmpty())
            {
                remainingAccountIds.insert(acc);
            }
            if (!path.isEmpty())
            {
                retainedMediaPaths.insert(path);
            }
        }
    }

    if (removedCount == 0)
    {
        m_error = tr("Selected references no longer exist.");
        return false;
    }

    auto next = m_data;
    next.insert("references", remaining);

    // If all references for an account were deleted, clear its last spy banner too
    auto sessions = next.value("spySessions").toObject();
    bool sessionsChanged = false;
    for (const QString &acc : affectedAccounts)
    {
        if (!remainingAccountIds.contains(acc) && sessions.contains(acc))
        {
            sessions.remove(acc);
            sessionsChanged = true;
        }
    }
    if (sessionsChanged)
    {
        next.insert("spySessions", sessions);
    }

    if (!save(next))
    {
        return false;
    }

    for (const QString &path : removedMediaPaths)
    {
        if (!retainedMediaPaths.contains(path))
        {
            QFile::remove(path);
            QFile::remove(path + QStringLiteral(".preview.png"));
        }
    }
    return true;
}

bool ResearchStore::review(const QStringList &ids, const QString &status, const QString &reason)
{
    if (!QStringList{"pending", "approved", "rejected"}.contains(status) || ids.isEmpty())
    { m_error = tr("Choose references and a valid review decision."); return false; }
    for (const auto &id : ids)
        if (reference(id).isEmpty()) { m_error = tr("A selected reference no longer exists."); return false; }
    auto next = m_data;
    auto rejectedMap = next.value("rejectedUrls").toObject();
    auto refs = references();
    for (int i = 0; i < refs.size(); ++i)
    {
        auto r = refs[i].toObject();
        if (!ids.contains(r.value("id").toString())) continue;
        auto history = r.value("history").toArray();
        history.append(QJsonObject{{"status", status}, {"reason", reason.trimmed()}, {"at", now()}, {"actor", "human"}});
        r.insert("history", history);
        r.insert("status", status);
        r.insert("reviewReason", reason.trimmed());
        r.insert("reviewedAt", now());
        refs[i] = r;

        const QString acc = r.value("accountId").toString();
        const QString normUrl = canonicalUrl(r.value("url").toString());
        if (!acc.isEmpty() && !normUrl.isEmpty())
        {
            QStringList accRejected;
            for (const auto &v : rejectedMap.value(acc).toArray())
            {
                const QString u = canonicalUrl(v.toString());
                if (!u.isEmpty() && !accRejected.contains(u))
                {
                    accRejected << u;
                }
            }
            if (status == QLatin1String("rejected"))
            {
                if (!accRejected.contains(normUrl))
                {
                    accRejected << normUrl;
                }
            }
            else
            {
                accRejected.removeAll(normUrl);
            }
            QJsonArray updatedArr;
            for (const QString &u : accRejected)
            {
                updatedArr.append(u);
            }
            rejectedMap.insert(acc, updatedArr);
        }
    }
    next.insert("references", refs);
    next.insert("rejectedUrls", rejectedMap);
    return save(next);
}

bool ResearchStore::isRejectedUrl(const QString &accountId, const QString &url) const
{
    const QString norm = canonicalUrl(url);
    if (accountId.isEmpty() || norm.isEmpty())
    {
        return false;
    }
    for (const auto &v : m_data.value("rejectedUrls").toObject().value(accountId).toArray())
    {
        if (canonicalUrl(v.toString()) == norm)
        {
            return true;
        }
    }
    for (const auto &v : references())
    {
        const auto r = v.toObject();
        if (r.value("accountId").toString() == accountId
            && r.value("status").toString() == QLatin1String("rejected")
            && canonicalUrl(r.value("url").toString()) == norm)
        {
            return true;
        }
    }
    return false;
}

QStringList ResearchStore::excludedSpyUrls(const QString &accountId) const
{
    QSet<QString> seen;
    if (accountId.isEmpty())
    {
        return {};
    }
    for (const auto &v : m_data.value("rejectedUrls").toObject().value(accountId).toArray())
    {
        const QString u = canonicalUrl(v.toString());
        if (!u.isEmpty())
        {
            seen.insert(u);
        }
    }
    for (const auto &v : references())
    {
        const auto r = v.toObject();
        if (r.value("accountId").toString() == accountId)
        {
            const QString u = canonicalUrl(r.value("url").toString());
            if (!u.isEmpty())
            {
                seen.insert(u);
            }
        }
    }
    return seen.values();
}

QJsonObject ResearchStore::spyContextPayload(const QString &accountId, const QString &platformId) const
{
    QSet<QString> savedSet;
    QSet<QString> rejectedSet;
    if (!accountId.isEmpty())
    {
        for (const auto &v : m_data.value("rejectedUrls").toObject().value(accountId).toArray())
        {
            const QString u = canonicalUrl(v.toString());
            if (!u.isEmpty())
            {
                rejectedSet.insert(u);
            }
        }
        for (const auto &v : references())
        {
            const auto r = v.toObject();
            if (r.value("accountId").toString() != accountId)
            {
                continue;
            }
            const QString u = canonicalUrl(r.value("url").toString());
            if (u.isEmpty())
            {
                continue;
            }
            if (r.value("status").toString() == QLatin1String("rejected"))
            {
                rejectedSet.insert(u);
            }
            else
            {
                savedSet.insert(u);
            }
        }
    }
    for (const QString &rj : rejectedSet)
    {
        savedSet.remove(rj);
    }
    QSet<QString> excludedSet = savedSet;
    excludedSet.unite(rejectedSet);

    QStringList favHandles;
    QStringList favUrls;
    for (const auto &v : influencers(accountId, platformId))
    {
        const auto inf = v.toObject();
        const QString h = inf.value("handle").toString().trimmed();
        const QString u = inf.value("url").toString().trimmed();
        if (!h.isEmpty() && !favHandles.contains(h, Qt::CaseInsensitive))
        {
            favHandles << h;
        }
        if (!u.isEmpty() && !favUrls.contains(u))
        {
            favUrls << u;
        }
    }

    return QJsonObject{
        {QStringLiteral("saved"), QJsonArray::fromStringList(savedSet.values())},
        {QStringLiteral("rejected"), QJsonArray::fromStringList(rejectedSet.values())},
        {QStringLiteral("excluded"), QJsonArray::fromStringList(excludedSet.values())},
        {QStringLiteral("favorite_handles"), QJsonArray::fromStringList(favHandles)},
        {QStringLiteral("favorite_urls"), QJsonArray::fromStringList(favUrls)}
    };
}

QString ResearchStore::normalizeInfluencerHandle(const QString &handleOrUrl, const QString &platformId)
{
    QString raw = handleOrUrl.trimmed();
    if (raw.isEmpty())
    {
        return {};
    }
    if (!raw.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        && !raw.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)
        && raw.contains(QLatin1Char('/'))
        && (raw.contains(QLatin1String("tiktok."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("pinterest."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("instagram."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("youtube."), Qt::CaseInsensitive)))
    {
        raw = QStringLiteral("https://") + raw;
    }

    if (raw.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
    {
        const QUrl fileUrl(raw);
        const QString base = QFileInfo(fileUrl.path()).completeBaseName().trimmed();
        return base.isEmpty() ? QString{} : (QStringLiteral("@") + base);
    }

    if (raw.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
    {
        const QUrl url(raw);
        const QString host = url.host().toLower();
        const QStringList segs = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segs.isEmpty())
        {
            return {};
        }
        const QString seg0 = segs.first().trimmed();
        if (host.contains(QLatin1String("tiktok.")) || platformId == QLatin1String("tiktok"))
        {
            if (seg0.startsWith(QLatin1Char('@')) && seg0.size() > 1)
            {
                return seg0;
            }
            return {};
        }
        if (host.contains(QLatin1String("pinterest.")) || platformId == QLatin1String("pinterest"))
        {
            static const QSet<QString> kReserved{
                QStringLiteral("pin"), QStringLiteral("search"), QStringLiteral("ideas"),
                QStringLiteral("today"), QStringLiteral("explore"), QStringLiteral("login"),
                QStringLiteral("settings"), QStringLiteral("business"), QStringLiteral("_")
            };
            QString clean = seg0;
            if (clean.startsWith(QLatin1Char('@'))) clean.remove(0, 1);
            if (!clean.isEmpty() && !kReserved.contains(clean.toLower()))
            {
                return QStringLiteral("@") + clean;
            }
            return {};
        }
        if (host.contains(QLatin1String("instagram.")) || platformId == QLatin1String("instagram"))
        {
            static const QSet<QString> kReserved{
                QStringLiteral("p"), QStringLiteral("reel"), QStringLiteral("reels"),
                QStringLiteral("explore"), QStringLiteral("stories"), QStringLiteral("accounts"),
                QStringLiteral("direct"), QStringLiteral("tv")
            };
            QString clean = seg0;
            if (clean.startsWith(QLatin1Char('@'))) clean.remove(0, 1);
            if (!clean.isEmpty() && !kReserved.contains(clean.toLower()))
            {
                return QStringLiteral("@") + clean;
            }
            return {};
        }
        if (host.contains(QLatin1String("youtube.")) || platformId == QLatin1String("youtube"))
        {
            if (seg0.startsWith(QLatin1Char('@')) && seg0.size() > 1)
            {
                return seg0;
            }
            if ((seg0 == QLatin1String("c") || seg0 == QLatin1String("user") || seg0 == QLatin1String("channel"))
                && segs.size() >= 2 && !segs.at(1).trimmed().isEmpty())
            {
                return QStringLiteral("@") + segs.at(1).trimmed();
            }
            return {};
        }
        QString fallback = seg0;
        if (fallback.startsWith(QLatin1Char('@'))) fallback.remove(0, 1);
        return fallback.isEmpty() ? QString{} : (QStringLiteral("@") + fallback);
    }

    while (raw.startsWith(QLatin1Char('@')) || raw.startsWith(QLatin1Char('/')))
    {
        raw.remove(0, 1);
    }
    while (raw.endsWith(QLatin1Char('/')))
    {
        raw.chop(1);
    }
    raw = raw.trimmed();
    static const QRegularExpression kValidHandleRe(QStringLiteral("^[A-Za-z0-9._-]{1,80}$"));
    if (!kValidHandleRe.match(raw).hasMatch())
    {
        return {};
    }
    return QStringLiteral("@") + raw;
}

QString ResearchStore::buildInfluencerUrl(const QString &platformId, const QString &handleOrUrl)
{
    QString raw = handleOrUrl.trimmed();
    if (raw.isEmpty())
    {
        return {};
    }
    if (raw.startsWith(QLatin1String("file://"), Qt::CaseInsensitive))
    {
        return raw;
    }
    if (!raw.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)
        && raw.contains(QLatin1Char('/'))
        && (raw.contains(QLatin1String("tiktok."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("pinterest."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("instagram."), Qt::CaseInsensitive)
            || raw.contains(QLatin1String("youtube."), Qt::CaseInsensitive)))
    {
        raw = QStringLiteral("https://") + raw;
    }

    if (raw.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || raw.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
    {
        const QString norm = canonicalUrl(raw);
        if (norm.isEmpty())
        {
            return {};
        }
        QUrl u(norm);
        const QString host = u.host().toLower();
        const QStringList segs = u.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (host.contains(QLatin1String("tiktok.")) && segs.size() >= 2 && segs.first().startsWith(QLatin1Char('@'))
            && (segs.at(1) == QLatin1String("video") || segs.at(1) == QLatin1String("photo")))
        {
            return QStringLiteral("https://www.tiktok.com/") + segs.first();
        }
        return norm;
    }

    const QString handle = normalizeInfluencerHandle(raw, platformId);
    if (handle.isEmpty())
    {
        return {};
    }
    const QString bare = handle.mid(1);
    const QString plat = platformId.trimmed().toLower();
    if (plat == QLatin1String("tiktok"))
    {
        return QStringLiteral("https://www.tiktok.com/@") + bare;
    }
    if (plat == QLatin1String("pinterest"))
    {
        return QStringLiteral("https://www.pinterest.com/") + bare;
    }
    if (plat == QLatin1String("instagram"))
    {
        return QStringLiteral("https://www.instagram.com/") + bare;
    }
    if (plat == QLatin1String("youtube"))
    {
        return QStringLiteral("https://www.youtube.com/@") + bare + QStringLiteral("/shorts");
    }
    return {};
}

QJsonArray ResearchStore::influencers(const QString &accountId, const QString &platformId) const
{
    const QString plat = platformId.trimmed().toLower();
    QList<QJsonObject> items;
    for (const auto &v : m_data.value("influencers").toArray())
    {
        const auto obj = v.toObject();
        if (!accountId.isEmpty() && obj.value("accountId").toString() != accountId)
        {
            continue;
        }
        if (!plat.isEmpty() && obj.value("platform").toString().toLower() != plat)
        {
            continue;
        }
        items.append(obj);
    }

    const auto effectiveLastSeen = [](const QJsonObject &o) {
        const QString c = o.value("lastCheckedAt").toString();
        const QString p = o.value("lastPickedAt").toString();
        return (c >= p) ? c : p;
    };

    std::stable_sort(items.begin(), items.end(), [&](const QJsonObject &a, const QJsonObject &b) {
        const QString sa = effectiveLastSeen(a);
        const QString sb = effectiveLastSeen(b);
        const bool aNever = sa.isEmpty();
        const bool bNever = sb.isEmpty();
        if (aNever != bNever)
        {
            return aNever; // Never checked/picked appears first!
        }
        if (sa != sb)
        {
            return sa < sb; // Oldest checked/picked date first!
        }
        return a.value("handle").toString().compare(b.value("handle").toString(), Qt::CaseInsensitive) < 0;
    });

    QJsonArray out;
    for (const auto &obj : items)
    {
        out.append(obj);
    }
    return out;
}

QJsonObject ResearchStore::influencer(const QString &id) const
{
    for (const auto &v : m_data.value("influencers").toArray())
    {
        if (v.toObject().value("id").toString() == id)
        {
            return v.toObject();
        }
    }
    return {};
}

QString ResearchStore::addInfluencer(const QString &accountId, const QString &platformId,
                                     const QString &handleOrUrl, const QString &notes)
{
    if (QUuid(accountId).isNull())
    {
        m_error = tr("Choose an account first.");
        return {};
    }
    const QString plat = platformId.trimmed().toLower();
    if (!QStringList{"pinterest", "tiktok", "instagram", "youtube"}.contains(plat))
    {
        m_error = tr("Choose a supported social platform.");
        return {};
    }
    const QString url = buildInfluencerUrl(plat, handleOrUrl);
    QString handle = normalizeInfluencerHandle(handleOrUrl, plat);
    if (handle.isEmpty() && !url.isEmpty())
    {
        handle = normalizeInfluencerHandle(url, plat);
    }
    if (url.isEmpty())
    {
        m_error = tr("Enter a valid @handle or influencer profile URL.");
        return {};
    }
    if (handle.isEmpty())
    {
        handle = url;
    }

    auto list = m_data.value("influencers").toArray();
    for (int i = 0; i < list.size(); ++i)
    {
        auto existing = list[i].toObject();
        if (existing.value("accountId").toString() != accountId
            || existing.value("platform").toString().toLower() != plat)
        {
            continue;
        }
        const bool sameUrl = existing.value("url").toString().compare(url, Qt::CaseInsensitive) == 0;
        const bool sameHandle = !handle.startsWith(QLatin1String("http"))
            && existing.value("handle").toString().compare(handle, Qt::CaseInsensitive) == 0;
        if (sameUrl || sameHandle)
        {
            const QString existingId = existing.value("id").toString();
            if (!notes.trimmed().isEmpty() && existing.value("notes").toString() != notes.trimmed())
            {
                existing.insert("notes", notes.trimmed());
                existing.insert("updatedAt", now());
                list[i] = existing;
                auto next = m_data;
                next.insert("influencers", list);
                if (!save(next))
                {
                    return {};
                }
            }
            m_error.clear();
            return existingId;
        }
    }

    const QString id = uuid();
    QJsonObject record{
        {QStringLiteral("id"), id},
        {QStringLiteral("accountId"), accountId},
        {QStringLiteral("platform"), plat},
        {QStringLiteral("handle"), handle},
        {QStringLiteral("url"), url},
        {QStringLiteral("notes"), notes.trimmed()},
        {QStringLiteral("createdAt"), now()},
        {QStringLiteral("lastCheckedAt"), QString{}},
        {QStringLiteral("lastPickedAt"), QString{}},
        {QStringLiteral("pickedCount"), 0}
    };
    list.append(record);
    auto next = m_data;
    next.insert("influencers", list);
    return save(next) ? id : QString{};
}

bool ResearchStore::removeInfluencer(const QString &id)
{
    if (id.isEmpty())
    {
        m_error = tr("Select an influencer to remove.");
        return false;
    }
    QJsonArray remaining;
    bool removed = false;
    for (const auto &v : m_data.value("influencers").toArray())
    {
        const auto obj = v.toObject();
        if (obj.value("id").toString() == id)
        {
            removed = true;
        }
        else
        {
            remaining.append(obj);
        }
    }
    if (!removed)
    {
        m_error = tr("Influencer no longer exists.");
        return false;
    }
    auto next = m_data;
    next.insert("influencers", remaining);
    return save(next);
}

bool ResearchStore::recordInfluencerActivity(const QString &id, bool checked, bool picked,
                                             int newlyPickedCount, const QString &timestampOverride)
{
    if (id.isEmpty() || (!checked && !picked && newlyPickedCount <= 0))
    {
        return false;
    }
    const QString ts = timestampOverride.trimmed().isEmpty() ? now() : timestampOverride.trimmed();
    auto list = m_data.value("influencers").toArray();
    for (int i = 0; i < list.size(); ++i)
    {
        auto obj = list[i].toObject();
        if (obj.value("id").toString() != id)
        {
            continue;
        }
        if (checked)
        {
            obj.insert("lastCheckedAt", ts);
        }
        if (picked)
        {
            obj.insert("lastPickedAt", ts);
        }
        if (newlyPickedCount > 0)
        {
            obj.insert("pickedCount", obj.value("pickedCount").toInt(0) + newlyPickedCount);
        }
        obj.insert("updatedAt", ts);
        list[i] = obj;
        auto next = m_data;
        next.insert("influencers", list);
        return save(next);
    }
    m_error = tr("Influencer no longer exists.");
    return false;
}

int ResearchStore::recordInfluencerPickByAuthorOrUrl(const QString &accountId, const QString &platformId,
                                                     const QString &authorHandle, const QString &postOrProfileUrl,
                                                     bool checked, const QString &timestampOverride)
{
    if (accountId.isEmpty())
    {
        return 0;
    }
    const QString plat = platformId.trimmed().toLower();
    QString normHandle = normalizeInfluencerHandle(authorHandle, plat);
    if (normHandle.isEmpty() && !postOrProfileUrl.isEmpty())
    {
        normHandle = normalizeInfluencerHandle(postOrProfileUrl, plat);
    }
    const QString normUrl = postOrProfileUrl.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)
        ? postOrProfileUrl.trimmed()
        : canonicalUrl(postOrProfileUrl);

    if (normHandle.isEmpty() && normUrl.isEmpty())
    {
        return 0;
    }

    const QString ts = timestampOverride.trimmed().isEmpty() ? now() : timestampOverride.trimmed();
    auto list = m_data.value("influencers").toArray();
    int matched = 0;
    for (int i = 0; i < list.size(); ++i)
    {
        auto obj = list[i].toObject();
        if (obj.value("accountId").toString() != accountId)
        {
            continue;
        }
        if (!plat.isEmpty() && obj.value("platform").toString().toLower() != plat)
        {
            continue;
        }
        const QString infHandle = obj.value("handle").toString().trimmed();
        const QString infUrl = obj.value("url").toString().trimmed();
        const bool handleMatch = !normHandle.isEmpty()
            && !infHandle.isEmpty()
            && infHandle.compare(normHandle, Qt::CaseInsensitive) == 0;
        const bool urlMatch = !normUrl.isEmpty()
            && !infUrl.isEmpty()
            && (normUrl.compare(infUrl, Qt::CaseInsensitive) == 0
                || normUrl.startsWith(infUrl + QLatin1Char('/'), Qt::CaseInsensitive));
        if (!handleMatch && !urlMatch)
        {
            continue;
        }
        if (checked)
        {
            obj.insert("lastCheckedAt", ts);
        }
        obj.insert("lastPickedAt", ts);
        obj.insert("pickedCount", obj.value("pickedCount").toInt(0) + 1);
        obj.insert("updatedAt", ts);
        list[i] = obj;
        ++matched;
    }
    if (matched > 0)
    {
        auto next = m_data;
        next.insert("influencers", list);
        if (!save(next))
        {
            return 0;
        }
    }
    return matched;
}

bool ResearchStore::recordAssessment(const QString &id, const QJsonObject &assessment)
{
    auto refs = references();
    for (int i = 0; i < refs.size(); ++i)
    {
        auto r = refs[i].toObject();
        if (r.value("id").toString() != id) continue;
        auto history = r.value("assessments").toArray();
        auto result = assessment;
        result.insert("at", now());
        history.append(result);
        r.insert("assessments", history);
        refs[i] = r;
        auto next = m_data;
        next.insert("references", refs);
        return save(next);
    }
    m_error = tr("Reference no longer exists."); return false;
}

bool ResearchStore::recordSpySession(const QString &accountId, const QJsonObject &session)
{
    if (QUuid(accountId).isNull())
    {
        m_error = tr("Choose an account first.");
        return false;
    }
    auto next = m_data;
    auto sessions = next.value("spySessions").toObject();
    auto entry = session;
    entry.insert("at", now());
    sessions.insert(accountId, entry);
    next.insert("spySessions", sessions);
    return save(next);
}

QJsonObject ResearchStore::lastSpySession(const QString &accountId) const
{
    return m_data.value("spySessions").toObject().value(accountId).toObject();
}

QString ResearchStore::mediaPath(const QJsonObject &r) const
{
    const QString relative = r.value("media").toString();
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || QDir::cleanPath(relative).startsWith("..")) return {};
    return m_dir.filePath(relative);
}

QString ResearchStore::productRestriction(const QJsonObject &product, const QJsonObject &profile)
{
    if (product.isEmpty()) return tr("Product is no longer available in the imported catalog.");
    if (!includes(profile.value("brands").toArray(), product.value("brand").toString())) return tr("Brand is not allowed for this account.");
    for (const auto &category : product.value("categories").toArray())
        if (includes(profile.value("categories").toArray(), category.toString())) return {};
    return tr("Category is not allowed for this account.");
}
