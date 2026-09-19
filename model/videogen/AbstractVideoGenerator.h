#ifndef ABSTRACTVIDEOGENERATOR_H
#define ABSTRACTVIDEOGENERATOR_H

#include <memory>

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <QCoro/QCoroTask>

class QProcess;

// Base class for text/image -> video generation backends.
//
// The first backend drives Gemini through a browser; future ones may call
// HTTP APIs instead (e.g. the ByteDance/Seedance API) — an implementation is
// free to do either, callers only ever see this interface and the registry,
// so adding a backend never changes generic code (same self-registering
// pattern as AbstractSocialAccount and AbstractCli: DECLARE_VIDEO_GENERATOR
// in the subclass's .cpp, discovery via ALL_VIDEO_GENERATORS()).
class AbstractVideoGenerator
{
public:
    struct Result
    {
        QString videoPath;      // generated file; empty on failure
        QString errorMessage;
        // true when the backend refused the prompt (content policy) — the
        // caller can retry with a slightly modified prompt, unlike ordinary
        // failures (timeouts, crashed browser...).
        bool rejected = false;
        // False for local setup errors (for example a missing browser
        // dependency): changing the prompt and launching another worker can
        // never fix those, so the workflow must stop after the first result.
        bool retryable = true;
        // The browser/worker connection was lost. A repeat can retry the
        // same inputs after relaunch without treating this as a refusal.
        bool browserLost = false;
    };

    // One tunable setting of a backend. Each backend declares its own list;
    // TableGenerationSettings turns them into an editable table persisted in
    // the working directory and hands the current values back to generate().
    struct SettingSpec
    {
        QString key;            // e.g. "maxTriesPerPrompt"
        QString label;          // human-readable description
        QVariant defaultValue;  // also defines the value's type
    };

    // Both defined out-of-line (in the .cpp, where QProcess is complete) so
    // this header can keep QProcess merely forward-declared despite the
    // unique_ptr member below — an inline/implicit one would need the full
    // type in every subclass's translation unit.
    AbstractVideoGenerator();
    virtual ~AbstractVideoGenerator();

    // Unique id identifying the backend (e.g. "gemini-browser").
    virtual QString getId() const = 0;

    // Human-readable name (e.g. "Gemini (browser)").
    virtual QString getName() const = 0;

    // The settings this backend understands; they differ per backend, so the
    // settings pane builds itself from this. Default: none.
    virtual QList<SettingSpec> availableSettings() const;

    // Generates one video from the prompt — optionally starting from input
    // image(s) — into outputDir. `settings` holds the current values for
    // this backend's availableSettings() keys. May take many minutes
    // (browser backends can also wait for an interactive login on first
    // run).
    virtual QCoro::Task<Result> generate(const QString &prompt,
                                         const QStringList &imagePaths,
                                         const QString &outputDir,
                                         const QVariantMap &settings) const = 0;

    // Returns all registered backends keyed by id.
    static const QMap<QString, AbstractVideoGenerator *> &ALL_VIDEO_GENERATORS();

    // Used by DECLARE_VIDEO_GENERATOR to register a backend at startup.
    class Recorder
    {
    public:
        explicit Recorder(AbstractVideoGenerator *generator);
    };

protected:
    // Shared infrastructure for script-driven backends: writes the prompt to
    // <outputDir>/generation_prompt.txt (argv has size limits, prompts do
    // not), then talks to a LONG-LIVED worker process running the given
    // Python script (must live next to this file, in model/videogen/),
    // started as `<script> --worker` on first use and kept alive afterwards.
    // One '\n'-terminated JSON request per call:
    //     {"promptFile":..., "outputDir":..., "settings":{...}, "images":[...]}
    // answered by one '\n'-terminated JSON line:
    //     {"video": <path|null>, "error": <string|null>, "rejected": <bool>}
    //
    // Long-lived on purpose: a browser-driven backend then keeps the SAME
    // browser (and login) across every retry, instead of the previous
    // one-process-per-attempt model that popped a fresh window on each try
    // and raced the previous Chrome's profile lock while it was still
    // shutting down. A worker that dies is simply relaunched on the next
    // call.
    QCoro::Task<Result> runGeneratorScript(const QString &scriptFileName,
                                           const QString &prompt,
                                           const QStringList &imagePaths,
                                           const QString &outputDir,
                                           const QVariantMap &settings) const;

private:
    // This backend's worker (see runGeneratorScript). mutable because
    // generate() — and so runGeneratorScript() — is const on what are
    // process-wide singletons.
    mutable std::unique_ptr<QProcess> m_worker;

    static QMap<QString, AbstractVideoGenerator *> &getGenerators();
};

#define DECLARE_VIDEO_GENERATOR(NEW_CLASS)                                       \
    NEW_CLASS instance##NEW_CLASS;                                               \
    AbstractVideoGenerator::Recorder recorder##NEW_CLASS{&instance##NEW_CLASS};

#endif // ABSTRACTVIDEOGENERATOR_H
