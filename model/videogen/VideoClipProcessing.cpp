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
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QCoro/QCoroTimer>

namespace
{
struct CommandResult
{
    QByteArray output;
    QString error;
};

QCoro::Task<CommandResult> run(QString program, QStringList args, int timeoutMs)
{
    const QString executable = QStandardPaths::findExecutable(program);
    if (executable.isEmpty())
    {
        co_return CommandResult{{}, QObject::tr("%1 was not found on PATH.").arg(program)};
    }
    QProcess process;
    process.start(executable, args);
    if (!process.waitForStarted(5000))
    {
        co_return CommandResult{{}, process.errorString()};
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
        co_return CommandResult{{}, QObject::tr("%1 timed out.").arg(program)};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        co_return CommandResult{{}, QObject::tr("%1 failed: %2").arg(program,
            QString::fromUtf8(process.readAllStandardError().right(1000)))};
    }
    co_return CommandResult{process.readAllStandardOutput(), {}};
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
}

QCoro::Task<VideoClipProcessing::Result> VideoClipProcessing::trimTail(QString source, double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0) co_return Result{source, {}};
    const auto info = co_await inspect(source);
    if (!info.error.isEmpty()) co_return Result{{}, info.error};
    if (info.duration <= seconds + 0.001) co_return Result{source, {}};

    QTemporaryDir temp(QFileInfo(source).absolutePath() + QStringLiteral("/.trim-XXXXXX"));
    if (!temp.isValid()) co_return Result{{}, QObject::tr("Could not create a clip processing directory.")};
    const QString output = temp.filePath(QStringLiteral("clip.mp4"));
    QStringList args{QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), source,
        QStringLiteral("-ss"), QString::number(info.duration - seconds, 'f', 6),
        QStringLiteral("-t"), QString::number(seconds, 'f', 6),
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-preset"), QStringLiteral("fast"), QStringLiteral("-crf"), QStringLiteral("18"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-movflags"), QStringLiteral("+faststart"), output};
    const auto result = co_await run(QStringLiteral("ffmpeg"), args, 600000);
    if (!result.error.isEmpty()) co_return Result{{}, result.error};
    // Preserve the original download for diagnosis/recovery.
    const QString destination = QFileInfo(source).absolutePath() + QLatin1Char('/')
        + QFileInfo(source).completeBaseName() + QStringLiteral("_clip.mp4");
    const QString error = publish(output, destination);
    co_return Result{error.isEmpty() ? destination : QString{}, error};
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
