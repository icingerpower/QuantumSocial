#ifndef PROPERTYSAMPLER_H
#define PROPERTYSAMPLER_H

#include <QList>
#include <QString>
#include <QUuid>

class TreeProperties;
class TableVideos;

// Picks the property values that will be tested, one recipe PER GENERATION
// PLAN VARIANT (DialogGenerationPlan's 3 suggested prompts), random but
// weighted by the statistics accumulated in TableVideos.
//
// Weighting (deliberately simple and explainable):
// - tested values:   weight = score + floor, score = median views/follower
// - untested values: weight = bestSibling + floor (optimistic start, so new
//                    values get tried quickly)
// - floor = 20% of the best sibling score, so losing values stay
//   occasionally alive until they have truly earned archiving.
// With no statistics at all every sibling weighs the same: pure exploration
// that anneals toward exploitation as snapshots arrive.
//
// The randomness lives HERE, in the app — the CLI downstream may only drop
// sampled properties that clash with the brief, never swap values, so the
// statistical attribution stays clean.
class PropertySampler
{
public:
    // One value a property could take — every sibling is exposed (not just
    // the sampled pick) so a caller can offer real manual override (e.g. a
    // combo box of all of them) instead of only keep/drop on the one the
    // sampler happened to draw.
    struct SiblingOption
    {
        QUuid id;
        QString name;
        QString promptFragment;
    };

    struct SampledValue
    {
        QUuid id;
        QUuid propertyId;   // the parent property's id — stable across values
        QString propertyName;
        QString valueName;
        QString promptFragment;
        double weightShare = 0.0;      // this value's share of its property
        int videoCount = 0;
        double medianNormViews = -1.0; // -1 = untested
        // Every value of this property, siblings.value(0) upward, in catalog
        // order (stable, unrelated to the weighted draw) — including this
        // one. Lets a UI show a dropdown of real alternatives.
        QList<SiblingOption> siblings;
    };

    // Draws variantCount recipes (one per suggested prompt) in ONE call, so
    // they can be coordinated instead of independently i.i.d.: for a
    // property with at least variantCount candidate values, every variant
    // is GUARANTEED a distinct one (a weighted shuffle, sampling without
    // replacement) — 3 independent draws-with-replacement could (and, with
    // only a few candidates, often did) land on the same value 2-3 times by
    // chance, defeating the point of offering 3 different prompts to
    // compare. With fewer candidates than variantCount, values are cycled
    // through the same weighted order round-robin, so repeats are spread
    // out instead of an unlucky draw picking one lone value every time.
    static QList<QList<SampledValue>> sampleVariants(const TreeProperties &tree,
                                                      const TableVideos &videos,
                                                      int variantCount);
};

#endif // PROPERTYSAMPLER_H
