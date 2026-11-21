#include "ResearchStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QUrl>
#include <QUrlQuery>

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
