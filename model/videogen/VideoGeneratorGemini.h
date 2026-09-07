#ifndef VIDEOGENERATORGEMINI_H
#define VIDEOGENERATORGEMINI_H

#include "AbstractVideoGenerator.h"

// Generates videos with Gemini (Veo) driven through a browser — the web app
// is used rather than an API so an existing Google subscription is enough.
class VideoGeneratorGemini : public AbstractVideoGenerator
{
public:
    QString getId() const override;
    QString getName() const override;
    QList<SettingSpec> availableSettings() const override;
    QCoro::Task<Result> generate(const QString &prompt,
                                 const QStringList &imagePaths,
                                 const QString &outputDir,
                                 const QVariantMap &settings) const override;
};

#endif // VIDEOGENERATORGEMINI_H
