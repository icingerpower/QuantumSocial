#include "ResearchCommand.h"
#include "ResearchStore.h"
#include "ProductSource.h"
#include "ResearchProject.h"
#include "model/TableAccounts.h"
#include "model/TableProjects.h"
#include "model/accounts/AbstractSocialAccount.h"
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>

int ResearchCommand::run(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("QuantumSocial research commands (no GUI or browser required).");
    parser.addHelpOption();
    parser.addPositionalArgument("research", "Research command group.");
    parser.addPositionalArgument("action", "accounts, products, references, import-products, add-reference, scroll-and-save, delete-references, link-product, review, or create-project");
    parser.addOption({"workspace", "Existing QuantumSocial working directory.", "directory"});
    parser.addOption({"source", "AmazonTemplate3 directory to import.", "directory"});
    parser.addOption({"account", "Stable account ID (see accounts).", "id"});
    parser.addOption({"platform", "Social platform ID for scroll-and-save (pinterest, tiktok, youtube, instagram).", "platform", "pinterest"});
    parser.addOption({"query", "Search query or URL for scroll-and-save.", "query"});
    parser.addOption({"max-posts", "Maximum posts to save during scroll-and-save.", "count", "24"});
    parser.addOption({"min-engagement", "Minimum engagement threshold (saves+likes or views) for scroll-and-save.", "score", "0"});
    parser.addOption({"max-days-old", "Maximum post age in days (0 = no limit) for scroll-and-save.", "days", "0"});
    parser.addOption({"mode", "Scroll mode for scroll-and-save: auto or manual.", "mode", "auto"});
    parser.addOption({"reference", "Reference ID.", "id"});
    parser.addOption({"all", "Apply action (such as delete-references) to all references of the account."});
    parser.addOption({"product", "Imported product group ID.", "id"});
    parser.addOption({"url", "Post URL.", "url"});
    parser.addOption({"media", "Local reference image/video to copy.", "file"});
    parser.addOption({"title", "Reference title.", "text"});
    parser.addOption({"status", "Review decision: approved, rejected, or pending.", "status"});
    parser.addOption({"reason", "Human review reason.", "text"});
    parser.addOption({"image", "Product photo for project creation (defaults to catalog image).", "file"});
    const auto output = [](const QJsonObject &result, int code = 0) {
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
        return code;
    };
    const auto error = [&](const QString &message) { return output({{"ok", false}, {"error", message}}, 1); };
    if (!parser.parse(arguments)) return error(parser.errorText());
    if (parser.isSet("help")) { QTextStream(stdout) << parser.helpText(); return 0; }
    const auto positional = parser.positionalArguments();
    if (positional.size() != 2 || positional[0] != "research") return error("Usage: QuantumSocial research <action> --workspace <directory>. See research --help.");
    const QString action = positional[1];
    if (!QStringList{"accounts", "products", "references", "import-products", "add-reference", "scroll-and-save", "delete-references", " link-product", "link-product", "review", "create-project"}.contains(action))
        return error("Unknown research action.");
    const QString workspace = QFileInfo(parser.value("workspace")).canonicalFilePath();
    if (!parser.isSet("workspace") || workspace.isEmpty() || !QFileInfo(workspace).isDir()) return error("--workspace must name an existing directory.");
    TableAccounts accounts(workspace);
    if (!accounts.lastError().isEmpty()) return error(accounts.lastError());
    if (action == "accounts")
    {
        QJsonArray rows;
        for (int row = 0; row < accounts.rowCount(); ++row)
            rows.append(QJsonObject{{"id", accounts.accountId(row).toString(QUuid::WithoutBraces)},
                {"platform", accounts.data(accounts.index(row, TableAccounts::IND_TYPE)).toString()},
                {"url", accounts.data(accounts.index(row, TableAccounts::IND_URL)).toString()}});
        return output({{"ok", true}, {"accounts", rows}});
    }
    ResearchStore store(workspace);
    if (!store.lastError().isEmpty()) return error(store.lastError());
    if (action == "import-products")
    {
        if (!parser.isSet("source")) return error("--source is required.");
        const auto result = ProductSource::all().value("amazon-template3")->read(parser.value("source"));
        if (!result.error.isEmpty()) return error(result.error);
        if (!store.replaceProducts(result.products, QFileInfo(parser.value("source")).canonicalFilePath())) return error(store.lastError());
        return output({{"ok", true}, {"productGroups", result.products.size()}, {"archivedRecordsExcluded", result.archivedRecords}});
    }
    const QString accountId = QUuid(parser.value("account")).toString(QUuid::WithoutBraces);
    if (accounts.rowOfId(QUuid(accountId)) < 0) return error("--account must identify an existing account (see research accounts).");
    if (action == "products" || action == "references")
    {
        QJsonArray rows;
        const auto values = action == "products" ? store.products() : store.references();
        for (const auto &v : values)
        {
            const auto object = v.toObject();
            if (action == "products" ? ResearchStore::productRestriction(object, store.profile(accountId)).isEmpty()
                                     : object.value("accountId").toString() == accountId) rows.append(object);
        }
        return output({{"ok", true}, {action, rows}});
    }
    if (action == "add-reference")
    {
        const QString id = store.addReference(accountId, parser.value("url"), parser.value("title"), parser.value("media"));
        return id.isEmpty() ? error(store.lastError()) : output({{"ok", true}, {"referenceId", id}});
    }
    if (action == "delete-references")
    {
        QStringList ids;
        if (parser.isSet("all"))
        {
            for (const auto &v : store.references())
            {
                const auto r = v.toObject();
                if (r.value("accountId").toString() == accountId)
                {
                    ids << r.value("id").toString();
                }
            }
            if (ids.isEmpty())
            {
                return output({{"ok", true}, {"deletedCount", 0}});
            }
        }
        else if (parser.isSet("reference"))
        {
            for (const QString &part : parser.value("reference").split(',', Qt::SkipEmptyParts))
            {
                const QString id = part.trimmed();
                const auto r = store.reference(id);
                if (r.isEmpty() || r.value("accountId").toString() != accountId)
                {
                    return error("Reference not found for this account: " + id);
                }
                ids << id;
            }
        }
        if (ids.isEmpty())
        {
            return error("Specify --reference <id[,id...]> or --all for delete-references.");
        }
        if (!store.removeReferences(ids))
        {
            return error(store.lastError());
        }
        return output({{"ok", true}, {"deletedCount", ids.size()}});
    }
    if (action == "scroll-and-save")
    {
        const QString platformId = parser.value("platform").trimmed().toLower();
        const auto *platform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS().value(platformId);
        if (!platform || !platform->supportsSpyScrollAndSave())
            return error("Unsupported platform for scroll-and-save.");
        const QString queryOrUrl = parser.isSet("query") ? parser.value("query").trimmed() : parser.value("url").trimmed();
        if (queryOrUrl.isEmpty())
            return error("--query (or --url) is required for scroll-and-save.");
        const int maxPosts = qMax(1, parser.value("max-posts").toInt());
        const int minEngagement = qMax(0, parser.value("min-engagement").toInt());
        const int maxDaysOld = qMax(0, parser.value("max-days-old").toInt());
        const bool autoScroll = parser.value("mode").trimmed().toLower() != QLatin1String("manual");
        const QString downloadDir = store.directory().filePath(QStringLiteral("spy_downloads"));
        QDir().mkpath(downloadDir);
        const QStringList excludeUrls = store.excludedSpyUrls(accountId);
        QEventLoop loop;
        AbstractSocialAccount::SpyResult spyRes;
        QCoro::connect(
            platform->spyScrollAndSave(queryOrUrl, downloadDir, maxPosts, autoScroll, minEngagement, excludeUrls, maxDaysOld),
            &loop,
            [&spyRes, &loop](AbstractSocialAccount::SpyResult r) {
                spyRes = std::move(r);
                loop.quit();
            });
        loop.exec();
        if (!spyRes.errorMessage.isEmpty() && spyRes.posts.isEmpty())
            return error(spyRes.errorMessage);
        const QString productId = parser.value("product").trimmed();
        QJsonArray referenceIds;
        for (const auto &post : spyRes.posts)
        {
            if (!post.postUrl.isEmpty() && store.isRejectedUrl(accountId, post.postUrl))
            {
                continue;
            }
            const QString id = store.addReference(accountId, post.postUrl, post.title.isEmpty() ? post.postUrl : post.title, post.localMediaPath);
            if (id.isEmpty()) continue;
            const QString existingStatus = store.reference(id).value("status").toString();
            if (existingStatus == QLatin1String("rejected") || existingStatus == QLatin1String("approved"))
            {
                continue;
            }
            QJsonObject patch;
            if (!productId.isEmpty() && !store.product(productId).isEmpty())
                patch.insert("productId", productId);
            QJsonObject stats = store.reference(id).value("stats").toObject();
            if (post.views >= 0) stats.insert("views", post.views);
            if (post.likes >= 0) stats.insert("likes", post.likes);
            if (post.comments >= 0) stats.insert("comments", post.comments);
            if (post.shares >= 0)
            {
                stats.insert("shares", post.shares);
                if (platformId == QLatin1String("pinterest")) stats.insert("saves", post.shares);
            }
            if (!post.author.isEmpty()) stats.insert("author", post.author);
            if (!post.publishedAt.isEmpty()) stats.insert("publishedAt", post.publishedAt);
            if (post.isAd) stats.insert("isAd", true);
            if (!stats.isEmpty()) patch.insert("stats", stats);
            if (!patch.isEmpty()) store.updateReference(id, patch);
            referenceIds.append(id);
        }
        if (spyRes.browsedCount > 0)
        {
            store.recordSpySession(accountId, QJsonObject{
                {"platform", platform->getName()},
                {"platformId", platformId},
                {"query", queryOrUrl},
                {"browsedCount", spyRes.browsedCount},
                {"savedCount", int(referenceIds.size())},
                {"rejectedCount", spyRes.rejectedCount},
                {"minEngagement", spyRes.minEngagement},
                {"maxDaysOld", spyRes.maxDaysOld}
            });
        }
        const double rejectRate = spyRes.browsedCount > 0
            ? (100.0 * double(spyRes.rejectedCount) / double(spyRes.browsedCount))
            : 0.0;
        return output({
            {"ok", true},
            {"browsedCount", spyRes.browsedCount},
            {"savedCount", referenceIds.size()},
            {"rejectedCount", spyRes.rejectedCount},
            {"rejectRatePercent", qRound(rejectRate * 10.0) / 10.0},
            {"referenceIds", referenceIds}
        });
    }
    const auto reference = store.reference(parser.value("reference"));
    if (reference.isEmpty() || reference.value("accountId").toString() != accountId) return error("Reference not found for this account.");
    if (action == "link-product")
    {
        if (store.product(parser.value("product")).isEmpty()) return error("--product must identify an imported product group.");
        const bool ok = store.updateReference(parser.value("reference"), {{"productId", parser.value("product")}});
        return ok ? output({{"ok", true}}) : error(store.lastError());
    }
    if (action == "review")
    {
        if (!QStringList{"pending", "approved", "rejected"}.contains(parser.value("status"))) return error("--status must be pending, approved, or rejected.");
        const bool ok = store.review({parser.value("reference")}, parser.value("status"), parser.value("reason"));
        return ok ? output({{"ok", true}}) : error(store.lastError());
    }
    const QString chosenProductId = parser.isSet("product")
        ? parser.value("product").trimmed()
        : reference.value("productId").toString();
    const auto product = store.product(chosenProductId);
    QJsonObject context{{"schemaVersion", 1}, {"accountId", accountId}, {"profile", store.profile(accountId)},
                        {"product", product}, {"reference", reference}, {"referenceMediaPath", store.mediaPath(reference)}};
    TableProjects projects(workspace);
    QString message;
    const QString image = parser.isSet("image") ? parser.value("image") : product.value("localImage").toString();
    const int row = ResearchProject::create(projects, context, image, &message);
    if (row < 0) return error(message);
    return output({{"ok", true}, {"projectId", projects.projectId(row).toString(QUuid::WithoutBraces)}});
}
