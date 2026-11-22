#include "InspirationAdaptationMethod.h"

#include "AbstractCli.h"

#include <algorithm>

namespace
{
QMap<QString, const InspirationAdaptationMethod *> &registry()
{
    static QMap<QString, const InspirationAdaptationMethod *> entries;
    return entries;
}

QString ensureVideoPrefix(const QString &raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
    {
        return QString{};
    }
    if (trimmed.startsWith(QLatin1String("Generate a video:"), Qt::CaseInsensitive))
    {
        return trimmed;
    }
    return QStringLiteral("Generate a video: ") + trimmed;
}

QString ensureTrailingPeriod(const QString &raw)
{
    const QString t = raw.trimmed();
    if (t.isEmpty())
    {
        return t;
    }
    if (t.endsWith(QLatin1Char('.')) || t.endsWith(QLatin1Char('!')) || t.endsWith(QLatin1Char('?')))
    {
        return t;
    }
    return t + QLatin1Char('.');
}

// Method 1 ("reference_swap"):
// Skips scene Properties completely.
// Step 1: Cleanly removes text/UI/artifacts from the reference image and replaces ONLY the product
//         with our exact real product (preserving exact pose, framing, outfit, setting, and lighting).
// Step 2: Animates that exact regenerated image with a concise, pure-motion prompt (zero conflicting
//         scene/model/outfit overrides).
class DirectSwapAnimateMethod final : public InspirationAdaptationMethod
{
public:
    QString id() const override
    {
        return QStringLiteral("reference_swap");
    }

    QString name() const override
    {
        return QStringLiteral("Direct Swap + Pure Motion (No Scene Properties)");
    }

    QString description() const override
    {
        return QStringLiteral(
            "Skips scene A/B properties: cleans text/artifacts and swaps our product into the "
            "inspiration frame, then animates that exact image with a short motion-only prompt.");
    }

    int sortOrder() const override
    {
        return 10;
    }

    bool requiresReferenceMedia() const override
    {
        return true;
    }

    bool usesSceneProperties() const override
    {
        return false;
    }

    bool appendsPropertiesToVideoPrompt() const override
    {
        return false;
    }

    QString plannerSlotInstruction(const PromptContext &ctx) const override
    {
        return QStringLiteral(
            "STRICT PRODUCT SWAP + PURE MOTION (NO SCENE OVERRIDES): Step 1 will preserve the exact "
            "scene, subject count, outfit, lighting, and camera framing of %1 while swapping in our "
            "product. For format=video, write a SHORT, SIMPLE 1-2 sentence motion-only \"prompt\" "
            "(25-40 words max, starting with \"Generate a video: \") that ONLY describes smooth "
            "natural movement starting from the regenerated image (matching the reference motion if "
            "multiple reference frames exist, or subtle ankle/weight shift + gentle camera glide if "
            "single frame) while keeping the exact scene, subject, outfit, and shoes unchanged.")
            .arg(ctx.referenceFrames.isEmpty()
                     ? QStringLiteral("the reference scene")
                     : ctx.referenceFrames.join(QStringLiteral(", ")));
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
        QString prompt = marker + QStringLiteral(
            "You are given TWO image files in the current working directory:\n"
            "1. '%1' — the REFERENCE SCENE (captured from a social-media inspiration post). "
            "Use this for the exact scene composition, environment/background, lighting, camera "
            "angle, framing, model pose, and outfit styling.\n"
            "2. '%2' — OUR REAL PRODUCT%3. This is the EXACT product that must appear in the "
            "generated image.\n\n"
            "Generate a NEW high-quality starting frame for a %4 social-media video by "
            "REPLACING the featured shoes/product in '%1' with OUR REAL PRODUCT from '%2':\n"
            "- Scene & composition: preserve the exact setting, subject count, camera angle, framing, "
            "lighting, mood, model pose, and outfit styling from '%1', adapted cleanly to a full %4 "
            "canvas (no letterboxing, pillarboxing, or blank bars).\n"
            "- Product replacement (NON-NEGOTIABLE): completely remove the original shoes/product "
            "shown in '%1' and replace them with the EXACT product from '%2' — matching its exact "
            "silhouette, heel shape and height, straps/closures, print/pattern, color, material, "
            "and hardware details. Do NOT blend features of the reference product with our product.\n"
            "- Clean starting frame for video animation: remove all social network icons, UI "
            "overlays, watermarks, captions, logos, and text.\n")
            .arg(ctx.referenceSceneFile,
                 ctx.productImageFile,
                 ctx.productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(ctx.productSummary),
                 ctx.videoFormatLabel);
        if (!ctx.adaptationNotes.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Adaptation notes: %1\n").arg(ctx.adaptationNotes.trimmed());
        }
        if (!ctx.refinementFixNote.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Specific refinement adjustment: %1\n").arg(ctx.refinementFixNote.trimmed());
        }
        prompt += QStringLiteral(
            "Save the result in the current working directory as exactly '%1'.\n"
            "Do not modify the input files. Reply with just the output file name.")
            .arg(ctx.outputImageFile);
        return prompt;
    }

    QString finalizeVideoAnimationPrompt(const QString &plannedPrompt,
                                         const PromptContext &ctx) const override
    {
        if (!plannedPrompt.trimmed().isEmpty())
        {
            return ensureTrailingPeriod(ensureVideoPrefix(plannedPrompt));
        }
        const bool fromMultiFrameVideo = ctx.referenceFrames.size() >= 2;
        return fromMultiFrameVideo
            ? QStringLiteral(
                  "Generate a video: Smoothly animate this exact starting image with fluid, natural "
                  "movement matching the reference pacing, keeping the exact scene, subject, outfit, "
                  "lighting, and %1 unchanged with no cuts and no on-screen text.")
                  .arg(ctx.productSummary.isEmpty() ? QStringLiteral("shoes") : ctx.productSummary)
            : QStringLiteral(
                  "Generate a video: Smoothly animate this exact starting image with subtle, natural "
                  "motion — a gentle ankle turn and poised weight shift catching the light on the %1 "
                  "with a slow, steady camera glide — keeping the exact scene, subject, outfit, and "
                  "shoes unchanged with no cuts and no text.")
                  .arg(ctx.productSummary.isEmpty() ? QStringLiteral("heels") : ctx.productSummary);
    }

    QString buildStillOrCarouselPrompt(const QString &plannedPrompt,
                                       const PromptContext &ctx,
                                       bool *outAttachReferenceScene) const override
    {
        if (outAttachReferenceScene)
        {
            *outAttachReferenceScene = !ctx.referenceSceneFile.isEmpty();
        }
        return QStringLiteral(
            "Recreate the exact scene composition, pose, lighting, and setting of '%1' (removing any "
            "text or UI icons), completely replacing its featured product with our exact real product "
            "shown in '%2' (%3). %4")
            .arg(ctx.referenceSceneFile, ctx.productImageFile, ctx.productSummary, plannedPrompt.trimmed());
    }
};

// Method 2 ("reference_variant"):
// Uses scene Properties ONLY in Step 1 (when regenerating the starting image from the reference +
// product), then animates that regenerated image with a short pure-motion prompt in Step 2 so the
// video prompt never contradicts the starting image.
class VariantInImageThenAnimateMethod final : public InspirationAdaptationMethod
{
public:
    QString id() const override
    {
        return QStringLiteral("reference_variant");
    }

    QString name() const override
    {
        return QStringLiteral("Image Variant + Pure Motion (Properties in Step 1 Only)");
    }

    QString description() const override
    {
        return QStringLiteral(
            "Applies A/B scene/lighting/styling tweaks during Step 1 image regeneration while keeping "
            "the inspiration's hook pose & camera angle, then animates that image with a simple motion prompt.");
    }

    int sortOrder() const override
    {
        return 20;
    }

    bool requiresReferenceMedia() const override
    {
        return true;
    }

    bool usesSceneProperties() const override
    {
        return true;
    }

    bool appendsPropertiesToVideoPrompt() const override
    {
        return false;
    }

    QString plannerSlotInstruction(const PromptContext &ctx) const override
    {
        Q_UNUSED(ctx);
        return QStringLiteral(
            "IMAGE VARIANT IN STEP 1 + PURE MOTION IN STEP 2: Step 1 will combine the inspiration's "
            "camera angle/pose with the slot's A/B properties to generate the starting image. For "
            "format=video, write a SHORT, SIMPLE 1-2 sentence motion-only \"prompt\" (25-45 words max, "
            "starting with \"Generate a video: \") that animates the regenerated starting image "
            "smoothly without contradicting its scene or subjects.");
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
        QString prompt = marker + QStringLiteral(
            "You are given TWO image files in the current working directory:\n"
            "1. '%1' — the REFERENCE SCENE (captured from a winning social-media inspiration post).\n"
            "2. '%2' — OUR REAL PRODUCT%3. This is the EXACT product that must appear in the generated image.\n\n"
            "Generate a NEW high-quality starting frame for a %4 social-media video by keeping the core "
            "winning visual hook of '%1' while introducing a tasteful sibling variation:\n"
            "- Keep from '%1': the flattering camera angle, leg/body framing, pose energy, and prominent "
            "visual focus on the featured product.\n"
            "- Controlled variation (change 'a little more' than just the product): refresh the "
            "complementary backdrop/setting, lighting mood, or outfit styling so this feels like a "
            "distinct sibling creative%5.\n"
            "- Product accuracy (NON-NEGOTIABLE): completely replace the reference shoes/product with "
            "OUR EXACT REAL PRODUCT from '%2' — matching its exact silhouette, heel shape/height, straps, "
            "print/pattern, color, material, and hardware.\n"
            "- Clean starting frame for video animation: full %4 canvas, zero social UI icons, watermarks, "
            "logos, or overlaid text.\n")
            .arg(ctx.referenceSceneFile,
                 ctx.productImageFile,
                 ctx.productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(ctx.productSummary),
                 ctx.videoFormatLabel,
                 ctx.propertyFragments.trimmed().isEmpty()
                     ? QString{}
                     : QStringLiteral(" guided by: %1").arg(ctx.propertyFragments.trimmed()));
        if (!ctx.adaptationNotes.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Adaptation notes: %1\n").arg(ctx.adaptationNotes.trimmed());
        }
        if (!ctx.refinementFixNote.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Specific refinement adjustment: %1\n").arg(ctx.refinementFixNote.trimmed());
        }
        prompt += QStringLiteral(
            "Save the result in the current working directory as exactly '%1'.\n"
            "Do not modify the input files. Reply with just the output file name.")
            .arg(ctx.outputImageFile);
        return prompt;
    }

    QString finalizeVideoAnimationPrompt(const QString &plannedPrompt,
                                         const PromptContext &ctx) const override
    {
        if (!plannedPrompt.trimmed().isEmpty())
        {
            return ensureTrailingPeriod(ensureVideoPrefix(plannedPrompt));
        }
        return QStringLiteral(
            "Generate a video: Smoothly animate this exact starting image with fluid, natural "
            "movement and a steady editorial camera glide showcasing %1, keeping the scene, styling, "
            "and exact product design consistent throughout with no cuts and no text.")
            .arg(ctx.productSummary.isEmpty() ? QStringLiteral("the featured heels") : ctx.productSummary);
    }

    QString buildStillOrCarouselPrompt(const QString &plannedPrompt,
                                       const PromptContext &ctx,
                                       bool *outAttachReferenceScene) const override
    {
        if (outAttachReferenceScene)
        {
            *outAttachReferenceScene = !ctx.referenceSceneFile.isEmpty();
        }
        QString out = QStringLiteral(
            "Keep the winning camera angle, leg/body framing, and visual hook energy of '%1', "
            "introduce a fresh complementary backdrop/lighting/outfit variation, and completely "
            "replace the featured product with our exact real product shown in '%2' (%3). %4")
            .arg(ctx.referenceSceneFile, ctx.productImageFile, ctx.productSummary, plannedPrompt.trimmed());
        if (!ctx.propertyFragments.trimmed().isEmpty())
        {
            out += QStringLiteral(" ") + ensureTrailingPeriod(ctx.propertyFragments);
        }
        return out;
    }
};

// Method 3 ("reference_properties_full"):
// Full Properties-driven scene & motion (inspired by the reference's hook concept, while both Step 1
// and Step 2 explicitly apply all sampled A/B Properties from the Properties tree so Step 1 and
// Step 2 agree 100% on the scene, model count, lighting, and styling).
class PropertiesGuidedFullMethod final : public InspirationAdaptationMethod
{
public:
    QString id() const override
    {
        return QStringLiteral("reference_properties_full");
    }

    QString name() const override
    {
        return QStringLiteral("Full A/B Properties Scene + Motion");
    }

    QString description() const override
    {
        return QStringLiteral(
            "Builds both the starting frame and the video animation prompt directly from the "
            "sampled A/B Properties tree (inspired by the reference's pacing/hook).");
    }

    int sortOrder() const override
    {
        return 30;
    }

    bool requiresReferenceMedia() const override
    {
        return true;
    }

    bool usesSceneProperties() const override
    {
        return true;
    }

    bool appendsPropertiesToVideoPrompt() const override
    {
        return true;
    }

    QString plannerSlotInstruction(const PromptContext &ctx) const override
    {
        Q_UNUSED(ctx);
        return QStringLiteral(
            "FULL A/B PROPERTIES SCENE + MOTION: Build a cohesive scene and smooth camera/model "
            "motion matching ALL listed A/B properties below, inspired by the reference's hook energy.");
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
        QString prompt = marker + QStringLiteral(
            "You are given '%1' — OUR REAL PRODUCT%2 — in the current working directory.\n"
            "Generate a scroll-stopping %3 fashion-editorial starting frame for a social-media video "
            "that follows EVERY ONE of these scene & styling specifications%4:\n"
            "- Product accuracy (NON-NEGOTIABLE): feature our exact product from '%1' prominently "
            "(matching its exact silhouette, heel shape/height, straps, print/pattern, color, material).\n"
            "- Clean starting frame: full %3 canvas, zero watermarks, social icons, or text.\n")
            .arg(ctx.productImageFile,
                 ctx.productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(ctx.productSummary),
                 ctx.videoFormatLabel,
                 ctx.propertyFragments.trimmed().isEmpty()
                     ? QString{}
                     : QStringLiteral(":\n- Scene & A/B properties: %1").arg(ctx.propertyFragments.trimmed()));
        if (!ctx.adaptationNotes.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Adaptation notes: %1\n").arg(ctx.adaptationNotes.trimmed());
        }
        if (!ctx.refinementFixNote.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Specific refinement adjustment: %1\n").arg(ctx.refinementFixNote.trimmed());
        }
        prompt += QStringLiteral(
            "Save the result in the current working directory as exactly '%1'.\n"
            "Do not modify the original file. Reply with just the output file name.")
            .arg(ctx.outputImageFile);
        return prompt;
    }

    QString finalizeVideoAnimationPrompt(const QString &plannedPrompt,
                                         const PromptContext &ctx) const override
    {
        QString base = plannedPrompt.trimmed().isEmpty()
            ? QStringLiteral("Generate a video: Smoothly animate the starting image with confident "
                             "editorial motion showcasing %1")
                  .arg(ctx.productSummary.isEmpty() ? QStringLiteral("our product") : ctx.productSummary)
            : ensureVideoPrefix(plannedPrompt);
        base = ensureTrailingPeriod(base);
        if (!ctx.propertyFragments.trimmed().isEmpty())
        {
            base += QStringLiteral(" ") + ensureTrailingPeriod(ctx.propertyFragments);
        }
        return base;
    }

    QString buildStillOrCarouselPrompt(const QString &plannedPrompt,
                                       const PromptContext &ctx,
                                       bool *outAttachReferenceScene) const override
    {
        if (outAttachReferenceScene)
        {
            *outAttachReferenceScene = false;
        }
        QString out = ensureTrailingPeriod(plannedPrompt);
        if (!ctx.propertyFragments.trimmed().isEmpty())
        {
            out += QStringLiteral(" ") + ensureTrailingPeriod(ctx.propertyFragments);
        }
        return out.trimmed();
    }
};

// Method 4 ("editorial_ab"):
// Used when no reference media exists (e.g., URL-only reference or 0 approved references yet).
class EditorialPropertiesOnlyMethod final : public InspirationAdaptationMethod
{
public:
    QString id() const override
    {
        return QStringLiteral("editorial_ab");
    }

    QString name() const override
    {
        return QStringLiteral("Pure Product + A/B Properties (No Reference Media)");
    }

    QString description() const override
    {
        return QStringLiteral(
            "Generates a fresh product-centric scene and animation guided by the A/B Properties tree.");
    }

    int sortOrder() const override
    {
        return 90;
    }

    bool requiresReferenceMedia() const override
    {
        return false;
    }

    bool usesSceneProperties() const override
    {
        return true;
    }

    bool appendsPropertiesToVideoPrompt() const override
    {
        return true;
    }

    QString plannerSlotInstruction(const PromptContext &ctx) const override
    {
        Q_UNUSED(ctx);
        return QStringLiteral(
            "PURE EDITORIAL A/B: Build a fresh fashion-editorial scene showcasing our product and "
            "matching the listed A/B properties.");
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        return PropertiesGuidedFullMethod{}.buildVideoStartingFramePrompt(ctx);
    }

    QString finalizeVideoAnimationPrompt(const QString &plannedPrompt,
                                         const PromptContext &ctx) const override
    {
        return PropertiesGuidedFullMethod{}.finalizeVideoAnimationPrompt(plannedPrompt, ctx);
    }

    QString buildStillOrCarouselPrompt(const QString &plannedPrompt,
                                       const PromptContext &ctx,
                                       bool *outAttachReferenceScene) const override
    {
        return PropertiesGuidedFullMethod{}.buildStillOrCarouselPrompt(
            plannedPrompt, ctx, outAttachReferenceScene);
    }
};

const DirectSwapAnimateMethod directSwapMethod;
const VariantInImageThenAnimateMethod variantInImageMethod;
const PropertiesGuidedFullMethod propertiesFullMethod;
const EditorialPropertiesOnlyMethod editorialOnlyMethod;

const InspirationAdaptationMethod::Recorder directSwapRecorder(&directSwapMethod);
const InspirationAdaptationMethod::Recorder variantInImageRecorder(&variantInImageMethod);
const InspirationAdaptationMethod::Recorder propertiesFullRecorder(&propertiesFullMethod);
const InspirationAdaptationMethod::Recorder editorialOnlyRecorder(&editorialOnlyMethod);
} // namespace

const QMap<QString, const InspirationAdaptationMethod *> &InspirationAdaptationMethod::all()
{
    return registry();
}

InspirationAdaptationMethod::Recorder::Recorder(const InspirationAdaptationMethod *method)
{
    if (method && !method->id().isEmpty())
    {
        registry().insert(method->id(), method);
    }
}

QList<const InspirationAdaptationMethod *> InspirationAdaptationMethod::orderedMethods(bool hasReferenceMedia)
{
    QList<const InspirationAdaptationMethod *> list;
    for (const InspirationAdaptationMethod *m : registry())
    {
        if (!m)
        {
            continue;
        }
        if (m->requiresReferenceMedia() && !hasReferenceMedia)
        {
            continue;
        }
        if (!m->requiresReferenceMedia() && hasReferenceMedia)
        {
            continue;
        }
        list.append(m);
    }
    std::sort(list.begin(), list.end(),
              [](const InspirationAdaptationMethod *a, const InspirationAdaptationMethod *b) {
                  if (a->sortOrder() != b->sortOrder())
                  {
                      return a->sortOrder() < b->sortOrder();
                  }
                  return a->id() < b->id();
              });
    return list;
}

const InspirationAdaptationMethod *InspirationAdaptationMethod::selectForSlot(
    bool hasReferenceMedia,
    int slotIndex,
    int priorUsesOfReference,
    bool isExploration,
    const QString &referenceBestMethodId,
    const QString &globalWinningMethodId)
{
    const QList<const InspirationAdaptationMethod *> candidates = orderedMethods(hasReferenceMedia);
    if (candidates.isEmpty())
    {
        return registry().value(QStringLiteral("editorial_ab"), nullptr);
    }
    if (candidates.size() == 1)
    {
        return candidates.first();
    }

    const QString preferredId = !referenceBestMethodId.isEmpty()
        ? referenceBestMethodId
        : globalWinningMethodId;
    const InspirationAdaptationMethod *preferred = registry().value(preferredId, nullptr);
    if (preferred && !candidates.contains(preferred))
    {
        preferred = nullptr;
    }

    // Exploration slots or every 3rd slot when a winner exists: rotate across all registered methods
    // so new or under-sampled Recorder methods always keep getting measured.
    if (isExploration || !preferred || ((slotIndex + priorUsesOfReference) % 3 == 2))
    {
        if (preferred)
        {
            QList<const InspirationAdaptationMethod *> others;
            for (const auto *c : candidates)
            {
                if (c != preferred)
                {
                    others.append(c);
                }
            }
            if (!others.isEmpty())
            {
                return others[(slotIndex + priorUsesOfReference) % others.size()];
            }
        }
        return candidates[(slotIndex + priorUsesOfReference) % candidates.size()];
    }

    return preferred;
}
