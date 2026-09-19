#include "VideoGenerationRecipe.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

bool VideoGenerationRecipe::save(const QDir &directory, QString *error) const
{
    QJsonArray images;
    if (!directory.mkpath(QStringLiteral("replay_inputs")))
    {
        *error = QStringLiteral("Could not create the saved source-image directory.");
        return false;
    }
    for (int i = 0; i < imagePaths.size(); ++i)
    {
        const QFileInfo source{imagePaths[i]};
        const QString relative = QStringLiteral("replay_inputs/image_%1.%2")
            .arg(i + 1).arg(source.suffix());
        const QString target = directory.filePath(relative);
        if (!source.isFile())
        {
            *error = QStringLiteral("Source image is missing: %1").arg(source.filePath());
            return false;
        }
        if (source.absoluteFilePath() != QFileInfo(target).absoluteFilePath())
        {
            QFile::remove(target);
            if (!QFile::copy(source.absoluteFilePath(), target))
            {
                *error = QStringLiteral("Could not save source image: %1").arg(source.filePath());
                return false;
            }
        }
        images.append(relative);
    }
    QVariantMap backendSettings = settings;
    backendSettings.remove(QStringLiteral("preservePrompt"));
    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("generatorId"), generatorId},
        {QStringLiteral("prompt"), prompt},
        {QStringLiteral("images"), images},
        {QStringLiteral("settings"), QJsonObject::fromVariantMap(backendSettings)}};
    QSaveFile file{directory.filePath(QStringLiteral("generation_config.json"))};
    const QByteArray data = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
    {
        *error = QStringLiteral("Could not save the video generation configuration.");
        return false;
    }
    return true;
}

bool VideoGenerationRecipe::load(const QDir &directory, VideoGenerationRecipe *recipe,
                                 QString *error)
{
    QFile file{directory.filePath(QStringLiteral("generation_config.json"))};
    if (!file.open(QIODevice::ReadOnly))
    {
        *error = QStringLiteral("The saved generation configuration is missing.");
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("version").toInt() != 1 || !object.value("settings").isObject()
        || !object.value("images").isArray() || object.value("generatorId").toString().isEmpty())
    {
        *error = QStringLiteral("The saved generation configuration is invalid.");
        return false;
    }
    VideoGenerationRecipe loaded;
    loaded.generatorId = object.value("generatorId").toString();
    loaded.prompt = object.value("prompt").toString();
    loaded.settings = object.value("settings").toObject().toVariantMap();
    // The browser can retry internally. Its successful prompt takes priority
    // over the initial prompt captured before launching the worker.
    QFile promptFile{directory.filePath(QStringLiteral("generation_prompt.txt"))};
    if (promptFile.open(QIODevice::ReadOnly))
    {
        loaded.prompt = QString::fromUtf8(promptFile.readAll()).trimmed();
    }
    if (loaded.prompt.isEmpty())
    {
        *error = QStringLiteral("The saved video prompt is empty.");
        return false;
    }
    for (const auto &value : object.value("images").toArray())
    {
        const QString relative = QDir::cleanPath(value.toString());
        if (!relative.startsWith(QStringLiteral("replay_inputs/"))
            || !QFileInfo(directory.filePath(relative)).isFile())
        {
            *error = QStringLiteral("A saved source image is missing: %1").arg(relative);
            return false;
        }
        loaded.imagePaths << directory.absoluteFilePath(relative);
    }
    *recipe = loaded;
    return true;
}
