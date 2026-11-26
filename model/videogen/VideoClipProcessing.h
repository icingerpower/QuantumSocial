#ifndef VIDEOCLIPPROCESSING_H
#define VIDEOCLIPPROCESSING_H

#include <QStringList>
#include <functional>
#include <QCoro/QCoroTask>

namespace VideoClipProcessing
{
struct Result
{
    QString path;
    QString error;
};

struct ContinuationResult
{
    QString path;              // Isolated new segment (for keyframe validation & N-step concat)
    QString nativeMergedPath;  // Non-empty when source already cleanly equals [previousClip + newSegment]
    bool hadPrependedPrefix = false;
    double prefixDuration = 0.0;
    QString error;
};

// Keep short clips unchanged; encode an accurate tail when a download is longer.
QCoro::Task<Result> trimTail(QString source, double seconds);
// Detect whether a continuation download prepended previousClip (via duration + prefix SSIM):
// - Standalone clip (no prepended prefix): returns source untouched (zero re-encode).
// - Direct 2-clip merge (starts with previousClip): trims from exact previousClip duration
//   for validation/takes, and exposes source as nativeMergedPath for lossless 2-step output.
// - Cumulative multi-turn merge: trims the trailing fallbackSeconds segment.
QCoro::Task<ContinuationResult> prepareContinuation(
    QString source, QString previousClip, double fallbackSeconds);
// Normalize video/audio streams and concatenate in the supplied step order.
QCoro::Task<Result> merge(QStringList sources, QString destination,
                         std::function<bool()> stillCurrent = {});
}

#endif
