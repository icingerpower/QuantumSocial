#include <QRandomGenerator>

#include "TreeProperties.h"

#include "PropertySampler.h"

namespace {

struct Candidate
{
    TreeProperties::PropertyNode *node;
    TableVideos::AggregateStats aggregate;
    double weight = 0.0;
};

// Weighted sampling WITHOUT replacement: repeatedly pick one of the
// remaining candidates (probability proportional to its share of the
// remaining total weight), remove it, repeat. The result is a full
// weighted shuffle — every candidate appears exactly once, higher-weighted
// ones more likely (not guaranteed) to land earlier.
QList<Candidate> weightedShuffle(QList<Candidate> remaining, double remainingTotal)
{
    QList<Candidate> order;
    while (!remaining.isEmpty())
    {
        double roll = QRandomGenerator::global()->generateDouble() * remainingTotal;
        int pickedIndex = remaining.size() - 1;
        for (int i = 0; i < remaining.size(); ++i)
        {
            roll -= remaining[i].weight;
            if (roll <= 0.0)
            {
                pickedIndex = i;
                break;
            }
        }
        order << remaining[pickedIndex];
        remainingTotal -= remaining[pickedIndex].weight;
        remaining.removeAt(pickedIndex);
    }
    return order;
}

} // namespace

QList<QList<PropertySampler::SampledValue>> PropertySampler::sampleVariants(
    const TreeProperties &tree, const TableVideos &videos, int variantCount)
{
    QList<QList<SampledValue>> variants;
    for (int v = 0; v < variantCount; ++v)
    {
        variants << QList<SampledValue>{};
    }
    if (variantCount <= 0)
    {
        return variants;
    }

    for (TreeProperties::PropertyNode *property : tree.topLevelNodes())
    {
        if (property->children.isEmpty())
        {
            continue;
        }

        QList<Candidate> candidates;
        double bestScore = 0.0;
        for (TreeProperties::PropertyNode *value : property->children)
        {
            Candidate candidate{value, videos.aggregateForValue(value->id)};
            bestScore = std::max(bestScore, candidate.aggregate.medianNormViews);
            candidates << candidate;
        }

        const double base = bestScore > 0.0 ? bestScore : 1.0;
        const double floor = 0.2 * base;
        double total = 0.0;
        for (Candidate &candidate : candidates)
        {
            const double score = candidate.aggregate.medianNormViews;
            candidate.weight = (score >= 0.0 ? score : base) + floor;
            total += candidate.weight;
        }

        const QList<Candidate> order = weightedShuffle(candidates, total);

        // Stable catalog order (unrelated to the weighted draw above) so a
        // UI's dropdown of alternatives doesn't reshuffle every time.
        QList<SiblingOption> siblings;
        for (const Candidate &candidate : candidates)
        {
            siblings << SiblingOption{candidate.node->id, candidate.node->name,
                                      candidate.node->promptFragment};
        }

        // Cycle through the shuffled order — when it has at least
        // variantCount entries, every variant below gets a genuinely
        // distinct value; otherwise repeats are spread evenly instead of
        // risking the same lone value for every variant.
        for (int v = 0; v < variantCount; ++v)
        {
            const Candidate &picked = order[v % order.size()];
            SampledValue sampled;
            sampled.id = picked.node->id;
            sampled.propertyId = property->id;
            sampled.propertyName = property->name;
            sampled.valueName = picked.node->name;
            sampled.promptFragment = picked.node->promptFragment;
            sampled.weightShare = total > 0.0 ? picked.weight / total : 0.0;
            sampled.videoCount = picked.aggregate.videoCount;
            sampled.medianNormViews = picked.aggregate.medianNormViews;
            sampled.siblings = siblings;
            variants[v] << sampled;
        }
    }
    return variants;
}
