#include "ResearchProject.h"
#include "ResearchStore.h"
#include "model/TableProjects.h"
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

QString deriveProductSeoKeyword(const QJsonObject &product)
{
    const QString directCat = product.value(QStringLiteral("category")).toString().trimmed();
    if (!directCat.isEmpty())
    {
        return directCat.toLower();
    }
    const QString title = product.value(QStringLiteral("title")).toString();
    static const QStringList kHighSignalTerms{
        QStringLiteral("slingback"), QStringLiteral("stiletto"), QStringLiteral("pumps"),
        QStringLiteral("heels"), QStringLiteral("sandals"), QStringLiteral("mules"),
        QStringLiteral("boots"), QStringLiteral("platform"), QStringLiteral("ankle strap"),
        QStringLiteral("pointed toe"), QStringLiteral("loafers"), QStringLiteral("sneakers")
    };
    QStringList matched;
    for (const QString &term : kHighSignalTerms)
    {
        if (title.contains(term, Qt::CaseInsensitive) && matched.size() < 2)
        {
            matched << term;
        }
    }
    if (!matched.isEmpty())
    {
        return matched.join(QLatin1Char(' '));
    }
    const QJsonArray cats = product.value(QStringLiteral("categories")).toArray();
    if (!cats.isEmpty() && !cats.first().toString().trimmed().isEmpty())
    {
        return cats.first().toString().trimmed().toLower();
    }
    return QString{};
}

} // namespace

int ResearchProject::create(TableProjects &projects, QJsonObject context, const QString &imagePath, QString *error)
{
    const auto reference = context.value("reference").toObject();
    const auto product = context.value("product").toObject();
    const QString restriction = ResearchStore::productRestriction(product, context.value("profile").toObject());
    if (!restriction.isEmpty() || reference.value("status").toString() != "approved"
        || reference.value("accountId") != context.value("accountId")
        || product.value("id").toString().isEmpty()
        || !QImageReader(imagePath).canRead())
    {
        if (error) *error = restriction.isEmpty() ? QStringLiteral("An approved reference, an eligible product, and a readable product image are required.") : restriction;
        return -1;
    }
    const int row = projects.addProject(imagePath, {}, deriveProductSeoKeyword(product), reference.value("notes").toString());
    const QString color = product.value("color").toString().trimmed();
    const QString prodTitle = product.value("title").toString().trimmed();
    const QString projectName = (!color.isEmpty() && !prodTitle.contains(color, Qt::CaseInsensitive))
        ? (color + QStringLiteral(" — ") + prodTitle).left(100)
        : prodTitle.left(100);
    projects.setData(projects.index(row, TableProjects::IND_NAME), projectName);
    const auto directory = projects.projectDir(row);
    const auto fail = [&](const QString &message) {
        if (error) *error = message;
        projects.removeProject(row);
        return -1;
    };
    if (!projects.absoluteImagePath(row).startsWith(directory.absolutePath() + '/'))
        return fail(QStringLiteral("Could not copy the product image into the new project."));
    const QString sourceMedia = context.take("referenceMediaPath").toString();
    if (!sourceMedia.isEmpty())
    {
        const QString relative = "research-reference." + QFileInfo(sourceMedia).suffix();
        if (!QFile::copy(sourceMedia, directory.filePath(relative)))
            return fail(QStringLiteral("Could not copy reference media into the new project."));
        context.insert("referenceLocalMedia", relative);
    }
    context.insert("projectId", projects.projectId(row).toString(QUuid::WithoutBraces));
    context.insert("schemaVersion", 1);
    const auto bytes = QJsonDocument(context).toJson();
    QSaveFile file(directory.filePath("research_context.json"));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(QStringLiteral("Could not save the research context: ") + file.errorString());
    if (error) error->clear();
    return row;
}

bool ResearchProject::snapshot(const QDir &projectDirectory, const QDir &generationDirectory, QString *error)
{
    QFile source(projectDirectory.filePath("research_context.json"));
    if (!source.exists()) return true;
    if (!source.open(QIODevice::ReadOnly)) { if (error) *error = source.errorString(); return false; }
    const auto bytes = source.readAll();
    const auto context = QJsonDocument::fromJson(bytes).object();
    if (context.value("schemaVersion").toInt() != 1)
    { if (error) *error = QStringLiteral("Invalid research context; generation stopped to preserve provenance."); return false; }
    const QString media = context.value("referenceLocalMedia").toString();
    if (!media.isEmpty() && (QFileInfo(media).fileName() != media
        || !QFile::copy(projectDirectory.filePath(media), generationDirectory.filePath(media))))
    { if (error) *error = QStringLiteral("Could not snapshot reference media for this generation."); return false; }
    QSaveFile destination(generationDirectory.filePath("research_context.json"));
    if (!destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size() || !destination.commit())
    { if (error) *error = destination.errorString(); return false; }
    return true;
}
