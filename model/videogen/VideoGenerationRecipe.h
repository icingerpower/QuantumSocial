#ifndef VIDEOGENERATIONRECIPE_H
#define VIDEOGENERATIONRECIPE_H

#include <QDir>
#include <QStringList>
#include <QVariantMap>

// Portable snapshot stored beside generation_prompt.txt in a generation's temp/.
struct VideoGenerationRecipe
{
    QString generatorId;
    QString prompt;
    QStringList imagePaths;
    QVariantMap settings;

    bool save(const QDir &directory, QString *error) const;
    static bool load(const QDir &directory, VideoGenerationRecipe *recipe, QString *error);
};

#endif
