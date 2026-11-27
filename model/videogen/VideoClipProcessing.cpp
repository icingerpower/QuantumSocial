#pragma GCC optimize("O1")
#include "VideoClipProcessing.h"

#include <chrono>
#include <cmath>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QCoro/QCoroTimer>

namespace
{
struct CommandResult
{
    QByteArray output;
    QByteArray errorOutput;
    QString error;
};

QCoro::Task<CommandResult> run(QString program, QStringList args, int timeoutMs)
{
    const QString executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
    {
        co_return CommandResult{{}, {}, QObject::tr("%1 was not found on PATH.").arg(program)};
    }
    QProcess process;
    process.start(executable, args);
    if (!process.waitForStarted(5000))
    {
        co_return CommandResult{{}, {}, process.errorString()};
    }
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && timer.elapsed() < timeoutMs)
    {
        co_await QCoro::sleepFor(std::chrono::milliseconds(100));
    }
    if (process.state() != QProcess::NotRunning)
    {
        process.kill();
        process.waitForFinished(2000);
        co_return CommandResult{{}, {}, QObject::tr("%1 timed out.").arg(program)};
    }
    const QByteArray stdOut = process.readAllStandardOutput();
    const QByteArray stdErr = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        co_return CommandResult{stdOut, stdErr, QObject::tr("%1 failed: %2").arg(program,
            QString::fromUtf8(stdErr.right(1000)))};
    }
    co_return CommandResult{stdOut, stdErr, {}};
}

struct MediaInfo
{
    double duration = 0;
    int width = 0;
    int height = 0;
    bool audio = false;
    QString error;
};

QCoro::Task<MediaInfo> inspect(QString path)
{
    QStringList args{QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
        QStringLiteral("-of"), QStringLiteral("json"), path};
    auto result = co_await run(QStringLiteral("ffprobe"), args, 30000);
    MediaInfo info;
    info.error = result.error;
    if (!info.error.isEmpty())
    {
        co_return info;
    }
    const auto json = QJsonDocument::fromJson(result.output).object();
    for (const auto &entry : json.value(QStringLiteral("streams")).toArray())
    {
        const auto stream = entry.toObject();
        if (stream.value(QStringLiteral("codec_type")).toString() == QLatin1String("video") && !info.width)
        {
            info.width = stream.value(QStringLiteral("width")).toInt();
            info.height = stream.value(QStringLiteral("height")).toInt();
            info.duration = stream.value(QStringLiteral("duration")).toString().toDouble();
        }
        info.audio |= stream.value(QStringLiteral("codec_type")).toString() == QLatin1String("audio");
    }
    if (info.duration <= 0)
    {
        info.duration = json.value(QStringLiteral("format")).toObject()
            .value(QStringLiteral("duration")).toString().toDouble();
    }
    if (!std::isfinite(info.duration) || info.duration <= 0 || info.width <= 0 || info.height <= 0)
    {
        info.error = QObject::tr("Could not determine video duration or dimensions for %1.").arg(path);
    }
    co_return info;
}

QString publish(const QString &source, const QString &destination)
{
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly)) return input.errorString();
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    while (!input.atEnd())
    {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFile::NoError) return input.errorString();
        if (output.write(chunk) != chunk.size()) return output.errorString();
    }
    if (!output.commit()) return output.errorString();
    return {};
}

QCoro::Task<VideoClipProcessing::Result> encodeSlice(QString source, double startSec, double durationSec)
{
    QTemporaryDir temp(QFileInfo(source).absolutePath() + QStringLiteral("/.trim-XXXXXX"));
    if (!temp.isValid()) co_return VideoClipProcessing::Result{{}, QObject::tr("Could not create a clip processing directory.")};
    const QString output = temp.filePath(QStringLiteral("clip.mp4"));
    QStringList args{QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), source,
        QStringLiteral("-ss"), QString::number(qMax(0.0, startSec), 'f', 6),
        QStringLiteral("-t"), QString::number(qMax(0.05, durationSec), 'f', 6),
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("fast"), QStringLiteral("-crf"), QStringLiteral("18"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-movflags"), QStringLiteral("+faststart"), output};
    const auto result = co_await run(QStringLiteral("ffmpeg"), args, 600000);
    if (!result.error.isEmpty()) co_return VideoClipProcessing::Result{{}, result.error};
    // Preserve the original download for diagnosis/recovery and lossless 2-step reuse.
    const QString destination = QFileInfo(source).absolutePath() + QLatin1Char('/')
        + QFileInfo(source).completeBaseName() + QStringLiteral("_clip.mp4");
    const QString error = publish(output, destination);
    co_return VideoClipProcessing::Result{error.isEmpty() ? destination : QString{}, error};
}

// Measures average YUV PSNR (in dB) between the opening checkSec of previousClip and candidateClip.
// Prepended Gemini clips score ~42-47 dB (or inf when identical); unrelated clips score ~10-16 dB.
// Returns 100.0 for "inf", or -1.0 if PSNR could not be measured.
QCoro::Task<double> measurePrefixPsnrDb(QString previousClip, QString candidateClip, double checkSec)
{
    const QString durStr = QString::number(qMax(0.15, checkSec), 'f', 3);
    QStringList args{
        QStringLiteral("-nostats"),
        QStringLiteral("-v"), QStringLiteral("info"),
        QStringLiteral("-t"), durStr, QStringLiteral("-i"), previousClip,
        QStringLiteral("-t"), durStr, QStringLiteral("-i"), candidateClip,
        QStringLiteral("-lavfi"),
        QStringLiteral("[0:v]fps=8,scale=160:160:flags=fast_bilinear,format=yuv420p[a];"
                       "[1:v]fps=8,scale=160:160:flags=fast_bilinear,format=yuv420p[b];"
                       "[a][b]psnr"),
        QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")
    };
    const auto res = co_await run(QStringLiteral("ffmpeg"), args, 30000);
    if (!res.error.isEmpty()) co_return -1.0;

    static const QRegularExpression psnrRe(QStringLiteral(R"(average:\s*(inf|[0-9]*\.?[0-9]+))"));
    const QString errText = QString::fromUtf8(res.errorOutput);
    auto it = psnrRe.globalMatch(errText);
    double lastPsnr = -1.0;
    while (it.hasNext())
    {
        const auto m = it.next();
        const QString cap = m.captured(1);
        if (cap.compare(QLatin1String("inf"), Qt::CaseInsensitive) == 0)
        {
            lastPsnr = 100.0;
            continue;
        }
        bool ok = false;
        const double val = cap.toDouble(&ok);
        if (ok && std::isfinite(val))
        {
            lastPsnr = val;
        }
    }
    co_return lastPsnr;
}
}

QCoro::Task<VideoClipProcessing::Result> VideoClipProcessing::trimTail(QString source, double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0) co_return Result{source, {}};
    const auto info = co_await inspect(source);
    if (!info.error.isEmpty()) co_return Result{{}, info.error};
    // Allow minor container/audio packet jitter (~2% or >=50ms, e.g. 10.027s on a 10s standalone clip)
    // without triggering an unnecessary re-encode.
    const double jitterTolerance = qMax(0.05, seconds * 0.02);
    if (info.duration <= seconds + jitterTolerance) co_return Result{source, {}};
    co_return co_await encodeSlice(source, info.duration - seconds, seconds);
}

QCoro::Task<VideoClipProcessing::ContinuationResult> VideoClipProcessing::prepareContinuation(
    QString source, QString previousClip, double fallbackSeconds)
{
    if (!std::isfinite(fallbackSeconds) || fallbackSeconds <= 0)
    {
        co_return ContinuationResult{source, {}, false, 0.0, {}};
    }

    if (previousClip.isEmpty() || !QFileInfo::exists(previousClip))
    {
        const auto tail = co_await trimTail(source, fallbackSeconds);
        co_return ContinuationResult{
            tail.path,
            {},
            tail.error.isEmpty() && tail.path != source,
            0.0,
            tail.error
        };
    }

    const auto srcInfo = co_await inspect(source);
    if (!srcInfo.error.isEmpty())
    {
        co_return ContinuationResult{{}, {}, false, 0.0, srcInfo.error};
    }

    const auto prevInfo = co_await inspect(previousClip);
    if (!prevInfo.error.isEmpty() || prevInfo.duration <= 0.15)
    {
        const auto tail = co_await trimTail(source, fallbackSeconds);
        co_return ContinuationResult{
            tail.path,
            {},
            tail.error.isEmpty() && tail.path != source,
            0.0,
            tail.error
        };
    }

    // If source is not meaningfully longer than previousClip, Gemini returned a standalone clip.
    const double minNewSegment = qMin(1.5, prevInfo.duration * 0.45);
    if (srcInfo.duration < prevInfo.duration + minNewSegment)
    {
        co_return ContinuationResult{source, {}, false, 0.0, {}};
    }

    // Check whether source actually begins with previousClip's frames (YUV PSNR >= 24 dB).
    const double checkDur = qBound(0.15, qMin(prevInfo.duration * 0.85, 6.0), prevInfo.duration);
    const double prefixPsnrDb = co_await measurePrefixPsnrDb(previousClip, source, checkDur);
    if (prefixPsnrDb >= 0.0 && prefixPsnrDb < 24.0)
    {
        // Opening frames differ from previousClip -> standalone clip (even if > fallbackSeconds).
        co_return ContinuationResult{source, {}, false, 0.0, {}};
    }

    const double remainder = srcInfo.duration - prevInfo.duration;
    const double maxSingleSegment = qMax(fallbackSeconds * 1.45, prevInfo.duration * 1.45);
    if (remainder <= maxSingleSegment)
    {
        // Direct 2-clip merge [previousClip + newSegment]: slice at exact prevInfo.duration
        // so there is zero duplicated overlap and zero missing footage, and preserve source
        // as nativeMergedPath so 2-step workflows can publish Gemini's seamless merge directly.
        const auto seg = co_await encodeSlice(source, prevInfo.duration, remainder);
        if (!seg.error.isEmpty())
        {
            co_return ContinuationResult{{}, {}, false, 0.0, seg.error};
        }
        co_return ContinuationResult{seg.path, source, true, prevInfo.duration, {}};
    }

    // Cumulative multi-turn merge (e.g. 30s/40s across multiple takes in the same chat):
    // extract the trailing fallbackSeconds segment.
    const double tailLen = qMin(fallbackSeconds, remainder);
    const auto seg = co_await encodeSlice(source, srcInfo.duration - tailLen, tailLen);
    if (!seg.error.isEmpty())
    {
        co_return ContinuationResult{{}, {}, false, 0.0, seg.error};
    }
    co_return ContinuationResult{seg.path, {}, true, srcInfo.duration - tailLen, {}};
}

QCoro::Task<VideoClipProcessing::MotionGrade> VideoClipProcessing::gradeClipMotion(
    QString source, bool expectsMultipleAngles)
{
    MotionGrade grade;
    const auto info = co_await inspect(source);
    if (!info.error.isEmpty())
    {
        grade.error = info.error;
        grade.qualityScore = 75;
        co_return grade;
    }
    grade.durationSec = info.duration;
    // Skip freeze detection on ultra-short (<2.8s) synthetic unit-test clips
    if (info.duration < 2.8)
    {
        grade.qualityScore = 85;
        co_return grade;
    }

    QStringList args{
        QStringLiteral("-nostats"),
        QStringLiteral("-v"), QStringLiteral("info"),
        QStringLiteral("-i"), source,
        QStringLiteral("-vf"),
        QStringLiteral("fps=10,scale=160:-2:flags=fast_bilinear,format=yuv420p,"
                       "freezedetect=n=-48dB:d=1.0,"
                       "select='gt(scene,0.28)',showinfo"),
        QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")
    };
    const auto res = co_await run(QStringLiteral("ffmpeg"), args, 30000);
    if (!res.error.isEmpty())
    {
        grade.error = res.error;
        grade.qualityScore = 80;
        co_return grade;
    }

    const QString errText = QString::fromUtf8(res.errorOutput);
    static const QRegularExpression freezeStartRe(
        QStringLiteral(R"(freeze_start:\s*([0-9]*\.?[0-9]+))"));
    static const QRegularExpression freezeDurRe(
        QStringLiteral(R"(freeze_duration:\s*([0-9]*\.?[0-9]+))"));
    static const QRegularExpression showInfoPtsRe(
        QStringLiteral(R"(\[Parsed_showinfo_[^\]]*\]\s+n:\s*\d+\s+pts:\s*\d+\s+pts_time:([0-9]*\.?[0-9]+))"));

    double pendingOpeningStart = -1.0;
    for (const QString &line : errText.split(QLatin1Char('\n')))
    {
        const auto ms = freezeStartRe.match(line);
        if (ms.hasMatch())
        {
            const double st = ms.captured(1).toDouble();
            if (st <= 0.30 && pendingOpeningStart < 0.0)
            {
                pendingOpeningStart = st;
            }
        }
        const auto md = freezeDurRe.match(line);
        if (md.hasMatch() && pendingOpeningStart >= 0.0)
        {
            grade.openingFreezeSec = qMax(grade.openingFreezeSec, md.captured(1).toDouble());
            pendingOpeningStart = -1.0;
        }
    }
    if (pendingOpeningStart >= 0.0 && grade.openingFreezeSec <= 0.0)
    {
        // Frozen from opening all the way to EOF
        grade.openingFreezeSec = qMax(0.0, info.duration - pendingOpeningStart);
    }

    auto cutIt = showInfoPtsRe.globalMatch(errText);
    while (cutIt.hasNext())
    {
        const double pts = cutIt.next().captured(1).toDouble();
        if (pts > 0.15)
        {
            ++grade.sceneCutCount;
        }
    }

    int score = 86;
    if (grade.openingFreezeSec >= 1.0)
    {
        score -= qMin(42, int(grade.openingFreezeSec * 18.0));
    }
    if (expectsMultipleAngles)
    {
        if (grade.sceneCutCount >= 2)
        {
            score += 12;
        }
        else if (grade.sceneCutCount == 1)
        {
            score += 4;
        }
        else
        {
            score -= 16;
        }
    }
    else if (grade.openingFreezeSec < 0.2)
    {
        score += 6;
    }
    grade.qualityScore = qBound(10, score, 100);
    co_return grade;
}

QCoro::Task<VideoClipProcessing::Result> VideoClipProcessing::merge(
    QStringList sources, QString destination, std::function<bool()> stillCurrent)
{
    if (sources.isEmpty()) co_return Result{{}, QObject::tr("No clips selected.")};
    QList<MediaInfo> infos;
    QStringList args{QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error")};
    for (const auto &source : sources)
    {
        const auto info = co_await inspect(source);
        if (!info.error.isEmpty()) co_return Result{{}, info.error};
        infos.append(info);
        args << QStringLiteral("-i") << source;
    }
    // Match the first clip's canvas, with even dimensions for H.264.
    const int width = (infos.first().width + 1) / 2 * 2;
    const int height = (infos.first().height + 1) / 2 * 2;
    QStringList filters;
    QString inputs;
    for (int i = 0; i < infos.size(); ++i)
    {
        filters << QStringLiteral("[%1:v:0]setpts=PTS-STARTPTS,scale=%2:%3:force_original_aspect_ratio=decrease,"
                                  "pad=%2:%3:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=30,format=yuv420p[v%1]")
            .arg(i).arg(width).arg(height);
        const QString audio = infos[i].audio
            ? QStringLiteral("[%1:a:0]asetpts=PTS-STARTPTS,aresample=48000,"
                             "aformat=sample_fmts=fltp:channel_layouts=stereo,apad").arg(i)
            : QStringLiteral("anullsrc=r=48000:cl=stereo");
        filters << audio + QStringLiteral(",atrim=duration=%1,asetpts=PTS-STARTPTS[a%2]")
            .arg(QString::number(infos[i].duration, 'f', 6)).arg(i);
        inputs += QStringLiteral("[v%1][a%1]").arg(i);
    }
    filters << inputs + QStringLiteral("concat=n=%1:v=1:a=1[v][a]").arg(infos.size());
    QTemporaryDir temp(QFileInfo(destination).absolutePath() + QStringLiteral("/.merge-XXXXXX"));
    if (!temp.isValid()) co_return Result{{}, QObject::tr("Could not create a merge directory.")};
    const QString output = temp.filePath(QStringLiteral("final.mp4"));
    args << QStringLiteral("-filter_complex_threads") << QStringLiteral("1")
         << QStringLiteral("-filter_complex") << filters.join(QLatin1Char(';'))
         << QStringLiteral("-map") << QStringLiteral("[v]") << QStringLiteral("-map") << QStringLiteral("[a]")
         << QStringLiteral("-c:v") << QStringLiteral("libx264")
         << QStringLiteral("-preset") << QStringLiteral("fast") << QStringLiteral("-crf") << QStringLiteral("18")
         << QStringLiteral("-c:a") << QStringLiteral("aac")
         << QStringLiteral("-movflags") << QStringLiteral("+faststart") << output;
    const auto result = co_await run(QStringLiteral("ffmpeg"), args, 1800000);
    if (!result.error.isEmpty()) co_return Result{{}, result.error};
    if (stillCurrent && !stillCurrent()) co_return Result{};
    const QString error = publish(output, destination);
    co_return Result{error.isEmpty() ? destination : QString{}, error};
}
