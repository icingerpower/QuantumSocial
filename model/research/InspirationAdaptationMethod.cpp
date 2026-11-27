#include "InspirationAdaptationMethod.h"

#include "AbstractCli.h"

#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
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
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Seasonal & weather coherence: %1\n").arg(ctx.seasonalGuidance.trimmed());
        }
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
        QString out = QStringLiteral(
            "Recreate the exact scene composition, pose, lighting, and setting of '%1' (removing any "
            "text or UI icons), completely replacing its featured product with our exact real product "
            "shown in '%2' (%3). %4")
            .arg(ctx.referenceSceneFile, ctx.productImageFile, ctx.productSummary, plannedPrompt.trimmed());
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            out += QStringLiteral(" ") + ensureTrailingPeriod(ctx.seasonalGuidance);
        }
        return out.trimmed();
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
            "camera angle/pose with the slot's A/B properties to generate the starting image (baking any "
            "model/hair/wardrobe/setting changes directly into the image). For format=video, write a "
            "SHORT, SIMPLE 1-2 sentence motion-only \"prompt\" (25-45 words max, starting with "
            "\"Generate a video: \") that animates the regenerated starting image smoothly — NEVER ask "
            "the video model to change or morph the model's hair, identity, or outfit mid-video.");
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
            "- Controlled variation (bake 100% into this starting frame): refresh the backdrop/setting, "
            "lighting mood, model appearance/hair, or outfit styling so this feels like a distinct "
            "sibling creative%5. If any model/hair/wardrobe/setting change is requested, apply it "
            "COMPLETELY in this image so zero conflicting traits from '%1' or '%2' remain.\n"
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
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Seasonal & weather coherence: %1\n").arg(ctx.seasonalGuidance.trimmed());
        }
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
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            out += QStringLiteral(" ") + ensureTrailingPeriod(ctx.seasonalGuidance);
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
            "motion matching ALL listed A/B properties below, inspired by the reference's hook energy. "
            "Step 1 already bakes the target model/hair/wardrobe/setting into the starting image — keep "
            "that exact subject and outfit consistent from frame 0 to the end without morphing mid-clip.");
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
        QString prompt = marker + QStringLiteral(
            "You are given '%1' — OUR REAL PRODUCT%2 — in the current working directory.\n"
            "Generate a brand-new, scroll-stopping %3 fashion-editorial starting frame for a social-media "
            "video that follows EVERY ONE of these scene & styling specifications%4:\n"
            "- Product accuracy (NON-NEGOTIABLE): extract ONLY the product design from '%1' and feature "
            "our exact product prominently (matching its exact silhouette, heel shape/height, straps, "
            "print/pattern, color, material). Completely replace any original model, legs, wardrobe, or "
            "studio backdrop that may appear in '%1' with the new scene and subject specified here.\n"
            "- Clean starting frame: full %3 canvas, zero watermarks, social icons, or text.\n")
            .arg(ctx.productImageFile,
                 ctx.productSummary.isEmpty() ? QString{} : QStringLiteral(" (%1)").arg(ctx.productSummary),
                 ctx.videoFormatLabel,
                 ctx.propertyFragments.trimmed().isEmpty()
                     ? QString{}
                     : QStringLiteral(":\n- Scene & A/B properties (bake 100% into this image): %1")
                           .arg(ctx.propertyFragments.trimmed()));
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Seasonal & weather coherence: %1\n").arg(ctx.seasonalGuidance.trimmed());
        }
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
        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            out += QStringLiteral(" ") + ensureTrailingPeriod(ctx.seasonalGuidance);
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

// Method 5 ("two_step_reveal"):
// Works both WITH Research inspirations and WITHOUT reference media (using A/B Properties):
// - Step 0 (Image CLI): Regenerates a clean neck-down (collarbone-to-shoes, faceless) 9:16 editorial
//   starting frame that bakes in the complete target outfit, model styling, lighting, setting, and
//   exact catalog product (avoiding Veo's real-person face filter AND eliminating any 1-2s flash of
//   an un-updated model).
// - Step 1 Video (~10s, fresh Gemini chat kept open): Slow-motion close-up camera reveal rising
//   smoothly from the shoes/heels at the bottom up to the collarbone/neck.
// - Step 2 Video (~10s continuation in the same Gemini chat, 0 uploaded images): Dynamic multi-camera
//   cuts alternating between crisp shoe close-ups in motion and full-body shots of the model in full.
class TwoStepRevealMethod final : public InspirationAdaptationMethod
{
public:
    QString id() const override
    {
        return QStringLiteral("two_step_reveal");
    }

    QString name() const override
    {
        return QStringLiteral("2-Step Reveal: Slow-Mo Bottom-to-Top + Multi-Angle Full Look");
    }

    QString description() const override
    {
        return QStringLiteral(
            "2-step multi-video sequence: (1) slow-motion close-up reveal rising from the product at "
            "the bottom up to the neck, followed in the same Gemini conversation by (2) dynamic "
            "camera cuts alternating crisp close-ups and the model in full.");
    }

    int sortOrder() const override
    {
        return 95;
    }

    bool requiresReferenceMedia() const override
    {
        return false;
    }

    bool supportsReferenceMedia() const override
    {
        return true;
    }

    bool supportsWithoutReferenceMedia() const override
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

    bool isTwoStepVideoStrategy() const override
    {
        return true;
    }

    QString plannerSlotInstruction(const PromptContext &ctx) const override
    {
        Q_UNUSED(ctx);
        return QStringLiteral(
            "TWO-STEP MULTI-VIDEO REVEAL (Step 1 Slow-Mo Bottom-to-Top Close-Up -> Step 2 Alternating "
            "Close-Up & Full-Body Views): Step 0 regenerates a neck-down (collarbone-to-shoes, no face) "
            "starting frame baking in all A/B properties, outfit, and our exact product. Write BOTH:\n"
            "    * \"prompt\" (Step 1 video, ~10s): starting with \"Generate a video: \", describe a "
            "slow-motion close-up bottom-to-top camera reveal of the woman walking gracefully forward, "
            "starting tightly framed on the shoes/feet at the bottom and gliding smoothly upward along "
            "the legs and outfit up to the collarbone/neck (never morphing model/outfit mid-clip).\n"
            "    * \"step2Prompt\" (Step 2 continuation in same chat, ~10s): starting with "
            "\"Generate a video: In the next video, \", describe 3 dynamic alternating camera views of "
            "her walking — alternating between crisp low-angle close-ups of the shoes in motion and "
            "full-body head-to-toe shots of the stylish model in full outfit and heels, keeping the "
            "exact same shoes, outfit, lighting, and setting.");
    }

    QString buildVideoStartingFramePrompt(const PromptContext &ctx) const override
    {
        const QString marker = QString::fromLatin1(RASTER_IMAGE_PROMPT_MARKER) + QStringLiteral("\n");
        const QString prodLabel = ctx.productSummary.isEmpty()
            ? QString{}
            : QStringLiteral(" (%1)").arg(ctx.productSummary);
        QString prompt = marker;
        if (!ctx.referenceSceneFile.isEmpty())
        {
            prompt += QStringLiteral(
                "You are given TWO image files in the current working directory:\n"
                "1. '%1' — the REFERENCE SCENE (use for luxury mood, floor/runway vibe, and styling energy).\n"
                "2. '%2' — OUR REAL PRODUCT%3. This is the EXACT product that must appear on the model's feet.\n\n")
                .arg(ctx.referenceSceneFile, ctx.productImageFile, prodLabel);
        }
        else
        {
            prompt += QStringLiteral(
                "You are given '%1' — OUR REAL PRODUCT%2 — in the current working directory.\n\n")
                .arg(ctx.productImageFile, prodLabel);
        }

        prompt += QStringLiteral(
            "Generate a brand-new, scroll-stopping %1 fashion-editorial starting frame for a 2-step "
            "reveal video:\n"
            "- Framing & refusal-safe composition (CRITICAL): frame the stylish woman walking forward "
            "from COLLARBONE / NECK DOWN TO HER SHOES (neck-to-toe framing — NO face or head visible in "
            "this starting image) so both the complete outfit silhouette AND the featured shoes at the "
            "bottom are clearly established for video continuation.\n"
            "- Bake ALL subject & scene updates into this image: completely generate a fresh model and "
            "outfit%2 — leave ZERO trace of any prior model, original legs, or unwanted wardrobe from "
            "the input file(s).\n"
            "- Product accuracy (NON-NEGOTIABLE): feature OUR EXACT REAL PRODUCT from '%3' on her feet "
            "in crisp focus — matching its exact silhouette, heel shape and height, straps/closures, "
            "print/pattern, color, material, and hardware.\n"
            "- Clean starting frame: full %1 canvas, zero social UI icons, watermarks, logos, or text.\n")
            .arg(ctx.videoFormatLabel,
                 ctx.propertyFragments.trimmed().isEmpty()
                     ? QString{}
                     : QStringLiteral(" matching these A/B scene & styling properties: %1")
                           .arg(ctx.propertyFragments.trimmed()),
                 ctx.productImageFile);

        if (!ctx.seasonalGuidance.trimmed().isEmpty())
        {
            prompt += QStringLiteral("- Seasonal & weather coherence: %1\n").arg(ctx.seasonalGuidance.trimmed());
        }
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
        const QString productLabel = ctx.productSummary.isEmpty()
            ? QStringLiteral("high heels")
            : ctx.productSummary;
        const QString trimmed = plannedPrompt.trimmed();
        const bool hasBottomUpCue =
            trimmed.contains(QLatin1String("slow"), Qt::CaseInsensitive)
            && (trimmed.contains(QLatin1String("up"), Qt::CaseInsensitive)
                || trimmed.contains(QLatin1String("neck"), Qt::CaseInsensitive)
                || trimmed.contains(QLatin1String("bottom"), Qt::CaseInsensitive));
        if (!trimmed.isEmpty() && hasBottomUpCue)
        {
            return ensureTrailingPeriod(ensureVideoPrefix(trimmed));
        }
        return QStringLiteral(
            "Generate a video: Slow-motion close-up bottom-to-top camera reveal of the stylish woman "
            "walking gracefully forward wearing the exact featured %1. Begin tightly framed at the "
            "bottom on her feet and heels stepping in crisp slow motion, then smoothly glide and tilt "
            "the camera upward from the shoes along her legs and outfit up to her collarbone and neck, "
            "keeping the exact shoe design, outfit, and lighting unchanged with no text.")
            .arg(productLabel);
    }

    QString finalizeSecondStepVideoPrompt(const QString &plannedSecondPrompt,
                                          const PromptContext &ctx) const override
    {
        const QString productLabel = ctx.productSummary.isEmpty()
            ? QStringLiteral("heels")
            : ctx.productSummary;
        QString trimmed = plannedSecondPrompt.trimmed();
        if (!trimmed.isEmpty())
        {
            if (trimmed.startsWith(QLatin1String("Generate a video:"), Qt::CaseInsensitive))
            {
                trimmed = trimmed.mid(17).trimmed();
            }
            if (!trimmed.startsWith(QLatin1String("In the next video"), Qt::CaseInsensitive))
            {
                trimmed = QStringLiteral("In the next video, ") + trimmed;
            }
            return ensureTrailingPeriod(ensureVideoPrefix(trimmed));
        }
        return QStringLiteral(
            "Generate a video: In the next video, we see her walking confidently across 3 different "
            "dynamic camera views, alternating between a full-body wide shot showing the stylish model "
            "in full head-to-toe outfit and heels, a crisp low-angle close-up tracking the %1 in "
            "motion, and a sleek full-length editorial angle — keeping the exact same shoe design, "
            "outfit, lighting, and setting throughout with no text.")
            .arg(productLabel);
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
const TwoStepRevealMethod twoStepRevealMethod;

const InspirationAdaptationMethod::Recorder directSwapRecorder(&directSwapMethod);
const InspirationAdaptationMethod::Recorder variantInImageRecorder(&variantInImageMethod);
const InspirationAdaptationMethod::Recorder propertiesFullRecorder(&propertiesFullMethod);
const InspirationAdaptationMethod::Recorder editorialOnlyRecorder(&editorialOnlyMethod);
const InspirationAdaptationMethod::Recorder twoStepRevealRecorder(&twoStepRevealMethod);
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

QList<const InspirationAdaptationMethod *> InspirationAdaptationMethod::orderedMethods(
    bool hasReferenceMedia, bool isVideoFormat)
{
    QList<const InspirationAdaptationMethod *> list;
    for (const InspirationAdaptationMethod *m : registry())
    {
        if (!m)
        {
            continue;
        }
        if (hasReferenceMedia && !m->supportsReferenceMedia())
        {
            continue;
        }
        if (!hasReferenceMedia && !m->supportsWithoutReferenceMedia())
        {
            continue;
        }
        if (!isVideoFormat && m->isTwoStepVideoStrategy())
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
    const QString &globalWinningMethodId,
    bool isVideoFormat)
{
    const QList<const InspirationAdaptationMethod *> candidates = orderedMethods(hasReferenceMedia, isVideoFormat);
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

QDate InspirationAdaptationMethod::effectiveCurrentDate()
{
    const QString overrideStr = qEnvironmentVariable("QS_CURRENT_DATE_OVERRIDE").trimmed();
    if (!overrideStr.isEmpty())
    {
        const QDate parsed = QDate::fromString(overrideStr.left(10), Qt::ISODate);
        if (parsed.isValid())
        {
            return parsed;
        }
    }
    return QDate::currentDate();
}

QString InspirationAdaptationMethod::currentSeasonLabel(const QDate &date)
{
    const QDate d = date.isValid() ? date : QDate::currentDate();
    const QString monthName = QLocale::c().monthName(d.month());
    switch (d.month())
    {
    case 12:
    case 1:
    case 2:
        return QStringLiteral("Winter (%1 %2)").arg(monthName).arg(d.year());
    case 3:
    case 4:
    case 5:
        return QStringLiteral("Spring (%1 %2)").arg(monthName).arg(d.year());
    case 6:
    case 7:
    case 8:
        return QStringLiteral("Summer (%1 %2)").arg(monthName).arg(d.year());
    case 9:
    case 10:
        return QStringLiteral("Autumn (%1 %2)").arg(monthName).arg(d.year());
    case 11:
    default:
        return QStringLiteral("Late Autumn / Early Winter (%1 %2)").arg(monthName).arg(d.year());
    }
}

namespace
{
bool isOpenSummerFootwear(const QString &productText)
{
    static const QRegularExpression openFootwearRe(
        QStringLiteral("\\b(sandals?|open[-\\s]?toe|espadrilles?|flip[-\\s]?flops?|slides?|thong\\s+heels?|strappy\\s+sandals?)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return openFootwearRe.match(productText).hasMatch();
}

bool isWinterBootFootwear(const QString &productText)
{
    static const QRegularExpression bootRe(
        QStringLiteral("\\b(boots?|booties|snow\\s*boots?|fur[-\\s]?lined|ankle\\s+boots?|knee[-\\s]?high\\s+boots?)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return bootRe.match(productText).hasMatch();
}
} // namespace

QString InspirationAdaptationMethod::seasonalGuardrailInstruction(const QString &productSummary,
                                                                  const QDate &date)
{
    const QDate d = date.isValid() ? date : effectiveCurrentDate();
    const QString seasonLbl = currentSeasonLabel(d);
    const bool openFootwear = isOpenSummerFootwear(productSummary);
    const bool bootFootwear = isWinterBootFootwear(productSummary);
    const int m = d.month();

    QString rule = QStringLiteral("Current calendar season is %1. ").arg(seasonLbl);
    if (m == 11 || m == 12 || m == 1 || m == 2)
    {
        rule += QStringLiteral(
            "Avoid out-of-season midsummer beach, poolside, swimsuit, or tropical resort backdrops. ");
        if (openFootwear)
        {
            rule += QStringLiteral(
                "Because this product is open-toe / sandal footwear in %1, style it EXCLUSIVELY in warm, "
                "plush INDOOR evening/party/luxury settings (marble hotel lobby, candlelit dinner lounge, "
                "holiday gala, chic boutique interior, carpeted penthouse) — NEVER outdoors in snow, ice, "
                "slush, frost, or paired with heavy winter parkas.")
                .arg(seasonLbl);
        }
        else
        {
            rule += QStringLiteral(
                "Use crisp autumn/winter city streets, upscale indoor hotel lobbies, boutiques, or evening venues "
                "(never pair delicate evening heels with deep snowbanks or ski gear).");
        }
    }
    else if (m >= 6 && m <= 8)
    {
        rule += QStringLiteral(
            "NEVER depict snow, frost, ice, winter blizzards, holiday/Christmas decor, or heavy winter coats/scarves/parkas in %1. ")
            .arg(seasonLbl);
        if (bootFootwear)
        {
            rule += QStringLiteral(
                "Because this product is a boot being shown in Summer, style it in a sleek season-neutral indoor "
                "runway/studio/boutique or chic transitional evening look — NEVER in snow or heavy winter outerwear.");
        }
        else
        {
            rule += QStringLiteral(
                "Favor warm sunlit city terraces, golden-hour avenues, garden courtyards, or airy luxury interiors.");
        }
    }
    else if (m >= 3 && m <= 5)
    {
        rule += QStringLiteral(
            "Avoid winter snow/ice/blizzards, Christmas decor, or heavy fur parkas in %1; favor fresh spring "
            "daylight, cafés, city avenues, galleries, or refined indoor settings (and never place sandals in snow).")
            .arg(seasonLbl);
    }
    else
    {
        rule += QStringLiteral(
            "Avoid midsummer beach/swimwear scenes and avoid deep winter blizzards; favor golden-hour autumn "
            "boulevards, cafés, or warm luxury indoor venues (style open-toe sandals indoors rather than cold outdoor streets).");
    }
    return rule.trimmed();
}

bool InspirationAdaptationMethod::hasTextSeasonalClash(const QString &sceneText,
                                                       const QString &productText,
                                                       const QDate &date,
                                                       QString *outReason)
{
    const QString s = sceneText.trimmed();
    if (s.isEmpty())
    {
        return false;
    }
    const QDate d = date.isValid() ? date : effectiveCurrentDate();
    const int m = d.month();

    static const QRegularExpression winterSnowRe(
        QStringLiteral("\\b(snow|snowy|snowing|snowfall|snowbank|blizzard|frosty|icy\\s+street|icicles|ski\\s+slope|apres[-\\s]?ski|winter\\s+wonderland|christmas\\s+tree|xmas|puffer\\s+coat|heavy\\s+parka)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression summerBeachRe(
        QStringLiteral("\\b(beach|poolside|swimming\\s+pool|bikini|swimsuit|swimwear|tropical\\s+island|ocean\\s+surf|palm\\s+beach|summer\\s+heatwave)\\b"),
        QRegularExpression::CaseInsensitiveOption);

    const auto snowMatch = winterSnowRe.match(s);
    const auto beachMatch = summerBeachRe.match(s);
    const bool openFootwear = isOpenSummerFootwear(productText);
    const bool bootFootwear = isWinterBootFootwear(productText);

    if (snowMatch.hasMatch() && openFootwear)
    {
        if (outReason)
        {
            *outReason = QStringLiteral("open-toe/sandal product clashes with wintry/snowy scene (\"%1\")")
                             .arg(snowMatch.captured(0));
        }
        return true;
    }
    if (beachMatch.hasMatch() && bootFootwear)
    {
        if (outReason)
        {
            *outReason = QStringLiteral("boot footwear clashes with beach/poolside scene (\"%1\")")
                             .arg(beachMatch.captured(0));
        }
        return true;
    }
    if (snowMatch.hasMatch() && (m >= 3 && m <= 9))
    {
        if (outReason)
        {
            *outReason = QStringLiteral("wintry/snowy scene (\"%1\") is out of season for %2")
                             .arg(snowMatch.captured(0), currentSeasonLabel(d));
        }
        return true;
    }
    if (beachMatch.hasMatch() && (m == 11 || m == 12 || m == 1 || m == 2))
    {
        if (outReason)
        {
            *outReason = QStringLiteral("summer beach/poolside scene (\"%1\") is out of season for %2")
                             .arg(beachMatch.captured(0), currentSeasonLabel(d));
        }
        return true;
    }
    return false;
}

bool InspirationAdaptationMethod::hasReferenceSeasonalClash(const QJsonObject &reference,
                                                            const QJsonObject &product,
                                                            const QDate &date,
                                                            QString *outReason)
{
    QStringList sceneParts{
        reference.value(QStringLiteral("title")).toString(),
        reference.value(QStringLiteral("notes")).toString(),
        reference.value(QStringLiteral("url")).toString()
    };
    QStringList prodParts{
        product.value(QStringLiteral("title")).toString(),
        product.value(QStringLiteral("category")).toString(),
        product.value(QStringLiteral("color")).toString()
    };
    for (const QJsonValue &cv : product.value(QStringLiteral("categories")).toArray())
    {
        prodParts << cv.toString();
    }
    return hasTextSeasonalClash(
        sceneParts.join(QLatin1Char(' ')),
        prodParts.join(QLatin1Char(' ')),
        date,
        outReason);
}
