#include "ProductSource.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <algorithm>

namespace {
constexpr int kMinHighResDimension = 200;

QMap<QString, const ProductSource *> &registry()
{
    static QMap<QString, const ProductSource *> sources;
    return sources;
}

QString colorSlug(const QString &color)
{
    QString result;
    for (const QChar &c : color.toLower())
    {
        if (c.isLetterOrNumber())
            result += c;
        else if (!result.isEmpty() && result.back() != QLatin1Char('-'))
            result += QLatin1Char('-');
    }
    while (result.endsWith(QLatin1Char('-')))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("unknown") : result;
}

QString translatedColorSlug(const QString &colorOrSlug)
{
    static const QHash<QString, QString> kTokenMap{
        {QStringLiteral("schwarz"), QStringLiteral("black")},
        {QStringLiteral("weiß"), QStringLiteral("white")},
        {QStringLiteral("weiss"), QStringLiteral("white")},
        {QStringLiteral("grau"), QStringLiteral("grey")},
        {QStringLiteral("silber"), QStringLiteral("silver")},
        {QStringLiteral("gold"), QStringLiteral("gold")},
        {QStringLiteral("blau"), QStringLiteral("blue")},
        {QStringLiteral("rot"), QStringLiteral("red")},
        {QStringLiteral("gelb"), QStringLiteral("yellow")},
        {QStringLiteral("grün"), QStringLiteral("green")},
        {QStringLiteral("gruen"), QStringLiteral("green")},
        {QStringLiteral("braun"), QStringLiteral("brown")},
        {QStringLiteral("kastanienbraun"), QStringLiteral("brown")},
        {QStringLiteral("marine"), QStringLiteral("navy")},
        {QStringLiteral("und"), QStringLiteral("and")},
        {QStringLiteral("zwart"), QStringLiteral("black")},
        {QStringLiteral("wit"), QStringLiteral("white")},
        {QStringLiteral("grijs"), QStringLiteral("grey")},
        {QStringLiteral("zilver"), QStringLiteral("silver")},
        {QStringLiteral("blauw"), QStringLiteral("blue")},
        {QStringLiteral("rood"), QStringLiteral("red")},
        {QStringLiteral("geel"), QStringLiteral("yellow")},
        {QStringLiteral("groen"), QStringLiteral("green")},
        {QStringLiteral("bruin"), QStringLiteral("brown")},
        {QStringLiteral("kastanjebruin"), QStringLiteral("brown")},
        {QStringLiteral("roze"), QStringLiteral("rose")},
        {QStringLiteral("en"), QStringLiteral("and")},
        {QStringLiteral("noir"), QStringLiteral("black")},
        {QStringLiteral("blanc"), QStringLiteral("white")},
        {QStringLiteral("gris"), QStringLiteral("grey")},
        {QStringLiteral("argent"), QStringLiteral("silver")},
        {QStringLiteral("bleu"), QStringLiteral("blue")},
        {QStringLiteral("rouge"), QStringLiteral("red")},
        {QStringLiteral("jaune"), QStringLiteral("yellow")},
        {QStringLiteral("vert"), QStringLiteral("green")},
        {QStringLiteral("marron"), QStringLiteral("brown")},
        {QStringLiteral("brun"), QStringLiteral("brown")},
        {QStringLiteral("châtain"), QStringLiteral("brown")},
        {QStringLiteral("chatain"), QStringLiteral("brown")},
        {QStringLiteral("et"), QStringLiteral("and")},
        {QStringLiteral("nero"), QStringLiteral("black")},
        {QStringLiteral("bianco"), QStringLiteral("white")},
        {QStringLiteral("grigio"), QStringLiteral("grey")},
        {QStringLiteral("rosso"), QStringLiteral("red")},
        {QStringLiteral("blu"), QStringLiteral("blue")},
        {QStringLiteral("giallo"), QStringLiteral("yellow")},
        {QStringLiteral("verde"), QStringLiteral("green")},
        {QStringLiteral("marrone"), QStringLiteral("brown")},
        {QStringLiteral("castano"), QStringLiteral("brown")},
        {QStringLiteral("negro"), QStringLiteral("black")},
        {QStringLiteral("blanco"), QStringLiteral("white")},
        {QStringLiteral("plata"), QStringLiteral("silver")},
        {QStringLiteral("azul"), QStringLiteral("blue")},
        {QStringLiteral("amarillo"), QStringLiteral("yellow")},
        {QStringLiteral("marrón"), QStringLiteral("brown")},
        {QStringLiteral("castaño"), QStringLiteral("brown")},
    };
    const QStringList parts = colorSlug(colorOrSlug).split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QStringLiteral("unknown");
    QStringList mapped;
    mapped.reserve(parts.size());
    for (const QString &part : parts)
        mapped.append(kTokenMap.value(part, part));
    return mapped.join(QLatin1Char('-'));
}

QString sortedTokensKey(const QString &colorOrSlug)
{
    QStringList parts = translatedColorSlug(colorOrSlug).split(QLatin1Char('-'), Qt::SkipEmptyParts);
    parts.removeDuplicates();
    std::sort(parts.begin(), parts.end());
    return parts.isEmpty() ? QStringLiteral("unknown") : parts.join(QLatin1Char('-'));
}

bool isHighResImage(const QString &path, QSize *dimensions = nullptr)
{
    QImageReader reader(path);
    const QSize size = reader.size();
    if (dimensions)
        *dimensions = size;
    if (size.isValid())
        return qMax(size.width(), size.height()) >= kMinHighResDimension;
    return false;
}

struct APlusFolder
{
    QString dirName;
    QString uploadedAt;
    QSet<QString> excludedSlugs;
    QMap<QString, QStringList> colorImagesBySlug;
    QMap<QString, QStringList> colorImagesByTokenSet;
    QStringList genericImages;
    QMap<QString, QString> mainImages;
    bool multiColor = false;
};

bool folderHasAPlusContent(const QDir &subDir, const QString &uploadedAt)
{
    if (!uploadedAt.isEmpty())
        return true;
    QFile indexFile(subDir.filePath("aplus/index.json"));
    if (!indexFile.open(QIODevice::ReadOnly))
        return false;
    const auto doc = QJsonDocument::fromJson(indexFile.readAll());
    for (const auto &val : doc.object().value("elements").toArray())
    {
        const auto elem = val.toObject();
        const QString type = elem.value("type").toString();
        if ((type == QLatin1String("image") || type == QLatin1String("faq"))
            && !elem.value("versions").toArray().isEmpty())
        {
            return true;
        }
    }
    return false;
}

QStringList resolveHighResImages(const APlusFolder &folder, const QString &color, const QString &asin)
{
    const QString slug = colorSlug(color);
    const QString translated = translatedColorSlug(color);
    if (folder.excludedSlugs.contains(slug) || folder.excludedSlugs.contains(translated))
        return {};
    if (folder.colorImagesBySlug.contains(slug))
        return folder.colorImagesBySlug.value(slug);
    if (folder.colorImagesBySlug.contains(translated))
        return folder.colorImagesBySlug.value(translated);
    const QString tokenKey = sortedTokensKey(color);
    if (tokenKey != QLatin1String("unknown") && folder.colorImagesByTokenSet.contains(tokenKey))
        return folder.colorImagesByTokenSet.value(tokenKey);
    if (!folder.multiColor)
    {
        if (!folder.genericImages.isEmpty())
            return folder.genericImages;
        if (folder.mainImages.contains(asin))
            return {folder.mainImages.value(asin)};
        if (folder.mainImages.size() == 1)
            return {folder.mainImages.first()};
    }
    return {};
}

class AmazonTemplateSource final : public ProductSource
{
public:
    QString id() const override { return QStringLiteral("amazon-template3"); }
    QString name() const override { return QStringLiteral("AmazonTemplate3 working directory"); }
    Result read(const QString &directory) const override
    {
        Result result;
        const QString trimmed = directory.trimmed();
        if (trimmed.isEmpty())
        {
            result.error = QStringLiteral("Select an AmazonTemplate3 working directory first.");
            return result;
        }
        if (!QDir(trimmed).exists())
        {
            result.error = QStringLiteral("The working directory \"%1\" does not exist and seems invalid.")
                .arg(QDir::toNativeSeparators(trimmed));
            return result;
        }
        const QDir root(QFileInfo(trimmed).canonicalFilePath());
        const QDir stores(root.filePath("stores"));
        if (!stores.exists())
        {
            result.error = QStringLiteral("The working directory \"%1\" does not contain the folder \"stores\" and seems invalid.")
                .arg(QDir::toNativeSeparators(trimmed));
            return result;
        }
        const QDir sizing(root.filePath("sizing"));
        if (!sizing.exists())
        {
            result.error = QStringLiteral("The working directory \"%1\" does not contain the folder \"sizing\" and seems invalid.")
                .arg(QDir::toNativeSeparators(trimmed));
            return result;
        }

        // Index only sizing subfolders that have A+ content and high-resolution product photos
        // (never falling back to the 54x54 preview thumbnails in stores/thumbs/).
        QList<APlusFolder> aplusFolders;
        QHash<QString, int> asinToFolder;
        QHash<QString, int> skuToFolder;
        const QRegularExpression colorImgRe(QStringLiteral(R"(^(.+)-image-(\d+)\.(?:jpg|jpeg|png)$)"),
                                            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression genericImgRe(QStringLiteral(R"(^image-(\d+)\.(?:jpg|jpeg|png)$)"),
                                              QRegularExpression::CaseInsensitiveOption);
        const QRegularExpression asinPrefixRe(QStringLiteral("^B0[A-Z0-9]{8,}$"));

        for (const QFileInfo &dirInfo : sizing.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        {
            const QDir subDir(dirInfo.absoluteFilePath());
            QSettings ini(subDir.filePath("settings.ini"), QSettings::IniFormat);
            const QString uploadedAt = ini.value("aplus/uploadedAt").toString().trimmed();
            if (!folderHasAPlusContent(subDir, uploadedAt))
                continue;

            APlusFolder folder;
            folder.dirName = dirInfo.fileName();
            folder.uploadedAt = uploadedAt;

            for (const QString &rawEntry : ini.value("aplus/excluded_colors").toStringList())
            {
                for (const QString &part : rawEntry.split(QLatin1Char(','), Qt::SkipEmptyParts))
                {
                    const QString clean = part.trimmed();
                    if (!clean.isEmpty())
                    {
                        folder.excludedSlugs.insert(colorSlug(clean));
                        folder.excludedSlugs.insert(translatedColorSlug(clean));
                    }
                }
            }

            QSet<QString> folderAsins;
            QSet<QString> folderSkus;
            const QString parentAsin = ini.value("sizing/parentAsin").toString().trimmed();
            if (!parentAsin.isEmpty())
                folderAsins.insert(parentAsin);

            ini.beginGroup("sizing/skus");
            for (const QString &asinKey : ini.childKeys())
            {
                const QString a = asinKey.trimmed();
                const QString s = ini.value(asinKey).toString().trimmed();
                if (!a.isEmpty()) folderAsins.insert(a);
                if (!s.isEmpty()) folderSkus.insert(s);
            }
            ini.endGroup();

            QFile healthFile(subDir.filePath("broken_child_health.json"));
            if (healthFile.open(QIODevice::ReadOnly))
            {
                const auto healthDoc = QJsonDocument::fromJson(healthFile.readAll());
                for (const auto &rowVal : healthDoc.object().value("rows").toArray())
                {
                    const auto row = rowVal.toObject();
                    const QString a = row.value("asin").toString().trimmed();
                    const QString s = row.value("sku").toString().trimmed();
                    if (!a.isEmpty()) folderAsins.insert(a);
                    if (!s.isEmpty()) folderSkus.insert(s);
                }
            }

            const QString namePrefix = folder.dirName.section(QLatin1Char('-'), 0, 0);
            if (asinPrefixRe.match(namePrefix).hasMatch())
                folderAsins.insert(namePrefix);

            for (const QFileInfo &fileInfo : subDir.entryInfoList(QDir::Files, QDir::Name))
            {
                const QString fileName = fileInfo.fileName();
                const QString filePath = fileInfo.absoluteFilePath();
                if (const auto m = colorImgRe.match(fileName); m.hasMatch())
                {
                    if (isHighResImage(filePath))
                    {
                        const QString rawSlug = colorSlug(m.captured(1));
                        const QString transSlug = translatedColorSlug(rawSlug);
                        const QString tokKey = sortedTokensKey(rawSlug);
                        folder.colorImagesBySlug[rawSlug].append(filePath);
                        if (transSlug != rawSlug)
                            folder.colorImagesBySlug[transSlug].append(filePath);
                        if (tokKey != QLatin1String("unknown"))
                            folder.colorImagesByTokenSet[tokKey].append(filePath);
                    }
                    continue;
                }
                if (genericImgRe.match(fileName).hasMatch())
                {
                    if (isHighResImage(filePath))
                        folder.genericImages.append(filePath);
                    continue;
                }
                if (fileName.endsWith(QLatin1String("_main.jpg"), Qt::CaseInsensitive))
                {
                    const QString mainAsin = fileName.left(fileName.size() - 9);
                    if (!mainAsin.isEmpty() && mainAsin.compare(QLatin1String("main"), Qt::CaseInsensitive) != 0)
                    {
                        folderAsins.insert(mainAsin);
                        if (isHighResImage(filePath))
                            folder.mainImages.insert(mainAsin, filePath);
                    }
                }
            }

            folder.multiColor = !folder.colorImagesBySlug.isEmpty()
                || QDir(subDir.filePath("excluded")).exists();
            if (folder.colorImagesBySlug.isEmpty() && folder.genericImages.isEmpty() && folder.mainImages.isEmpty())
                continue;

            const int folderIdx = aplusFolders.size();
            aplusFolders.append(folder);
            const auto shouldPrefer = [&](int existingIdx) {
                return aplusFolders[existingIdx].uploadedAt.isEmpty() && !folder.uploadedAt.isEmpty();
            };
            for (const QString &a : folderAsins)
            {
                if (!asinToFolder.contains(a) || shouldPrefer(asinToFolder.value(a)))
                    asinToFolder.insert(a, folderIdx);
            }
            for (const QString &s : folderSkus)
            {
                if (!skuToFolder.contains(s) || shouldPrefer(skuToFolder.value(s)))
                    skuToFolder.insert(s, folderIdx);
            }
        }

        const auto files = stores.entryInfoList({"*.json"}, QDir::Files, QDir::Name);
        QMap<QString, QJsonObject> groups;
        QSet<QString> seen;
        int marketplaceFiles = 0;
        const auto validMarketplace = QRegularExpression(QStringLiteral("^[A-Z0-9]{10,20}$"));
        for (const QFileInfo &info : files)
        {
            const QString marketplace = info.baseName();
            if (!validMarketplace.match(marketplace).hasMatch()) continue;
            ++marketplaceFiles;
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
            {
                result.error = info.fileName() + ": " + file.errorString();
                return result;
            }
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isArray())
            {
                result.error = info.fileName() + ": invalid product catalog JSON array.";
                return result;
            }
            for (const auto &value : document.array())
            {
                if (!value.isObject() || value.toObject().value("asin").toString().isEmpty())
                {
                    result.error = info.fileName() + ": product record missing ASIN.";
                    return result;
                }
                const auto item = value.toObject();
                if (item.value("deleted").toBool())
                {
                    ++result.archivedRecords;
                    continue;
                }
                const QString asin = item.value("asin").toString();
                const QString variantKey = marketplace + ":" + asin;
                if (seen.contains(variantKey)) continue;
                seen.insert(variantKey);

                const QString sku = item.value("sku").toString().trimmed();
                const int folderIdx = asinToFolder.contains(asin)
                    ? asinToFolder.value(asin)
                    : skuToFolder.value(sku, -1);
                if (folderIdx < 0)
                {
                    ++result.skippedWithoutAPlus;
                    continue;
                }

                const APlusFolder &folder = aplusFolders.at(folderIdx);
                const QStringList resolvedImages = resolveHighResImages(folder, item.value("color").toString(), asin);
                if (resolvedImages.isEmpty())
                {
                    ++result.skippedWithoutAPlus;
                    continue;
                }

                const QString primaryImage = resolvedImages.first();
                const QString key = marketplace + QChar(0x1f) + item.value("brand").toString()
                    + QChar(0x1f) + folder.dirName + QChar(0x1f) + QFileInfo(primaryImage).fileName();
                QJsonObject product = groups.value(key);
                if (product.isEmpty())
                {
                    product = item;
                    product.remove("placements");
                    // Stock is per variant and may be missing/stale, never a group total.
                    product.remove("inventory");
                    product.insert("id", QString::fromLatin1(QCryptographicHash::hash(
                        (root.absolutePath() + QChar(0x1f) + key).toUtf8(), QCryptographicHash::Sha256).toHex()));
                    product.insert("source", id());
                    product.insert("sourceDirectory", root.absolutePath());
                    product.insert("marketplace", marketplace);
                    product.insert("sizingFolder", folder.dirName);
                    if (!folder.uploadedAt.isEmpty())
                        product.insert("aplusUploadedAt", folder.uploadedAt);
                    product.insert("localImage", primaryImage);
                    QJsonArray allImages;
                    for (const QString &imgPath : resolvedImages)
                        allImages.append(imgPath);
                    product.insert("localImages", allImages);
                    QSize imgSize;
                    if (isHighResImage(primaryImage, &imgSize) && imgSize.isValid())
                        product.insert("imageResolution", QStringLiteral("%1x%2").arg(imgSize.width()).arg(imgSize.height()));
                    product.insert("sourceModifiedAt", info.lastModified().toUTC().toString(Qt::ISODateWithMs));
                    product.insert("importedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                }
                QJsonObject variant = item;
                variant.insert("localImage", primaryImage);
                auto variants = product.value("variants").toArray();
                variants.append(variant);
                product.insert("variants", variants);
                auto categories = product.value("categories").toArray();
                const auto addCategory = [&](const QString &category) {
                    if (!category.isEmpty() && !categories.contains(category)) categories.append(category);
                };
                addCategory(item.value("category").toString());
                for (const auto &placement : item.value("placements").toArray())
                {
                    const auto path = placement.toObject().value("path").toArray();
                    if (path.size() > 1) addCategory(path.at(1).toString());
                }
                product.insert("categories", categories);
                groups.insert(key, product);
            }
        }
        if (!marketplaceFiles)
        {
            result.error = QStringLiteral("The working directory \"%1\" contains \"stores\", but no marketplace catalog JSON files were found in \"stores/\" and seems invalid.")
                .arg(QDir::toNativeSeparators(trimmed));
        }
        else if (aplusFolders.isEmpty() || groups.isEmpty())
        {
            result.error = QStringLiteral("No products with A+ content and high-resolution images were found in \"%1\".")
                .arg(QDir::toNativeSeparators(trimmed));
        }
        for (const auto &product : groups) result.products.append(product);
        return result;
    }
};
const AmazonTemplateSource amazonSource;
const ProductSource::Recorder amazonRecorder(&amazonSource);
}

const QMap<QString, const ProductSource *> &ProductSource::all() { return registry(); }
ProductSource::Recorder::Recorder(const ProductSource *source) { registry().insert(source->id(), source); }
