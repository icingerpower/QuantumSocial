#ifndef ABSTRACTIMAGEGENERATOR_H
#define ABSTRACTIMAGEGENERATOR_H

#include <functional>

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <QCoro/QCoroTask>

class AbstractCli;

// Base class for prompt -> image(s) generation backends: one striking still
// image, or a coherent set for a slideshow.
//
// The only backend today is CLI-driven (ask an image-capable AI CLI to
// write the file(s) via its own file tools — the same contract already used
// by the project's image-regeneration/bootstrap step): generate() takes the
// AbstractCli to drive as a parameter, since unlike video generation (a
// distinct browser-automation engine) there is currently no non-CLI way to
// produce images. A future dedicated image API backend would simply ignore
// that parameter.
//
// Self-registering like AbstractSocialAccount/AbstractVideoGenerator:
// DECLARE_IMAGE_GENERATOR in the subclass's .cpp, discovery via
// ALL_IMAGE_GENERATORS().
class AbstractImageGenerator
{
public:
    struct Result
    {
        QStringList imagePaths;   // absolute paths of the produced image(s)
        QString errorMessage;
        bool rejected = false;    // the backend refused the prompt (content policy)
    };

    // One tunable setting of a backend — same shape as
    // AbstractVideoGenerator::SettingSpec, shown in the same Settings table.
    struct SettingSpec
    {
        QString key;
        QString label;
        QVariant defaultValue;
    };

    virtual ~AbstractImageGenerator() = default;

    // Unique id identifying the backend (e.g. "cli").
    virtual QString getId() const = 0;

    // Human-readable name (e.g. "AI CLI").
    virtual QString getName() const = 0;

    // The settings this backend understands. Default: none.
    virtual QList<SettingSpec> availableSettings() const;

    // Generates imageCount image(s) (1 for a single image, N for a
    // slideshow) from the prompt, optionally starting from reference
    // image(s), into outputDir. cli is the AI CLI to drive for CLI-backed
    // implementations (the only kind today) — chosen by the caller.
    // logProgress (optional) is called with a human-readable line whenever
    // something worth surfacing happens (e.g. a retry after the CLI flaked)
    // — CLI-backed implementations can be flaky per call and retry
    // internally, so the caller's single "Generating..." log line wouldn't
    // otherwise show that. recordLesson (optional) is called with a short,
    // reusable takeaway whenever a content-check failure pinpoints a
    // concrete, generalizable pitfall (e.g. "a full-body shot reduced the
    // product to an unreadable detail") — the caller persists these (see
    // PromptLessons) and feeds them back into future prompts.
    virtual QCoro::Task<Result> generate(const QString &prompt,
                                         const QStringList &referenceImagePaths,
                                         int imageCount, const QString &outputDir,
                                         const QVariantMap &settings,
                                         AbstractCli *cli,
                                         const std::function<void(const QString &)>
                                             &logProgress = {},
                                         const std::function<void(const QString &)>
                                             &recordLesson = {}) const = 0;

    // Returns all registered backends keyed by id.
    static const QMap<QString, AbstractImageGenerator *> &ALL_IMAGE_GENERATORS();

    // Used by DECLARE_IMAGE_GENERATOR to register a backend at startup.
    class Recorder
    {
    public:
        explicit Recorder(AbstractImageGenerator *generator);
    };

private:
    static QMap<QString, AbstractImageGenerator *> &getGenerators();
};

#define DECLARE_IMAGE_GENERATOR(NEW_CLASS)                                       \
    NEW_CLASS instance##NEW_CLASS;                                               \
    AbstractImageGenerator::Recorder recorder##NEW_CLASS{&instance##NEW_CLASS};

#endif // ABSTRACTIMAGEGENERATOR_H
