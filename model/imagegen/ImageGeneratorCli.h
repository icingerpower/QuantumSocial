#ifndef IMAGEGENERATORCLI_H
#define IMAGEGENERATORCLI_H

#include "AbstractImageGenerator.h"

// Generates image(s) by asking an image-capable AI CLI to write the file(s)
// with its own file tools — the CLI to drive is chosen by the caller (the
// generation plan dialog) and passed to generate().
class ImageGeneratorCli : public AbstractImageGenerator
{
public:
    QString getId() const override;
    QString getName() const override;
    QCoro::Task<Result> generate(const QString &prompt,
                                 const QStringList &referenceImagePaths,
                                 int imageCount, const QString &outputDir,
                                 const QVariantMap &settings,
                                 AbstractCli *cli,
                                 const std::function<void(const QString &)>
                                     &logProgress = {},
                                 const std::function<void(const QString &)>
                                     &recordLesson = {}) const override;
};

#endif // IMAGEGENERATORCLI_H
