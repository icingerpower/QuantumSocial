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

// Keep short clips unchanged; encode an accurate tail when a download is longer.
QCoro::Task<Result> trimTail(QString source, double seconds);
// Normalize video/audio streams and concatenate in the supplied step order.
QCoro::Task<Result> merge(QStringList sources, QString destination,
                         std::function<bool()> stillCurrent = {});
}

#endif
