#ifndef INSPIRATIONADAPTATIONMETHOD_H
#define INSPIRATIONADAPTATIONMETHOD_H

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// Extensible strategy interface (self-registered via InspirationAdaptationMethod::Recorder)
// defining how an approved social-media reference + product (+ optional A/B scene properties)
// are turned into a regenerated starting image/carousel and a video animation prompt.
//
// Adding a new method to A/B test over time requires only defining a subclass and
// instantiating `const InspirationAdaptationMethod::Recorder rec(&instance);` — both
// Auto-Batch generation and SocialStatistics::summarizeReferenceExperiment discover
// and measure all registered methods automatically via InspirationAdaptationMethod::all().
class InspirationAdaptationMethod
{
public:
    struct PromptContext
    {
        QString productImageFile;   // e.g. "my_product.jpg"
        QString productSummary;     // e.g. "Pradize · Cheetah · Leopard Print Slingback Pumps"
        QString referenceSceneFile; // e.g. "reference_scene.png" (empty if no local media)
        QStringList referenceFrames;
        QString referenceTitle;
        QString adaptationNotes;
        QString videoFormatLabel;   // e.g. "vertical (9:16)"
        QString outputImageFile;    // e.g. "generation_source.png"
        QString propertyFragments;  // Joined A/B property prompt fragments (when usesSceneProperties() is true)
        QString refinementFixNote;  // Optional round-2 AI vision quality-gate instruction
    };

    virtual ~InspirationAdaptationMethod() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    virtual QString description() const = 0;
    virtual int sortOrder() const { return 100; }

    virtual bool requiresReferenceMedia() const { return true; }
    virtual bool supportsReferenceMedia() const { return requiresReferenceMedia(); }
    virtual bool supportsWithoutReferenceMedia() const { return !requiresReferenceMedia(); }
    virtual bool usesSceneProperties() const = 0;
    virtual bool appendsPropertiesToVideoPrompt() const { return false; }
    virtual bool isTwoStepVideoStrategy() const { return false; }

    virtual QString plannerSlotInstruction(const PromptContext &ctx) const = 0;
    virtual QString buildVideoStartingFramePrompt(const PromptContext &ctx) const = 0;
    virtual QString finalizeVideoAnimationPrompt(const QString &plannedPrompt,
                                                 const PromptContext &ctx) const = 0;
    virtual QString finalizeSecondStepVideoPrompt(const QString &plannedSecondPrompt,
                                                  const PromptContext &ctx) const
    {
        Q_UNUSED(plannedSecondPrompt);
        Q_UNUSED(ctx);
        return QString{};
    }
    virtual QString buildStillOrCarouselPrompt(const QString &plannedPrompt,
                                               const PromptContext &ctx,
                                               bool *outAttachReferenceScene) const = 0;

    static const QMap<QString, const InspirationAdaptationMethod *> &all();
    static QList<const InspirationAdaptationMethod *> orderedMethods(
        bool hasReferenceMedia, bool isVideoFormat = true);
    static const InspirationAdaptationMethod *selectForSlot(
        bool hasReferenceMedia,
        int slotIndex,
        int priorUsesOfReference,
        bool isExploration,
        const QString &referenceBestMethodId,
        const QString &globalWinningMethodId,
        bool isVideoFormat = true);

    class Recorder
    {
    public:
        explicit Recorder(const InspirationAdaptationMethod *method);
    };
};

#endif // INSPIRATIONADAPTATIONMETHOD_H
