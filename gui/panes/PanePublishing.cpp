// GCC 13 miscompiles some coroutine call shapes at -O2/-O3 — force -O1 for
// this translation unit, same workaround used elsewhere for QCoro-based code.
#pragma GCC optimize("O1")

#include "PanePublishing.h"

#include "AbstractCli.h"
#include "../../../common/workingdirectory/WorkingDirectoryManager.h"
#include "model/FavoriteHooks.h"
#include "model/PreferredHashtags.h"
#include "model/TableAccounts.h"
#include "model/TableProjects.h"
#include "model/accounts/AbstractSocialAccount.h"
#include "model/videos/SocialStatistics.h"
#include "model/videos/TableVideos.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>

namespace {

QString codeTagFromShortCodePub(const QString &shortCode)
{
    if (shortCode.isEmpty())
    {
        return QString{};
    }
    QString digits = shortCode;
    digits.remove(QStringLiteral("QS-"));
    return QStringLiteral("qs") + digits.toLower();
}

QJsonObject parseJsonObjFromText(const QString &raw)
{
    const qsizetype first = raw.indexOf(QLatin1Char('{'));
    const qsizetype last = raw.lastIndexOf(QLatin1Char('}'));
    if (first < 0 || last <= first)
    {
        return QJsonObject{};
    }
    return QJsonDocument::fromJson(raw.mid(first, last - first + 1).toUtf8()).object();
}

QString strategicWrapSingleLine(const QString &paragraph, int maxLineChars = 24)
{
    const QString cleaned = paragraph.simplified();
    if (cleaned.isEmpty())
    {
        return QString{};
    }
    const QStringList words = cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const int n = words.size();
    if (cleaned.size() <= 18 || n <= 2)
    {
        return cleaned;
    }

    static const QSet<QString> danglingEndWords{
        QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("the"),
        QStringLiteral("this"), QStringLiteral("these"), QStringLiteral("that"), QStringLiteral("those"),
        QStringLiteral("my"), QStringLiteral("your"), QStringLiteral("our"), QStringLiteral("her"), QStringLiteral("his"),
        QStringLiteral("in"), QStringLiteral("on"), QStringLiteral("of"), QStringLiteral("to"),
        QStringLiteral("for"), QStringLiteral("with"), QStringLiteral("at"), QStringLiteral("by"), QStringLiteral("from"),
        QStringLiteral("le"), QStringLiteral("la"), QStringLiteral("les"), QStringLiteral("un"), QStringLiteral("une"),
        QStringLiteral("des"), QStringLiteral("du"), QStringLiteral("de"), QStringLiteral("d"),
        QStringLiteral("ce"), QStringLiteral("ces"), QStringLiteral("cet"), QStringLiteral("cette"),
        QStringLiteral("mon"), QStringLiteral("ma"), QStringLiteral("mes"), QStringLiteral("en"), QStringLiteral("au"), QStringLiteral("aux")
    };
    static const QSet<QString> clauseStartWords{
        QStringLiteral("in"), QStringLiteral("on"), QStringLiteral("with"), QStringLiteral("for"),
        QStringLiteral("when"), QStringLiteral("until"), QStringLiteral("while"), QStringLiteral("from"),
        QStringLiteral("into"), QStringLiteral("that"), QStringLiteral("but"), QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("to"),
        QStringLiteral("pour"), QStringLiteral("avec"), QStringLiteral("dans"), QStringLiteral("sans"),
        QStringLiteral("quand"), QStringLiteral("mais"), QStringLiteral("ou"), QStringLiteral("sur"), QStringLiteral("chez")
    };

    QList<int> kCandidates;
    if (cleaned.size() <= 44)
    {
        kCandidates = {2, 3};
    }
    else if (cleaned.size() <= 68)
    {
        kCandidates = {3, 2, 4};
    }
    else
    {
        kCandidates = {3, 4};
    }

    double bestOverallCost = 1e18;
    QStringList bestLines{cleaned};

    for (int k : kCandidates)
    {
        if (k < 1 || k > n)
        {
            continue;
        }
        const double idealLen = double(cleaned.size()) / double(k);
        QVector<QVector<double>> costDp(n + 1, QVector<double>(k + 1, 1e18));
        QVector<QVector<int>> prevIdx(n + 1, QVector<int>(k + 1, -1));
        costDp[0][0] = 0.0;

        for (int used = 0; used < k; ++used)
        {
            for (int i = 0; i < n; ++i)
            {
                if (costDp[i][used] >= 1e17)
                {
                    continue;
                }
                const int maxJ = n - (k - used - 1);
                for (int j = i + 1; j <= maxJ; ++j)
                {
                    if (used == k - 1 && j != n)
                    {
                        continue;
                    }
                    const QStringList segWords = words.mid(i, j - i);
                    const QString seg = segWords.join(QLatin1Char(' '));
                    const int segLen = seg.size();
                    const bool isLast = (j == n);

                    const double diff = double(segLen) - idealLen;
                    double c = diff * diff * 2.5;
                    if (segLen > maxLineChars)
                    {
                        const double over = double(segLen - maxLineChars);
                        c += 900.0 + over * over * 38.0;
                    }
                    if (segWords.size() == 1 && segLen <= 6 && n >= 4)
                    {
                        c += 450.0; // avoid 1-word orphan line
                    }
                    if (!isLast)
                    {
                        const QString lastW = segWords.last();
                        if (!lastW.isEmpty() && QStringLiteral(",;:—-?!.").contains(lastW.back()))
                        {
                            c -= 85.0;
                        }
                        QString lastClean = lastW.toLower();
                        if (lastClean.contains(QLatin1Char('\'')))
                        {
                            lastClean = lastClean.section(QLatin1Char('\''), -1);
                        }
                        lastClean.remove(QRegularExpression{QStringLiteral("[^a-zà-ÿ0-9]")});
                        QString nextClean = words[j].toLower();
                        nextClean.remove(QRegularExpression{QStringLiteral("[^a-zà-ÿ0-9]")});
                        if (danglingEndWords.contains(lastClean))
                        {
                            c += 145.0;
                        }
                        if (clauseStartWords.contains(nextClean))
                        {
                            c -= 35.0;
                        }
                    }

                    const double candTotal = costDp[i][used] + c;
                    if (candTotal < costDp[j][used + 1])
                    {
                        costDp[j][used + 1] = candTotal;
                        prevIdx[j][used + 1] = i;
                    }
                }
            }
        }

        if (costDp[n][k] < 1e17)
        {
            const double kPenalty = k >= 2 ? double(k - 2) * 30.0 : 0.0;
            if (costDp[n][k] + kPenalty < bestOverallCost)
            {
                bestOverallCost = costDp[n][k] + kPenalty;
                QStringList recon;
                int cur = n;
                for (int u = k; u >= 1; --u)
                {
                    const int p = prevIdx[cur][u];
                    recon.prepend(words.mid(p, cur - p).join(QLatin1Char(' ')));
                    cur = p;
                }
                bestLines = recon;
            }
        }
    }
    return bestLines.join(QLatin1Char('\n'));
}

QString strategicWrapOverlayText(const QString &raw, int maxLineChars = 24)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty())
    {
        return QString{};
    }
    if (trimmed.contains(QLatin1Char('\n')))
    {
        QStringList out;
        for (const QString &ln : trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        {
            const QString s = ln.trimmed();
            if (!s.isEmpty())
            {
                out << (s.size() > maxLineChars ? strategicWrapSingleLine(s, maxLineChars) : s);
            }
        }
        return out.join(QLatin1Char('\n'));
    }
    return strategicWrapSingleLine(trimmed, maxLineChars);
}

void drawHookTextOverSlidePub(const QString &imagePath, const QString &hookText)
{
    const QString text = strategicWrapOverlayText(hookText, 24);
    if (imagePath.isEmpty() || text.isEmpty())
    {
        return;
    }
    QImage img{imagePath};
    if (img.isNull() || img.width() < 64 || img.height() < 64)
    {
        return;
    }
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter{&img};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = painter.font();
    font.setPixelSize(qMax(18, img.width() / 16));
    font.setBold(true);
    painter.setFont(font);

    const int marginX = qMax(16, img.width() / 10);
    const int maxTextW = img.width() - 2 * marginX;
    const QRect textBounds = painter.fontMetrics().boundingRect(
        QRect(0, 0, maxTextW, img.height() / 2),
        Qt::AlignCenter | Qt::TextWordWrap,
        text);
    const int padX = qMax(14, img.width() / 28);
    const int padY = qMax(10, img.height() / 55);
    const int boxW = qMin(img.width() - 24, textBounds.width() + 2 * padX);
    const int boxH = textBounds.height() + 2 * padY;
    const QRect pillRect((img.width() - boxW) / 2,
                         qMax(20, img.height() / 7),
                         boxW,
                         boxH);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(15, 18, 24, 195));
    painter.drawRoundedRect(pillRect, 14.0, 14.0);

    painter.setPen(QColor(255, 255, 255));
    painter.drawText(pillRect.adjusted(padX, padY, -padX, -padY),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     text);
    painter.end();
    img.save(imagePath, "PNG");
}

QString resolveCreativePreviewImagePath(const QDir &genDir,
                                        const QDir &tempDir,
                                        const QString &format,
                                        const QStringList &mediaPaths,
                                        bool allowFfmpegExtract)
{
    if (format != QLatin1String("video"))
    {
        for (const QString &p : mediaPaths)
        {
            if (QFileInfo::exists(p))
            {
                return p;
            }
        }
    }

    // For video creatives, prefer a cached or freshly extracted frame from the actual .mp4
    // BEFORE falling back to generation_source.png, so the table row preview always matches the video!
    if (format == QLatin1String("video"))
    {
        for (const QDir &dir : {tempDir, genDir})
        {
            if (!dir.exists()) continue;
            for (const QString &thumbName : {QStringLiteral("preview_thumb.jpg"), QStringLiteral("preview_thumb.png")})
            {
                const QString cand = dir.filePath(thumbName);
                if (QFileInfo::exists(cand) && !QImage{cand}.isNull())
                {
                    return cand;
                }
            }
        }
        if (allowFfmpegExtract && !mediaPaths.isEmpty())
        {
            const QString ffmpegExe = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
            if (!ffmpegExe.isEmpty())
            {
                QDir{}.mkpath(tempDir.absolutePath());
                const QString thumbPath = tempDir.filePath(QStringLiteral("preview_thumb.jpg"));
                for (const QString &seekSec : {QStringLiteral("0.20"), QStringLiteral("0.00")})
                {
                    QProcess proc;
                    proc.start(ffmpegExe, {
                        QStringLiteral("-hide_banner"),
                        QStringLiteral("-loglevel"), QStringLiteral("error"),
                        QStringLiteral("-y"),
                        QStringLiteral("-ss"), seekSec,
                        QStringLiteral("-i"), mediaPaths.first(),
                        QStringLiteral("-frames:v"), QStringLiteral("1"),
                        QStringLiteral("-vf"), QStringLiteral("scale=480:-2"),
                        thumbPath
                    });
                    if (proc.waitForFinished(2500) && QFileInfo::exists(thumbPath) && !QImage{thumbPath}.isNull())
                    {
                        return thumbPath;
                    }
                }
            }
        }
    }

    // Check known starting-frame / cached preview candidates as fast fallback
    const QStringList priorityNames{
        QStringLiteral("preview_thumb.jpg"),
        QStringLiteral("preview_thumb.png"),
        QStringLiteral("generation_source.png"),
        QStringLiteral("product_swapped_source.png"),
        QStringLiteral("starting_frame.png"),
        QStringLiteral("slide_1.png"),
        QStringLiteral("image.png")
    };
    for (const QDir &dir : {tempDir, genDir})
    {
        if (!dir.exists())
        {
            continue;
        }
        for (const QString &name : priorityNames)
        {
            const QString cand = dir.filePath(name);
            if (QFileInfo::exists(cand) && !QImage{cand}.isNull())
            {
                return cand;
            }
        }
        const QStringList imgs = dir.entryList(
            {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.webp")},
            QDir::Files, QDir::Name);
        for (const QString &fn : imgs)
        {
            if (fn.startsWith(QLatin1String("reference_"), Qt::CaseInsensitive)
                || fn.startsWith(QLatin1String("my_product"), Qt::CaseInsensitive))
            {
                continue;
            }
            const QString cand = dir.filePath(fn);
            if (!QImage{cand}.isNull())
            {
                return cand;
            }
        }
    }
    return QString{};
}

const QStringList &curatedVideoTextStyleIds()
{
    static const QStringList kIds{
        QStringLiteral("classic_black_pill"),
        QStringLiteral("classic_white_pill"),
        QStringLiteral("elegance_serif"),
        QStringLiteral("classic_clean_stroke")
    };
    return kIds;
}

QString normalizeVideoTextStyleId(const QString &raw)
{
    const QString s = raw.trimmed().toLower();
    return curatedVideoTextStyleIds().contains(s)
        ? s
        : QStringLiteral("classic_black_pill");
}

QString videoTextStyleDisplayName(const QString &styleId)
{
    const QString s = normalizeVideoTextStyleId(styleId);
    if (s == QLatin1String("classic_white_pill"))
    {
        return QObject::tr("Classic White Pill (Black on White)");
    }
    if (s == QLatin1String("elegance_serif"))
    {
        return QObject::tr("Elegance Editorial Serif (No Box)");
    }
    if (s == QLatin1String("classic_clean_stroke"))
    {
        return QObject::tr("Classic Outlined White (No Box)");
    }
    return QObject::tr("Classic Black Pill (White on Black)");
}

QString videoTextStyleShortTag(const QString &styleId)
{
    const QString s = normalizeVideoTextStyleId(styleId);
    if (s == QLatin1String("classic_white_pill")) return QStringLiteral("White Pill");
    if (s == QLatin1String("elegance_serif")) return QStringLiteral("Elegance Serif");
    if (s == QLatin1String("classic_clean_stroke")) return QStringLiteral("Clean Outline");
    return QStringLiteral("Black Pill");
}

QPixmap renderCreativeCardPixmap(const QString &imagePath,
                                 const QString &shortCode,
                                 const QString &productSummary,
                                 const QString &badgeText,
                                 const QColor &badgeColor,
                                 const QSize &targetSize,
                                 const QString &overlayPreviewText = QString{},
                                 const QString &overlayStyleId = QString{})
{
    QImage canvas(targetSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, targetSize.width(), targetSize.height()), 8.0, 8.0);
    p.setClipPath(clip);

    QImage src = imagePath.isEmpty() ? QImage{} : QImage{imagePath};
    if (!src.isNull())
    {
        const QImage scaled = src.scaled(targetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        const int ox = (scaled.width() - targetSize.width()) / 2;
        const int oy = (scaled.height() - targetSize.height()) / 2;
        p.drawImage(0, 0, scaled, ox, oy, targetSize.width(), targetSize.height());
    }
    else
    {
        QLinearGradient grad(0, 0, targetSize.width(), targetSize.height());
        grad.setColorAt(0.0, QColor(34, 41, 56));
        grad.setColorAt(1.0, QColor(18, 22, 32));
        p.fillRect(canvas.rect(), grad);

        QFont f = p.font();
        f.setBold(true);
        f.setPixelSize(qMax(11, targetSize.width() / 11));
        p.setFont(f);
        p.setPen(QColor(210, 220, 235));
        p.drawText(QRect(8, targetSize.height() / 3, targetSize.width() - 16, targetSize.height() / 2),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   productSummary.isEmpty() ? shortCode : productSummary.left(42));
    }

    if (!badgeText.isEmpty())
    {
        QFont bf = p.font();
        bf.setBold(true);
        bf.setPixelSize(qMax(9, qMin(13, targetSize.width() / 10)));
        p.setFont(bf);
        const QRect fm = p.fontMetrics().boundingRect(badgeText);
        const QRect pill(6, 6, qMin(targetSize.width() - 12, fm.width() + 12), fm.height() + 6);
        p.setPen(Qt::NoPen);
        QColor bg = badgeColor;
        bg.setAlpha(220);
        p.setBrush(bg);
        p.drawRoundedRect(pill, 5.0, 5.0);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, badgeText);
    }

    const QString cleanOverlay = strategicWrapOverlayText(overlayPreviewText, 24);
    if (!cleanOverlay.isEmpty())
    {
        const QString styleKey = normalizeVideoTextStyleId(overlayStyleId);
        QFont tf = p.font();
        if (styleKey == QLatin1String("elegance_serif"))
        {
            tf.setFamily(QStringLiteral("Georgia"));
            tf.setStyleHint(QFont::Serif);
        }
        tf.setBold(true);
        tf.setPixelSize(qMax(8, qMin(14, targetSize.width() / 15)));
        p.setFont(tf);
        const int marginX = qMax(6, targetSize.width() / 10);
        const int maxW = qMax(36, targetSize.width() - 2 * marginX);
        const QRect tb = p.fontMetrics().boundingRect(
            QRect(0, 0, maxW, targetSize.height() / 2),
            Qt::AlignCenter | Qt::TextWordWrap,
            cleanOverlay);
        const int padX = qMax(5, targetSize.width() / 28);
        const int padY = qMax(3, targetSize.height() / 65);
        const int boxW = qMin(targetSize.width() - 8, tb.width() + 2 * padX);
        const int boxH = tb.height() + 2 * padY;
        const QRect pillRect((targetSize.width() - boxW) / 2,
                             qMax(24, int(targetSize.height() * 0.19)),
                             boxW,
                             boxH);
        const QRect textRect = pillRect.adjusted(padX, padY, -padX, -padY);

        if (styleKey == QLatin1String("classic_white_pill"))
        {
            p.setPen(QPen(QColor(0, 0, 0, 45), 1.0));
            p.setBrush(QColor(250, 250, 252, 238));
            p.drawRoundedRect(pillRect, 6.0, 6.0);
            p.setPen(QColor(17, 24, 39));
            p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, cleanOverlay);
        }
        else if (styleKey == QLatin1String("elegance_serif")
                 || styleKey == QLatin1String("classic_clean_stroke"))
        {
            // Outlined / drop-shadowed white text with NO rectangular pill
            p.setPen(QColor(10, 12, 18, 230));
            for (const QPoint &off : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1), QPoint(1, 2)})
            {
                p.drawText(textRect.translated(off), Qt::AlignCenter | Qt::TextWordWrap, cleanOverlay);
            }
            p.setPen(Qt::white);
            p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, cleanOverlay);
        }
        else
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(12, 14, 20, 218));
            p.drawRoundedRect(pillRect, 6.0, 6.0);
            p.setPen(Qt::white);
            p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, cleanOverlay);
        }
    }

    p.setClipping(false);
    p.setPen(QPen(QColor(255, 255, 255, 38), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(0.5, 0.5, targetSize.width() - 1.0, targetSize.height() - 1.0), 8.0, 8.0);
    p.end();
    return QPixmap::fromImage(canvas);
}

QString cleanShortHookForOverlay(const QString &rawHook, const QString &productSummary)
{
    QString text = rawHook.trimmed();
    text.remove(QRegularExpression{QStringLiteral("(?:#\\s*)[A-Za-z0-9_]+")});
    text.remove(QRegularExpression{QStringLiteral("\\bqs[-_]?[0-9a-z]+\\b"),
                                   QRegularExpression::CaseInsensitiveOption});
    text = text.simplified();
    if (text.size() > 62)
    {
        const int sp = text.lastIndexOf(QLatin1Char(' '), 58);
        text = (sp > 20 ? text.left(sp) : text.left(58)).trimmed();
    }
    if (text.isEmpty())
    {
        QString prod = productSummary.section(QStringLiteral(" — "), 0, 0).trimmed();
        if (prod.isEmpty())
        {
            prod = QStringLiteral("these heels");
        }
        text = QObject::tr("How would you style %1?").arg(prod);
    }
    return text;
}

QString readCaptionFromGenDir(const QDir &genDir,
                              const QDir &tempDir,
                              const QString &shortCode,
                              QString *outHook)
{
    if (outHook)
    {
        outHook->clear();
    }
    QFile f{genDir.absoluteFilePath(QStringLiteral("hook-description.txt"))};
    QString hook;
    QString desc;
    if (f.open(QIODevice::ReadOnly))
    {
        const QString content = QString::fromUtf8(f.readAll());
        const int dbl = content.indexOf(QStringLiteral("\n\n"));
        if (dbl >= 0)
        {
            hook = content.left(dbl).trimmed();
            desc = content.mid(dbl + 2).trimmed();
        }
        else
        {
            const int sgl = content.indexOf(QLatin1Char('\n'));
            if (sgl >= 0)
            {
                hook = content.left(sgl).trimmed();
                desc = content.mid(sgl + 1).trimmed();
            }
            else
            {
                desc = content.trimmed();
            }
        }
    }

    // Fallback 1: hooks.json in genDir or tempDir (if user generated suggestions without saving a single pick)
    if (hook.isEmpty() && desc.isEmpty())
    {
        for (const QString &candPath : {genDir.filePath(QStringLiteral("hooks.json")),
                                        tempDir.filePath(QStringLiteral("hooks.json"))})
        {
            QFile hf{candPath};
            if (hf.open(QIODevice::ReadOnly))
            {
                const QJsonArray arr = QJsonDocument::fromJson(hf.readAll()).object()
                    .value(QStringLiteral("hooks")).toArray();
                if (!arr.isEmpty())
                {
                    const QJsonObject firstObj = arr.first().toObject();
                    hook = firstObj.value(QStringLiteral("hook")).toString().trimmed();
                    desc = firstObj.value(QStringLiteral("description")).toString().trimmed();
                    if (!hook.isEmpty() || !desc.isEmpty())
                    {
                        break;
                    }
                }
            }
        }
    }

    if (outHook)
    {
        *outHook = hook;
    }
    QString combined;
    if (!hook.isEmpty() && !desc.isEmpty())
    {
        combined = hook + QStringLiteral(" — ") + desc;
    }
    else if (!desc.isEmpty())
    {
        combined = desc;
    }
    else
    {
        combined = hook;
    }
    const QString tag = codeTagFromShortCodePub(shortCode);
    if (!tag.isEmpty()
        && AbstractSocialAccount::extractShortCode(combined).compare(shortCode, Qt::CaseInsensitive) != 0
        && !combined.contains(tag, Qt::CaseInsensitive))
    {
        combined = (combined.isEmpty() ? QString{} : combined + QLatin1Char(' ')) + tag;
    }
    return combined.trimmed();
}

} // namespace

PanePublishing::PanePublishing(TableAccounts *accounts,
                               TableProjects *projects,
                               TableVideos *videos,
                               const QList<AbstractCli *> &availableClis,
                               QWidget *parent,
                               PreferredHashtags *hashtags,
                               FavoriteHooks *favoriteHooks)
    : QWidget(parent)
    , m_accounts(accounts)
    , m_projects(projects)
    , m_videos(videos)
    , m_hashtags(hashtags)
    , m_favoriteHooks(favoriteHooks)
    , m_clis(availableClis)
    , m_cancelFlag(std::make_shared<bool>(false))
{
    auto *root = new QVBoxLayout(this);

    // Top control bar
    auto *topBar = new QHBoxLayout;
    m_btnNewJob = new QPushButton(tr("Auto-Publish Job…"));
    m_btnNewJob->setObjectName(QStringLiteral("buttonPublishingNewJob"));
    m_btnNewJob->setStyleSheet(QStringLiteral("font-weight: 600; padding: 5px 12px;"));

    m_btnPublishNextNow = new QPushButton(tr("Publish Next Now"));
    m_btnPublishNextNow->setObjectName(QStringLiteral("buttonPublishNextNow"));
    m_btnPublishNextNow->setToolTip(tr("Skip the scheduled wait timer and publish the next queued creative immediately."));
    m_btnPublishNextNow->setEnabled(false);

    m_btnStopJob = new QPushButton(tr("Stop Job"));
    m_btnStopJob->setObjectName(QStringLiteral("buttonStopPublishJob"));
    m_btnStopJob->setStyleSheet(QStringLiteral("color: #e89774; font-weight: 600;"));
    m_btnStopJob->setEnabled(false);

    m_btnReconnect = new QPushButton(tr("Reconnect…"));
    m_btnReconnect->setObjectName(QStringLiteral("buttonReconnectPublishBrowser"));
    m_btnReconnect->setToolTip(
        tr("Open Chrome on this social account's profile so you can log in, log out, or switch accounts manually."));

    m_btnOpenDebugDir = new QPushButton(tr("Open Debug Folder"));
    m_btnOpenDebugDir->setObjectName(QStringLiteral("buttonOpenPublishDebugDir"));
    m_btnOpenDebugDir->setToolTip(tr("Open ~/.cache/quantumsocial/publish_logs/ (screenshots + DOM dumps + Chrome logs)."));

    m_btnClearLogs = new QPushButton(tr("Clear Logs"));

    topBar->addWidget(m_btnNewJob);
    topBar->addWidget(m_btnPublishNextNow);
    topBar->addWidget(m_btnStopJob);
    topBar->addStretch();
    topBar->addWidget(m_btnReconnect);
    topBar->addWidget(m_btnOpenDebugDir);
    topBar->addWidget(m_btnClearLogs);
    root->addLayout(topBar);

    m_statusLabel = new QLabel(tr("Idle — click \"Auto-Publish Job…\" (or \"Auto-publish…\" in Generation) to schedule & publish creatives."));
    m_statusLabel->setObjectName(QStringLiteral("labelPublishJobStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "background: rgba(128, 140, 160, 0.12); border: 1px solid rgba(128, 140, 160, 0.28); "
        "border-radius: 5px; padding: 6px 10px;"));
    root->addWidget(m_statusLabel);

    auto *splitter = new QSplitter(Qt::Vertical);
    root->addWidget(splitter, 1);

    // Upper pane: Planned schedule queue
    auto *queueBox = new QWidget;
    auto *queueLayout = new QVBoxLayout(queueBox);
    queueLayout->setContentsMargins(0, 0, 0, 0);
    queueLayout->addWidget(new QLabel(tr("Planned Publish Queue (AI-selected order & spacing):")));

    m_queueTable = new QTableWidget(0, 8);
    m_queueTable->setObjectName(QStringLiteral("tablePublishQueue"));
    m_queueTable->setHorizontalHeaderLabels({
        tr("#"), tr("Code"), tr("Age"), tr("Format"),
        tr("Product / Project"), tr("Target Account"), tr("Scheduled Time"), tr("Status / AI Reason")
    });
    m_queueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queueTable->verticalHeader()->setVisible(false);
    m_queueTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_queueTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_queueTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_queueTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_queueTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    m_queueTable->horizontalHeader()->resizeSection(4, 220);
    m_queueTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    m_queueTable->horizontalHeader()->resizeSection(5, 220);
    m_queueTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_queueTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    queueLayout->addWidget(m_queueTable, 1);
    splitter->addWidget(queueBox);

    // Lower pane: Live diagnostic console
    auto *logBox = new QWidget;
    auto *logLayout = new QVBoxLayout(logBox);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->addWidget(new QLabel(tr("Live Diagnostic Logs (browser profile, upload progress, login/CAPTCHA gates, & error dumps):")));

    m_logConsole = new QPlainTextEdit;
    m_logConsole->setObjectName(QStringLiteral("textPublishLogs"));
    m_logConsole->setReadOnly(true);
    m_logConsole->setMaximumBlockCount(5000);
    m_logConsole->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { font-family: monospace; font-size: 12px; }"));
    logLayout->addWidget(m_logConsole, 1);
    splitter->addWidget(logBox);
    splitter->setSizes({300, 340});

    m_waitTimer = new QTimer(this);
    m_waitTimer->setSingleShot(true);
    connect(m_waitTimer, &QTimer::timeout, this, &PanePublishing::_launchNextQueuedItem);

    m_countdownUiTimer = new QTimer(this);
    m_countdownUiTimer->setInterval(15'000);
    connect(m_countdownUiTimer, &QTimer::timeout, this, [this]() {
        if (!m_jobActive || m_uploadingNow)
        {
            return;
        }
        for (int i = 0; i < m_queue.size(); ++i)
        {
            if (m_queue[i].status == QLatin1String("Queued"))
            {
                const qint64 secsLeft = qMax<qint64>(0, QDateTime::currentDateTime().secsTo(m_queue[i].scheduledAt));
                m_statusLabel->setText(
                    tr("Waiting to publish #%1 (%2 → %3) at %4 (%5 min remaining) — click \"Publish Next Now\" to post immediately.")
                        .arg(i + 1)
                        .arg(m_queue[i].creative.shortCode,
                             m_queue[i].target.accountUrl,
                             m_queue[i].scheduledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                        .arg((secsLeft + 59) / 60));
                break;
            }
        }
    });

    connect(m_btnNewJob, &QPushButton::clicked, this, &PanePublishing::openAutoPublishDialog);
    connect(m_btnPublishNextNow, &QPushButton::clicked, this, &PanePublishing::publishNextNow);
    connect(m_btnStopJob, &QPushButton::clicked, this, &PanePublishing::stopJob);
    connect(m_btnReconnect, &QPushButton::clicked, this, [this]() { reconnectBrowserSession(); });
    connect(m_btnOpenDebugDir, &QPushButton::clicked, this, &PanePublishing::openDebugLogsFolder);
    connect(m_btnClearLogs, &QPushButton::clicked, m_logConsole, &QPlainTextEdit::clear);
}

PanePublishing::~PanePublishing()
{
    if (m_cancelFlag)
    {
        *m_cancelFlag = true;
    }
}

void PanePublishing::reconnectBrowserSession(const QString &accountUrl)
{
    if (m_uploadingNow)
    {
        QMessageBox::information(this, tr("Upload in progress"),
            tr("Click \"Stop Job\" first before opening the browser manually."));
        return;
    }

    QString chosenUrl = accountUrl.trimmed();
    if (chosenUrl.isEmpty() && m_accounts)
    {
        QStringList labels;
        QStringList urls;
        for (int r = 0; r < m_accounts->rowCount(); ++r)
        {
            const QString typeStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_TYPE)).toString();
            const QString urlStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_URL)).toString();
            AbstractSocialAccount *platform = SocialStatistics::resolveSocialAccount(typeStr);
            if (platform && platform->supportsAutoPublish() && !urlStr.trimmed().isEmpty())
            {
                labels << QStringLiteral("%1 — %2").arg(typeStr, urlStr);
                urls << urlStr.trimmed();
            }
        }
        if (urls.isEmpty())
        {
            QMessageBox::information(this, tr("No supported account"),
                tr("Add a supported social account (e.g. TikTok) in the Accounts tab first."));
            return;
        }
        if (urls.size() == 1)
        {
            chosenUrl = urls.first();
        }
        else
        {
            bool ok = false;
            const QString picked = QInputDialog::getItem(
                this,
                tr("Reconnect Browser Session"),
                tr("Choose the social account whose Chrome session you want to open:"),
                labels, 0, false, &ok);
            if (!ok)
            {
                return;
            }
            const int idx = labels.indexOf(picked);
            if (idx >= 0)
            {
                chosenUrl = urls[idx];
            }
        }
    }

    static const QRegularExpression handleRe{QStringLiteral("/@([A-Za-z0-9._-]+)")};
    const auto m = handleRe.match(chosenUrl);
    QString handle = m.hasMatch() ? m.captured(1).toLower() : QStringLiteral("default");
    static const QRegularExpression nonSafeRe{QStringLiteral("[^A-Za-z0-9._-]")};
    handle.replace(nonSafeRe, QStringLiteral("_"));

    const QString profileDir = QDir::home().filePath(
        QStringLiteral(".cache/quantumsocial/publish-tiktok-") + handle);
    const QString legacyDir = QDir::home().filePath(
        QStringLiteral(".cache/quantumsocial/tiktok-profile"));
    if (!QFileInfo::exists(profileDir) && QFileInfo::exists(legacyDir))
    {
        QDir{}.rename(legacyDir, profileDir);
    }
    QDir{}.mkpath(profileDir);

    const QString targetPage = QStringLiteral("https://www.tiktok.com/tiktokstudio/upload");

    QString chromeBin;
    if (QFileInfo::exists(QStringLiteral("/opt/google/chrome/chrome")))
    {
        chromeBin = QStringLiteral("/opt/google/chrome/chrome");
    }
    if (chromeBin.isEmpty())
    {
        for (const QString &candidate : {QStringLiteral("google-chrome"),
                                         QStringLiteral("google-chrome-stable"),
                                         QStringLiteral("chromium"),
                                         QStringLiteral("chromium-browser")})
        {
            const QString found = QStandardPaths::findExecutable(candidate);
            if (!found.isEmpty())
            {
                chromeBin = found;
                break;
            }
        }
    }

    bool launched = false;
    if (!chromeBin.isEmpty())
    {
        launched = QProcess::startDetached(
            chromeBin,
            {QStringLiteral("--user-data-dir=") + profileDir,
             QStringLiteral("--no-first-run"),
             QStringLiteral("--no-default-browser-check"),
             targetPage});
    }
    if (!launched)
    {
        const QString pyBin = QStandardPaths::findExecutable(QStringLiteral("python3"));
        const QString scriptPath = QDir{QStringLiteral(SOCIAL_STATS_SCRIPT_DIR)}
            .filePath(QStringLiteral("publish_tiktok.py"));
        if (!pyBin.isEmpty() && QFileInfo::exists(scriptPath))
        {
            launched = QProcess::startDetached(
                pyBin, {scriptPath, QStringLiteral("--reconnect"), chosenUrl});
        }
    }

    if (launched)
    {
        const QString msg = tr("Opened Chrome for @%1 (%2) — log in or switch accounts there, then close that Chrome window when done.")
            .arg(handle, profileDir);
        m_statusLabel->setText(msg);
        _appendLog(QStringLiteral("[RECONNECT] ") + msg);
    }
    else
    {
        QMessageBox::warning(this, tr("Could not launch browser"),
            tr("Could not find Google Chrome or Chromium to open the session."));
    }
}

void PanePublishing::openDebugLogsFolder()
{
    const QString logDir = QDir::home().filePath(QStringLiteral(".cache/quantumsocial/publish_logs"));
    QDir{}.mkpath(logDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(logDir));
}

void PanePublishing::_appendLog(const QString &line)
{
    if (!m_logConsole || line.trimmed().isEmpty())
    {
        return;
    }
    const QString trimmed = line.trimmed();
    const bool alreadyTimestamped = trimmed.startsWith(QLatin1Char('['));
    const QString formatted = alreadyTimestamped
        ? trimmed
        : QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), trimmed);
    m_logConsole->appendPlainText(formatted);
}

QList<PanePublishing::EligibleCreative> PanePublishing::_collectEligibleCreatives(
    int maxDaysOld, bool skipSingleImages, int *outOlderExcluded, int *outSingleImagesExcluded) const
{
    if (outOlderExcluded)
    {
        *outOlderExcluded = 0;
    }
    if (outSingleImagesExcluded)
    {
        *outSingleImagesExcluded = 0;
    }
    QList<EligibleCreative> list;
    if (!m_videos || !m_projects)
    {
        return list;
    }

    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    for (int r = 0; r < m_videos->rowCount(); ++r)
    {
        const auto *rec = m_videos->recordAt(r);
        if (!rec || rec->published || !rec->publications.isEmpty())
        {
            continue;
        }
        const int projRow = m_projects->rowOfId(rec->projectId);
        if (projRow < 0)
        {
            continue;
        }
        const QDir genDir = m_projects->generationDir(projRow, rec->shortCode, false);
        if (!genDir.exists())
        {
            continue;
        }

        // Gather media files at top level of the generation folder
        QStringList mediaPaths;
        QString format;
        if (genDir.exists(QStringLiteral("final_video.mp4")))
        {
            mediaPaths << genDir.absoluteFilePath(QStringLiteral("final_video.mp4"));
            format = QStringLiteral("video");
        }
        else
        {
            const QStringList vidFiles = genDir.entryList(
                {QStringLiteral("*.mp4"), QStringLiteral("*.mov"), QStringLiteral("*.webm")},
                QDir::Files, QDir::Name);
            if (!vidFiles.isEmpty())
            {
                mediaPaths << genDir.absoluteFilePath(vidFiles.first());
                format = QStringLiteral("video");
            }
            else
            {
                const QStringList imgFiles = genDir.entryList(
                    {QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.webp")},
                    QDir::Files, QDir::Name);
                for (const QString &img : imgFiles)
                {
                    mediaPaths << genDir.absoluteFilePath(img);
                }
                if (!mediaPaths.isEmpty())
                {
                    format = mediaPaths.size() > 1 ? QStringLiteral("carousel") : QStringLiteral("image");
                }
            }
        }
        if (mediaPaths.isEmpty())
        {
            continue;
        }
        // TikTok requires either a Video or a multi-image Slideshow (>= 2 images); skip lone single images
        if (skipSingleImages && format == QLatin1String("image"))
        {
            if (outSingleImagesExcluded)
            {
                ++(*outSingleImagesExcluded);
            }
            continue;
        }

        const qint64 elapsedSecs = qMax<qint64>(0, rec->generatedDate.toUTC().secsTo(nowUtc));
        const int ageDays = int(elapsedSecs / 86400);
        if (maxDaysOld > 0 && ageDays > maxDaysOld)
        {
            if (outOlderExcluded)
            {
                ++(*outOlderExcluded);
            }
            continue;
        }

        EligibleCreative item;
        item.videoId = rec->id;
        item.projectId = rec->projectId;
        item.projectRow = projRow;
        item.shortCode = rec->shortCode;
        item.ageDays = ageDays;
        item.format = format;
        item.mediaPaths = mediaPaths;
        item.projectName = m_projects->data(m_projects->index(projRow, TableProjects::IND_NAME)).toString();
        const QDir tempDir = m_projects->generationTempDir(projRow, rec->shortCode);
        item.previewImagePath = resolveCreativePreviewImagePath(genDir, tempDir, format, mediaPaths, false);
        item.caption = readCaptionFromGenDir(genDir, tempDir, rec->shortCode, &item.hook);

        QFile dnaFile{genDir.filePath(QStringLiteral("creative_dna.json"))};
        if (dnaFile.open(QIODevice::ReadOnly))
        {
            const QJsonObject dna = QJsonDocument::fromJson(dnaFile.readAll()).object();
            const QString pColor = dna.value(QStringLiteral("productColor")).toString().trimmed();
            const QString pTitle = dna.value(QStringLiteral("productTitle")).toString().trimmed();
            item.productSummary = !pColor.isEmpty() && !pTitle.isEmpty()
                ? QStringLiteral("%1 — %2").arg(pColor, pTitle)
                : (!pTitle.isEmpty() ? pTitle : item.projectName);
            item.hookCategory = dna.value(QStringLiteral("hookCategory")).toString();
            item.strategy = dna.value(QStringLiteral("strategy")).toString();
            item.referenceUrl = dna.value(QStringLiteral("referenceUrl")).toString();
            item.dnaAccountId = dna.value(QStringLiteral("accountId")).toString();
        }
        else
        {
            item.productSummary = item.projectName;
        }
        list.append(item);
    }

    // Oldest unpublished creatives first by default so mature generations get published in FIFO order
    std::stable_sort(list.begin(), list.end(),
                     [](const EligibleCreative &a, const EligibleCreative &b) {
                         if (a.ageDays != b.ageDays)
                         {
                             return a.ageDays > b.ageDays;
                         }
                         return a.shortCode < b.shortCode;
                     });
    return list;
}

void PanePublishing::openAutoPublishDialog()
{
    if (m_jobActive)
    {
        QMessageBox::information(this, tr("Job already running"),
            tr("An Auto-Publish job is currently active. Click \"Stop Job\" first if you want to start a new one."));
        return;
    }
    if (!m_accounts || m_accounts->rowCount() == 0)
    {
        QMessageBox::information(this, tr("No social accounts"),
            tr("Add at least one social account (e.g. TikTok) in the Accounts tab first."));
        return;
    }

    QSettings settings;
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("dialogAutoPublish"));
    dlg.setWindowTitle(tr("Auto-Publish Creatives"));
    dlg.resize(720, 540);
    auto *layout = new QVBoxLayout(&dlg);

    auto *intro = new QLabel(
        tr("Select target social account(s) and pacing rules below. The AI planner will choose "
           "the best unpublished creatives, order them for product & format variety, and space "
           "them across the day while streaming detailed diagnostic logs in the Publishing tab."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *accHeaderRow = new QHBoxLayout;
    accHeaderRow->addWidget(new QLabel(tr("1. Social media account(s) to publish to:")));
    accHeaderRow->addStretch();
    auto *btnDlgReconnect = new QPushButton(tr("Reconnect…"));
    btnDlgReconnect->setObjectName(QStringLiteral("buttonDialogReconnectBrowser"));
    btnDlgReconnect->setToolTip(tr("Open Chrome on the selected account's profile so you can log in or switch accounts manually."));
    accHeaderRow->addWidget(btnDlgReconnect);
    layout->addLayout(accHeaderRow);

    auto *accList = new QListWidget;
    accList->setObjectName(QStringLiteral("autoPublishAccountsList"));
    int supportedCount = 0;
    for (int r = 0; r < m_accounts->rowCount(); ++r)
    {
        const QString typeStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_TYPE)).toString();
        const QString urlStr = m_accounts->data(m_accounts->index(r, TableAccounts::IND_URL)).toString();
        const qint64 followers = m_accounts->data(m_accounts->index(r, TableAccounts::IND_FOLLOWERS)).toLongLong();
        AbstractSocialAccount *platform = SocialStatistics::resolveSocialAccount(typeStr);
        const bool supported = platform && platform->supportsAutoPublish();

        QString label = QStringLiteral("%1 — %2").arg(typeStr, urlStr);
        if (!supported)
        {
            label += tr(" (auto-publish coming soon)");
        }
        auto *item = new QListWidgetItem(label, accList);
        item->setData(Qt::UserRole, platform ? platform->getId() : QString{});
        item->setData(Qt::UserRole + 1, platform ? platform->getName() : typeStr);
        item->setData(Qt::UserRole + 2, urlStr);
        item->setData(Qt::UserRole + 3, followers);
        if (supported)
        {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
            ++supportedCount;
        }
        else
        {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setCheckState(Qt::Unchecked);
        }
    }
    layout->addWidget(accList, 1);

    auto *form = new QFormLayout;

    auto *spinMaxDays = new QSpinBox;
    spinMaxDays->setObjectName(QStringLiteral("spinPublishMaxDaysOld"));
    spinMaxDays->setRange(0, 365);
    spinMaxDays->setSpecialValueText(tr("No limit (all ages)"));
    spinMaxDays->setSuffix(tr(" day(s) or less"));
    spinMaxDays->setToolTip(
        tr("Publish unpublished creatives generated within the last N days (e.g. 10 = created 10 days ago or less). Set to 0 for no age limit."));
    spinMaxDays->setValue(settings.value(QStringLiteral("publishing/maxGenerationAgeDays"), 10).toInt());
    form->addRow(tr("2. Maximum generation age:"), spinMaxDays);

    auto *eligibleInfo = new QLabel;
    eligibleInfo->setObjectName(QStringLiteral("labelEligiblePublishCount"));
    eligibleInfo->setWordWrap(true);
    form->addRow(QString{}, eligibleInfo);

    auto *spinPerDay = new QSpinBox;
    spinPerDay->setObjectName(QStringLiteral("spinPublishPostsPerDay"));
    spinPerDay->setRange(1, 24);
    spinPerDay->setValue(settings.value(QStringLiteral("publishing/postsPerDay"), 3).toInt());
    form->addRow(tr("3. Max posts per day:"), spinPerDay);

    auto *spinTotal = new QSpinBox;
    spinTotal->setObjectName(QStringLiteral("spinPublishTotalPosts"));
    spinTotal->setRange(1, 100);
    spinTotal->setValue(settings.value(QStringLiteral("publishing/totalPosts"), 6).toInt());
    form->addRow(tr("4. Total posts in this job:"), spinTotal);

    auto *spacingBox = new QWidget;
    auto *spacingLayout = new QVBoxLayout(spacingBox);
    spacingLayout->setContentsMargins(0, 0, 0, 0);
    spacingLayout->setSpacing(4);

    auto *radioDaytime12h = new QRadioButton;
    radioDaytime12h->setObjectName(QStringLiteral("radioSpacingDaytime12h"));
    auto *radioEven24h = new QRadioButton;
    radioEven24h->setObjectName(QStringLiteral("radioSpacingEven24h"));

    auto *fixedRow = new QHBoxLayout;
    fixedRow->setContentsMargins(0, 0, 0, 0);
    auto *radioFixed = new QRadioButton(tr("Fixed interval per account:"));
    radioFixed->setObjectName(QStringLiteral("radioSpacingFixed"));
    auto *spinFixedMins = new QSpinBox;
    spinFixedMins->setObjectName(QStringLiteral("spinPublishFixedMinutes"));
    spinFixedMins->setRange(5, 1440);
    spinFixedMins->setSingleStep(15);
    spinFixedMins->setSuffix(tr(" min"));
    spinFixedMins->setValue(settings.value(QStringLiteral("publishing/fixedIntervalMinutes"), 60).toInt());
    fixedRow->addWidget(radioFixed);
    fixedRow->addWidget(spinFixedMins);
    fixedRow->addStretch();

    const QString savedSpacingMode = settings.value(QStringLiteral("publishing/spacingMode"), QStringLiteral("daytime12h")).toString();
    if (savedSpacingMode == QLatin1String("even24h"))
    {
        radioEven24h->setChecked(true);
    }
    else if (savedSpacingMode == QLatin1String("fixed"))
    {
        radioFixed->setChecked(true);
    }
    else
    {
        radioDaytime12h->setChecked(true);
    }
    spinFixedMins->setEnabled(radioFixed->isChecked());
    connect(radioFixed, &QRadioButton::toggled, spinFixedMins, &QWidget::setEnabled);

    const auto updateSpacingLabels = [spinPerDay, radioDaytime12h, radioEven24h]() {
        const int n = qMax(1, spinPerDay->value());
        const int dayMins = qMax(15, 720 / n);
        const int evenMins = qMax(15, 1440 / n);
        radioDaytime12h->setText(
            QObject::tr("Active daytime spread (12h / %1 = ~%2 min apart + overnight pause)")
                .arg(n).arg(dayMins));
        const double evenHours = double(evenMins) / 60.0;
        radioEven24h->setText(
            QObject::tr("Even 24h spread (24h / %1 = %2 min / %3h apart)")
                .arg(n).arg(evenMins).arg(QString::number(evenHours, 'f', (evenMins % 60 == 0) ? 0 : 1)));
    };
    connect(spinPerDay, &QSpinBox::valueChanged, &dlg, updateSpacingLabels);
    updateSpacingLabels();

    spacingLayout->addWidget(radioDaytime12h);
    spacingLayout->addWidget(radioEven24h);
    spacingLayout->addLayout(fixedRow);
    form->addRow(tr("5. Wait between posts:"), spacingBox);

    auto *cliCombo = new QComboBox;
    cliCombo->setObjectName(QStringLiteral("comboPublishPlannerCli"));
    for (AbstractCli *cli : m_clis)
    {
        cliCombo->addItem(cli->getName(), QVariant::fromValue(cli));
    }
    if (cliCombo->count() == 0)
    {
        cliCombo->addItem(tr("(Built-in diversity scheduler — no AI CLI needed)"), QVariant{});
    }
    else
    {
        const QString lastCli = settings.value(QStringLiteral("publishing/lastPlannerCli")).toString();
        int idx = cliCombo->findText(lastCli);
        if (idx >= 0) cliCombo->setCurrentIndex(idx);
    }
    form->addRow(tr("6. AI schedule planner:"), cliCombo);

    auto *chkSound = new QCheckBox(
        tr("Mute original video audio & add 1st suggested TikTok sound (recommended for FYP reach)"));
    chkSound->setObjectName(QStringLiteral("chkPublishSuggestedSound"));
    chkSound->setChecked(settings.value(QStringLiteral("publishing/useSuggestedSound"), true).toBool());
    form->addRow(QString{}, chkSound);

    auto *chkDryRun = new QCheckBox(
        tr("Dry-run test mode (upload media + fill caption in Chrome, stop right before clicking Post)"));
    chkDryRun->setObjectName(QStringLiteral("chkPublishDryRun"));
    chkDryRun->setChecked(settings.value(QStringLiteral("publishing/dryRun"), false).toBool());
    form->addRow(QString{}, chkDryRun);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    if (auto *okBtn = buttons->button(QDialogButtonBox::Ok))
    {
        okBtn->setText(tr("Next: AI Plan & Review Selection…"));
        okBtn->setObjectName(QStringLiteral("buttonStartAutoPublish"));
    }
    layout->addWidget(buttons);

    const auto refreshCounts = [&]() {
        int checkedAccounts = 0;
        bool hasTikTokTarget = false;
        for (int i = 0; i < accList->count(); ++i)
        {
            if (accList->item(i)->checkState() == Qt::Checked)
            {
                ++checkedAccounts;
                if (accList->item(i)->data(Qt::UserRole).toString() == QLatin1String("tiktok"))
                {
                    hasTikTokTarget = true;
                }
            }
        }
        int olderExcluded = 0;
        int singleImagesExcluded = 0;
        const auto eligible = _collectEligibleCreatives(
            spinMaxDays->value(), hasTikTokTarget, &olderExcluded, &singleImagesExcluded);
        const QString singleImgNote = singleImagesExcluded > 0
            ? tr(", %1 single-image skipped for TikTok").arg(singleImagesExcluded)
            : QString{};
        if (!eligible.isEmpty())
        {
            eligibleInfo->setStyleSheet(QStringLiteral("color: #8fd19e; font-weight: 600;"));
            if (spinMaxDays->value() > 0)
            {
                eligibleInfo->setText(
                    tr("%1 unpublished creative(s) created ≤ %2 day(s) ago ready (%3 older > %2d skipped%4).")
                        .arg(eligible.size()).arg(spinMaxDays->value()).arg(olderExcluded).arg(singleImgNote));
            }
            else
            {
                eligibleInfo->setText(
                    tr("%1 unpublished creative(s) ready (all ages%2).").arg(eligible.size()).arg(singleImgNote));
            }
        }
        else if (olderExcluded > 0 || singleImagesExcluded > 0)
        {
            eligibleInfo->setStyleSheet(QStringLiteral("color: #fbbf24; font-weight: 600;"));
            eligibleInfo->setText(
                tr("0 eligible Videos / Slideshows created ≤ %1 day(s) ago (%2 older > %1d skipped%3).")
                    .arg(spinMaxDays->value()).arg(olderExcluded).arg(singleImgNote));
        }
        else
        {
            eligibleInfo->setStyleSheet(QStringLiteral("color: #e89774;"));
            eligibleInfo->setText(tr("No unpublished creatives found on disk yet. Generate creatives first."));
        }
        if (auto *okBtn = buttons->button(QDialogButtonBox::Ok))
        {
            okBtn->setEnabled(checkedAccounts > 0 && !eligible.isEmpty() && supportedCount > 0);
        }
    };

    connect(btnDlgReconnect, &QPushButton::clicked, &dlg, [&]() {
        QString targetUrl;
        if (auto *cur = accList->currentItem())
        {
            targetUrl = cur->data(Qt::UserRole + 2).toString();
        }
        if (targetUrl.isEmpty())
        {
            for (int i = 0; i < accList->count(); ++i)
            {
                if (accList->item(i)->checkState() == Qt::Checked)
                {
                    targetUrl = accList->item(i)->data(Qt::UserRole + 2).toString();
                    break;
                }
            }
        }
        reconnectBrowserSession(targetUrl);
    });
    connect(spinMaxDays, &QSpinBox::valueChanged, &dlg, [&]() { refreshCounts(); });
    connect(accList, &QListWidget::itemChanged, &dlg, [&]() { refreshCounts(); });
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    refreshCounts();

    if (dlg.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString chosenSpacingKey = radioEven24h->isChecked()
        ? QStringLiteral("even24h")
        : (radioFixed->isChecked() ? QStringLiteral("fixed") : QStringLiteral("daytime12h"));
    settings.setValue(QStringLiteral("publishing/maxGenerationAgeDays"), spinMaxDays->value());
    settings.setValue(QStringLiteral("publishing/postsPerDay"), spinPerDay->value());
    settings.setValue(QStringLiteral("publishing/totalPosts"), spinTotal->value());
    settings.setValue(QStringLiteral("publishing/spacingMode"), chosenSpacingKey);
    settings.setValue(QStringLiteral("publishing/fixedIntervalMinutes"), spinFixedMins->value());
    settings.setValue(QStringLiteral("publishing/useSuggestedSound"), chkSound->isChecked());
    settings.setValue(QStringLiteral("publishing/dryRun"), chkDryRun->isChecked());
    if (auto *cli = cliCombo->currentData().value<AbstractCli *>())
    {
        settings.setValue(QStringLiteral("publishing/lastPlannerCli"), cli->getName());
    }
    else
    {
        settings.remove(QStringLiteral("publishing/lastPlannerCli"));
    }

    PublishJobConfig cfg;
    cfg.maxDaysOld = spinMaxDays->value();
    cfg.postsPerDay = spinPerDay->value();
    cfg.totalPosts = spinTotal->value();
    cfg.spacingMode = radioEven24h->isChecked()
        ? SpacingMode::EvenSpread24h
        : (radioFixed->isChecked() ? SpacingMode::FixedInterval : SpacingMode::DaytimeSpread12h);
    cfg.fixedIntervalMinutes = spinFixedMins->value();
    cfg.muteOriginalAudio = chkSound->isChecked();
    cfg.useSuggestedSound = chkSound->isChecked();
    cfg.dryRun = chkDryRun->isChecked();
    cfg.reviewBeforePublish = true;
    cfg.plannerCli = cliCombo->currentData().value<AbstractCli *>();

    for (int i = 0; i < accList->count(); ++i)
    {
        auto *it = accList->item(i);
        if (it->checkState() == Qt::Checked)
        {
            cfg.targets.append(AccountTarget{
                it->data(Qt::UserRole).toString(),
                it->data(Qt::UserRole + 1).toString(),
                it->data(Qt::UserRole + 2).toString(),
                it->data(Qt::UserRole + 3).toLongLong()
            });
        }
    }
    startPublishJob(cfg);
}

bool PanePublishing::startPublishJob(const PublishJobConfig &config)
{
    if (m_jobActive || config.targets.isEmpty())
    {
        return false;
    }
    bool skipSingleImages = false;
    for (const auto &t : config.targets)
    {
        if (t.platformId == QLatin1String("tiktok"))
        {
            skipSingleImages = true;
            break;
        }
    }
    int olderExcluded = 0;
    int singleImagesExcluded = 0;
    const QList<EligibleCreative> candidates = _collectEligibleCreatives(
        config.maxDaysOld, skipSingleImages, &olderExcluded, &singleImagesExcluded);
    if (candidates.isEmpty())
    {
        _appendLog(tr("[PLANNER] Cannot start job: 0 unpublished Videos / Slideshows created ≤ %1 day(s) ago (%2 older excluded, %3 single-image excluded).")
            .arg(config.maxDaysOld).arg(olderExcluded).arg(singleImagesExcluded));
        return false;
    }

    m_activeConfig = config;
    m_queue.clear();
    m_currentIndex = -1;
    m_jobActive = true;
    m_uploadingNow = false;
    m_cancelFlag = std::make_shared<bool>(false);
    _refreshQueueTable();
    _updateActionButtons();

    m_planningTask = std::make_unique<QCoro::Task<void>>(_runJobCoroutine(config, candidates));
    return true;
}

QCoro::Task<void> PanePublishing::_runJobCoroutine(PublishJobConfig config,
                                                   QList<EligibleCreative> candidates)
{
    const auto cancelToken = m_cancelFlag;

    // Live pre-sync: scan target social account(s) first so any video already published
    // on TikTok (from phone, manual browser, or an earlier session) is marked published
    // in QuantumSocial BEFORE scheduling, preventing duplicates and keeping the app 100% in sync!
    if (config.uploadUrlOverride.isEmpty() && m_videos)
    {
        QStringList knownShortCodes;
        for (int r = 0; r < m_videos->rowCount(); ++r)
        {
            if (const auto *rec = m_videos->recordAt(r))
            {
                if (!rec->shortCode.isEmpty())
                {
                    knownShortCodes << rec->shortCode;
                }
            }
        }
        int syncedPublishedCount = 0;
        for (const AccountTarget &target : config.targets)
        {
            AbstractSocialAccount *platform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS()
                .value(target.platformId);
            if (!platform || !platform->supportsAccountVideoStatistics() || target.accountUrl.isEmpty())
            {
                continue;
            }
            _appendLog(tr("[SYNC] Checking %1 for already-published posts so the app stays in sync...")
                .arg(target.accountUrl));
            AbstractSocialAccount::AccountVideosResult syncRes;
            syncRes = co_await platform->fetchAccountVideoStatistics(target.accountUrl, knownShortCodes);
            if (cancelToken && *cancelToken)
            {
                m_jobActive = false;
                _updateActionButtons();
                co_return;
            }
            const qint64 liveFollowers = syncRes.followers > 0 ? syncRes.followers : target.followers;
            for (const auto &post : syncRes.posts)
            {
                const TableVideos::VideoRecord *matched = SocialStatistics::matchVideoRecord(m_videos, post);
                if (!matched)
                {
                    continue;
                }
                const bool wasUnpublished = !matched->published;
                const QString pUrl = !post.postUrl.isEmpty()
                    ? post.postUrl
                    : QStringLiteral("%1#%2").arg(target.accountUrl, matched->shortCode);
                const QDateTime pubDt = post.publishDate.isValid()
                    ? post.publishDate
                    : QDateTime::currentDateTimeUtc();
                m_videos->addPublication(matched->id, platform->getId(), pUrl, pubDt, liveFollowers);
                if (SocialStatistics::hasAnyVideoStat(post.stats))
                {
                    m_videos->addSnapshot(matched->id, pUrl, post.source, post.stats);
                }
                if (wasUnpublished)
                {
                    ++syncedPublishedCount;
                    _appendLog(tr("[SYNC] Marked %1 as PUBLISHED in app (already live at %2).")
                        .arg(matched->shortCode, pUrl));
                }
            }
        }
        if (syncedPublishedCount > 0)
        {
            bool skipSingle = false;
            for (const auto &t : config.targets)
            {
                if (t.platformId == QLatin1String("tiktok")) skipSingle = true;
            }
            int olderExcluded = 0;
            candidates = _collectEligibleCreatives(config.maxDaysOld, skipSingle, &olderExcluded);
            if (candidates.isEmpty())
            {
                m_jobActive = false;
                const QString syncDone = tr("Synced %1 live post(s) from TikTok — all eligible creatives are already published!")
                    .arg(syncedPublishedCount);
                m_statusLabel->setText(syncDone);
                _appendLog(QStringLiteral("[JOB-DONE] ") + syncDone);
                _updateActionButtons();
                co_return;
            }
        }
    }

    const int targetTotal = qMin(config.totalPosts, int(candidates.size()));
    const QString ageDesc = config.maxDaysOld > 0
        ? tr("age ≤ %1d").arg(config.maxDaysOld)
        : tr("all ages");
    _appendLog(tr("[JOB-START] Starting Auto-Publish job: selecting up to %1 of %2 eligible creative(s) "
                  "(%3, max %4/day, %5 target account(s), dryRun=%6).")
        .arg(targetTotal)
        .arg(candidates.size())
        .arg(ageDesc)
        .arg(config.postsPerDay)
        .arg(config.targets.size())
        .arg(config.dryRun ? QStringLiteral("true") : QStringLiteral("false")));
    m_statusLabel->setText(tr("Planning optimal publish order and timing with AI..."));

    // Summarize measured TikTok native video text ('Aa Texte') AND source-reference / adaptation-mode A/B performance!
    const auto videoTextExp = SocialStatistics::summarizeVideoTextExperiment(m_videos, m_favoriteHooks);
    const auto refExp = SocialStatistics::summarizeReferenceExperiment(m_videos);
    _appendLog(QStringLiteral("[LEARNING] ") + videoTextExp.humanSummary());
    _appendLog(QStringLiteral("[LEARNING] ") + refExp.humanSummary());
    if (videoTextExp.withTextCount > 0 || videoTextExp.withoutTextCount > 0 || refExp.totalMeasuredPosts > 0)
    {
        // Also persist updated experiment summaries into account_top_posts.json so Generation learns too
        const QDir wd = WorkingDirectoryManager::instance()->workingDir();
        if (wd.exists())
        {
            const QString topPath = wd.filePath(QStringLiteral("account_top_posts.json"));
            QJsonObject topRoot;
            {
                QFile tf{topPath};
                if (tf.open(QIODevice::ReadOnly))
                {
                    topRoot = QJsonDocument::fromJson(tf.readAll()).object();
                }
            }
            topRoot.insert(QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
            topRoot.insert(QStringLiteral("videoTextExperiment"), videoTextExp.toJson());
            topRoot.insert(QStringLiteral("referenceExperiment"), refExp.toJson());
            QSaveFile sf{topPath};
            if (sf.open(QIODevice::WriteOnly))
            {
                sf.write(QJsonDocument{topRoot}.toJson(QJsonDocument::Indented));
                sf.commit();
            }
        }
    }

    const auto defaultVideoTextFlag = [&](int videoOrdinal) -> bool {
        if (videoTextExp.recommendation == QLatin1String("prefer_text"))
        {
            return (videoOrdinal % 4) != 3; // ~75% with text, ~25% clean holdout
        }
        if (videoTextExp.recommendation == QLatin1String("prefer_clean"))
        {
            return (videoOrdinal % 4) == 3; // ~25% text exploration, ~75% clean
        }
        return (videoOrdinal % 2) == 0; // ~50/50 A/B split
    };

    const auto defaultVideoTextStyle = [&](int videoOrdinal) -> QString {
        const auto &styles = curatedVideoTextStyleIds();
        if (!videoTextExp.winningStyle.isEmpty() && (videoOrdinal % 3) != 2)
        {
            return normalizeVideoTextStyleId(videoTextExp.winningStyle);
        }
        return styles[videoOrdinal % styles.size()];
    };

    // Compute base interval from the selected radio-button SpacingMode
    const int perDayCap = qMax(1, config.postsPerDay);
    int baseIntervalMins = qMax(15, 720 / perDayCap);
    QString spacingDesc;
    if (config.spacingMode == SpacingMode::EvenSpread24h)
    {
        baseIntervalMins = qMax(15, 1440 / perDayCap);
        spacingDesc = tr("Even 24h spread (%1 min apart)").arg(baseIntervalMins);
    }
    else if (config.spacingMode == SpacingMode::FixedInterval)
    {
        baseIntervalMins = qMax(5, config.fixedIntervalMinutes);
        spacingDesc = tr("Fixed %1 min per account (max %2/day)").arg(baseIntervalMins).arg(perDayCap);
    }
    else
    {
        spacingDesc = tr("12h daytime spread (~%1 min apart + overnight pause)").arg(baseIntervalMins);
    }
    _appendLog(tr("[SCHEDULE] Spacing mode: %1").arg(spacingDesc));

    QJsonArray plannedJsonItems;
    if (config.plannerCli)
    {
        QString prompt = QStringLiteral(
            "You are scheduling an automated social-media publishing queue.\n"
            "Constraints:\n"
            "- Select exactly %1 creative(s) from the eligible unpublished candidates below.\n"
            "- Max posts per day per account: %2 (%3).\n"
            "- Item 0 MUST have \"delayMinutes\": 0 so the first post publishes immediately.\n"
            "- Maximize variety: alternate different products/colors, formats (video vs carousel vs image), "
            "and hook categories so consecutive posts never look repetitive.\n")
            .arg(targetTotal)
            .arg(config.postsPerDay)
            .arg(spacingDesc);

        prompt += videoTextExp.plannerPromptAddendum();
        prompt += refExp.plannerPromptAddendum();
        prompt += QStringLiteral(
            "  - When two candidates derive from the same sourceRef, avoid placing them back-to-back on the same day.\n");
        prompt += QStringLiteral("\nTarget accounts:\n");

        for (int a = 0; a < config.targets.size(); ++a)
        {
            prompt += QStringLiteral("  [%1] %2 — %3\n")
                .arg(a).arg(config.targets[a].platformName, config.targets[a].accountUrl);
        }
        prompt += QStringLiteral("\nEligible unpublished creatives:\n");
        for (const auto &c : candidates)
        {
            prompt += QStringLiteral(
                "- code=%1 | age=%2d | format=%3 | mode=%4 | sourceRef=%5 | product=\"%6\" | hookCat=%7 | hook=\"%8\"\n")
                .arg(c.shortCode)
                .arg(c.ageDays)
                .arg(c.format,
                     c.strategy.isEmpty() ? QStringLiteral("editorial") : c.strategy,
                     c.referenceUrl.isEmpty() ? QStringLiteral("none") : c.referenceUrl,
                     c.productSummary,
                     c.hookCategory,
                     c.hook.left(70));
        }
        prompt += QStringLiteral(
            "\nReply with STRICT JSON only (no markdown fences):\n"
            "{\"plan\": [{\"shortCode\": \"QS-...\", \"accountIndex\": 0, "
            "\"delayMinutes\": 0, \"addVideoText\": true, "
            "\"videoTextStyle\": \"classic_black_pill\", "
            "\"videoOverlayText\": \"Short 4-8 word curiosity hook\", \"reason\": \"...\"}]}");

        _appendLog(tr("[AI-PLANNER] Asking %1 to choose what to publish, in what order, spacing, and whether to add TikTok video text...")
            .arg(config.plannerCli->getName()));

        CliRunResult cliRes;
        cliRes = co_await config.plannerCli->runPrompt(prompt, QDir::tempPath());
        if (m_cancelFlag && *m_cancelFlag)
        {
            m_jobActive = false;
            _updateActionButtons();
            co_return;
        }
        const QJsonObject obj = parseJsonObjFromText(cliRes.output);
        plannedJsonItems = obj.value(QStringLiteral("plan")).toArray();
    }

    QHash<QString, EligibleCreative> byCode;
    for (const auto &c : candidates)
    {
        byCode.insert(c.shortCode.toUpper(), c);
    }

    QSet<QString> chosenCodes;
    QDateTime cursorTime = QDateTime::currentDateTime();
    int plannedVideoOrdinal = 0;

    for (const QJsonValue &v : plannedJsonItems)
    {
        if (m_queue.size() >= targetTotal)
        {
            break;
        }
        const QJsonObject o = v.toObject();
        QString code = AbstractSocialAccount::extractShortCode(o.value(QStringLiteral("shortCode")).toString());
        if (code.isEmpty())
        {
            code = o.value(QStringLiteral("shortCode")).toString().trimmed().toUpper();
        }
        if (!byCode.contains(code) || chosenCodes.contains(code))
        {
            continue;
        }
        chosenCodes.insert(code);

        const int accIdx = qBound(0, o.value(QStringLiteral("accountIndex")).toInt(0),
                                  int(config.targets.size()) - 1);
        const int delayMins = m_queue.isEmpty()
            ? 0
            : qMax(1, o.value(QStringLiteral("delayMinutes")).toInt(baseIntervalMins));
        cursorTime = cursorTime.addSecs(qint64(delayMins) * 60);

        ScheduledItem item;
        item.creative = byCode.value(code);
        item.target = config.targets[accIdx];
        item.delayMinutesFromPrevious = delayMins;
        item.scheduledAt = cursorTime;
        if (item.creative.format == QLatin1String("video"))
        {
            item.addVideoText = o.contains(QStringLiteral("addVideoText"))
                ? o.value(QStringLiteral("addVideoText")).toBool()
                : defaultVideoTextFlag(plannedVideoOrdinal);
            item.aiSuggestedVideoText = item.addVideoText;
            const QString aiStyle = o.value(QStringLiteral("videoTextStyle")).toString().trimmed();
            item.videoTextStyle = !aiStyle.isEmpty()
                ? normalizeVideoTextStyleId(aiStyle)
                : defaultVideoTextStyle(plannedVideoOrdinal);
            const QString aiOverlay = o.value(QStringLiteral("videoOverlayText")).toString().trimmed();
            item.videoOverlayText = !aiOverlay.isEmpty()
                ? cleanShortHookForOverlay(aiOverlay, item.creative.productSummary)
                : cleanShortHookForOverlay(item.creative.hook, item.creative.productSummary);
            ++plannedVideoOrdinal;
        }
        item.aiReason = o.value(QStringLiteral("reason")).toString().trimmed();
        if (item.aiReason.isEmpty())
        {
            item.aiReason = tr("Selected by AI planner");
        }
        item.status = QStringLiteral("Queued");
        m_queue.append(item);
    }

    // Deterministic diversity fallback for any remaining slots
    if (m_queue.size() < targetTotal)
    {
        QList<EligibleCreative> remaining;
        for (const auto &c : candidates)
        {
            if (!chosenCodes.contains(c.shortCode.toUpper()))
            {
                remaining.append(c);
            }
        }
        // Interleave products so consecutive posts feature different shoes/products when possible
        while (m_queue.size() < targetTotal && !remaining.isEmpty())
        {
            int pickIdx = 0;
            if (!m_queue.isEmpty())
            {
                const QString prevProd = m_queue.last().creative.productSummary;
                for (int i = 0; i < remaining.size(); ++i)
                {
                    if (remaining[i].productSummary != prevProd)
                    {
                        pickIdx = i;
                        break;
                    }
                }
            }
            const EligibleCreative picked = remaining.takeAt(pickIdx);
            const int slotNum = m_queue.size();
            int delayMins = 0;
            if (slotNum > 0)
            {
                const bool newDayBoundary = (slotNum % qMax(1, config.postsPerDay)) == 0;
                const int jitter = ((slotNum * 37) % 41) - 15; // deterministic natural jitter
                delayMins = newDayBoundary
                    ? 720 + qMax(0, jitter)
                    : qMax(15, baseIntervalMins + jitter);
            }
            cursorTime = cursorTime.addSecs(qint64(delayMins) * 60);

            ScheduledItem item;
            item.creative = picked;
            item.target = config.targets[slotNum % config.targets.size()];
            item.delayMinutesFromPrevious = delayMins;
            item.scheduledAt = cursorTime;
            if (picked.format == QLatin1String("video"))
            {
                item.addVideoText = defaultVideoTextFlag(plannedVideoOrdinal);
                item.aiSuggestedVideoText = item.addVideoText;
                item.videoTextStyle = defaultVideoTextStyle(plannedVideoOrdinal);
                item.videoOverlayText = cleanShortHookForOverlay(picked.hook, picked.productSummary);
                ++plannedVideoOrdinal;
            }
            item.aiReason = tr("Diversity rotation (%1)").arg(picked.format);
            item.status = QStringLiteral("Queued");
            m_queue.append(item);
        }
    }

    // Apply deterministic per-account timing according to config.spacingMode so the user's
    // selected radio button (12h daytime spread, even 24h spread, or fixed N minutes per account)
    // is honored strictly rather than left to LLM delay guesses.
    {
        const QDateTime batchStart = QDateTime::currentDateTime();
        QHash<QString, int> accPostCount;
        QHash<QString, QDateTime> accLastTime;
        QHash<QString, QDateTime> accDayWindowStart;
        QDateTime latestFirstPost = batchStart;
        bool anyFirstAssigned = false;

        for (int i = 0; i < m_queue.size(); ++i)
        {
            ScheduledItem &it = m_queue[i];
            const QString accKey = QStringLiteral("%1|%2")
                .arg(it.target.platformId, it.target.accountUrl.trimmed().toLower());
            const int priorOnAcc = accPostCount.value(accKey, 0);

            QDateTime slotTime;
            if (priorOnAcc == 0)
            {
                if (!anyFirstAssigned)
                {
                    slotTime = batchStart;
                    latestFirstPost = slotTime;
                    anyFirstAssigned = true;
                }
                else
                {
                    // Stagger initial post of a second account by 2 min so browser sessions don't collide
                    slotTime = latestFirstPost.addSecs(120);
                    latestFirstPost = slotTime;
                }
                accDayWindowStart.insert(accKey, slotTime);
            }
            else
            {
                const QDateTime prevAccTime = accLastTime.value(accKey, batchStart);
                const bool newDayForAcc = (priorOnAcc % perDayCap) == 0;
                int waitMins = baseIntervalMins;

                if (config.spacingMode == SpacingMode::EvenSpread24h)
                {
                    waitMins = baseIntervalMins;
                    if (newDayForAcc)
                    {
                        accDayWindowStart.insert(accKey, prevAccTime.addSecs(qint64(waitMins) * 60));
                    }
                }
                else if (config.spacingMode == SpacingMode::FixedInterval)
                {
                    if (newDayForAcc)
                    {
                        const QDateTime dayStart = accDayWindowStart.value(accKey, batchStart);
                        const qint64 elapsedMins = qMax<qint64>(0, dayStart.secsTo(prevAccTime) / 60);
                        waitMins = qMax(baseIntervalMins, int(1440 - elapsedMins));
                        accDayWindowStart.insert(accKey, prevAccTime.addSecs(qint64(waitMins) * 60));
                    }
                    else
                    {
                        waitMins = baseIntervalMins;
                    }
                }
                else
                {
                    // DaytimeSpread12h: spread across 12h with subtle natural jitter + ~12h overnight pause
                    const int maxJitter = qMin(15, qMax(2, baseIntervalMins / 5));
                    const int rawJitter = (((i * 37) % (2 * maxJitter + 1)) - maxJitter);
                    waitMins = newDayForAcc
                        ? (720 + qMax(0, rawJitter))
                        : qMax(15, baseIntervalMins + rawJitter);
                    if (newDayForAcc)
                    {
                        accDayWindowStart.insert(accKey, prevAccTime.addSecs(qint64(waitMins) * 60));
                    }
                }
                slotTime = prevAccTime.addSecs(qint64(waitMins) * 60);
            }

            it.scheduledAt = slotTime;
            accPostCount.insert(accKey, priorOnAcc + 1);
            accLastTime.insert(accKey, slotTime);
        }

        std::stable_sort(m_queue.begin(), m_queue.end(),
                         [](const ScheduledItem &a, const ScheduledItem &b) {
                             return a.scheduledAt < b.scheduledAt;
                         });
        for (int i = 0; i < m_queue.size(); ++i)
        {
            if (i == 0)
            {
                m_queue[i].delayMinutesFromPrevious = 0;
            }
            else
            {
                const qint64 secs = m_queue[i - 1].scheduledAt.secsTo(m_queue[i].scheduledAt);
                m_queue[i].delayMinutesFromPrevious = qMax(1, int((secs + 30) / 60));
            }
        }
    }

    _appendLog(tr("[AI-PLANNER] Planned %1 publish slot(s):").arg(m_queue.size()));
    for (int i = 0; i < m_queue.size(); ++i)
    {
        const auto &it = m_queue[i];
        const QString textNote = (it.creative.format == QLatin1String("video"))
            ? (it.addVideoText
                   ? tr(" [TikTok Text (%1): \"%2\"]")
                         .arg(videoTextStyleShortTag(it.videoTextStyle), it.videoOverlayText)
                   : tr(" [Clean Video: no text]"))
            : QString{};
        _appendLog(tr("  #%1 %2 (%3, %4d old)%9 → %5 at %6 (+%7m) — %8")
            .arg(i + 1)
            .arg(it.creative.shortCode, it.creative.format)
            .arg(it.creative.ageDays)
            .arg(it.target.accountUrl,
                 it.scheduledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
            .arg(it.delayMinutesFromPrevious)
            .arg(it.aiReason, textNote));
    }

    _refreshQueueTable();

    if (config.reviewBeforePublish)
    {
        m_statusLabel->setText(tr("Reviewing AI selection & publish order (%1 of %2 candidate(s))...")
            .arg(m_queue.size()).arg(candidates.size()));
        const QPointer<PanePublishing> self(this);
        const bool confirmed = _reviewPlannedSelectionDialog(
            config, candidates, baseIntervalMins, videoTextExp.humanSummary());
        if (!self || (cancelToken && *cancelToken))
        {
            co_return;
        }
        if (!confirmed || m_queue.isEmpty())
        {
            m_queue.clear();
            m_jobActive = false;
            m_statusLabel->setText(tr("Auto-Publish cancelled during selection review."));
            _appendLog(tr("[PLANNER] Auto-Publish job cancelled during selection & order review."));
            _refreshQueueTable();
            _updateActionButtons();
            co_return;
        }
        _appendLog(tr("[USER-PLAN] Confirmed %1 creative(s) to publish in final order:").arg(m_queue.size()));
        for (int i = 0; i < m_queue.size(); ++i)
        {
            const auto &it = m_queue[i];
            const QString textNote = (it.creative.format == QLatin1String("video"))
                ? (it.addVideoText
                       ? tr(" [TikTok Text: \"%1\"]").arg(it.videoOverlayText)
                       : tr(" [Clean Video: no text]"))
                : QString{};
            _appendLog(tr("  #%1 %2 (%3, %4d old)%8 → %5 at %6 (+%7m)")
                .arg(i + 1)
                .arg(it.creative.shortCode, it.creative.format)
                .arg(it.creative.ageDays)
                .arg(it.target.accountUrl,
                     it.scheduledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                .arg(it.delayMinutesFromPrevious)
                .arg(textNote));
        }
        _refreshQueueTable();
    }

    _updateActionButtons();
    _scheduleNextTimer();
}

bool PanePublishing::_reviewPlannedSelectionDialog(const PublishJobConfig &config,
                                                   const QList<EligibleCreative> &allCandidates,
                                                   int baseIntervalMins,
                                                   const QString &experimentSummaryLine)
{
    if (allCandidates.isEmpty() || config.targets.isEmpty())
    {
        return false;
    }

    struct CandidateRowState
    {
        EligibleCreative creative;
        bool checked = false;
        int aiInitialRank = 0; // 1..K if AI selected, 0 if not initially picked
        bool addVideoText = false;
        bool aiSuggestedVideoText = false;
        QString videoOverlayText;
        QString videoTextStyle;
        QString aiReason;
        int targetIndex = 0;
        int customDelayMinutes = 180; // Delay used whenever this item is NOT #1
        QPixmap rowThumb;
    };

    // Ensure every candidate has a resolved preview image (extracting a video frame once via ffmpeg if needed)
    QList<EligibleCreative> enrichedCandidates = allCandidates;
    QHash<QString, int> candIdxByCode;
    for (int i = 0; i < enrichedCandidates.size(); ++i)
    {
        auto &c = enrichedCandidates[i];
        if (c.previewImagePath.isEmpty() && m_projects && c.projectRow >= 0)
        {
            const QDir genDir = m_projects->generationDir(c.projectRow, c.shortCode, false);
            const QDir tempDir = m_projects->generationTempDir(c.projectRow, c.shortCode);
            c.previewImagePath = resolveCreativePreviewImagePath(
                genDir, tempDir, c.format, c.mediaPaths, true);
        }
        candIdxByCode.insert(c.shortCode.toUpper(), i);
    }

    auto makeBadgeInfo = [](const EligibleCreative &c, QString *outBadge, QColor *outColor) {
        if (c.format == QLatin1String("video"))
        {
            *outBadge = QStringLiteral("VIDEO");
            *outColor = QColor(37, 99, 235);
        }
        else if (c.format == QLatin1String("carousel"))
        {
            *outBadge = QStringLiteral("%1 SLIDES").arg(c.mediaPaths.size());
            *outColor = QColor(124, 58, 237);
        }
        else
        {
            *outBadge = QStringLiteral("IMAGE");
            *outColor = QColor(13, 148, 136);
        }
    };

    const auto makeRowThumb = [&](const CandidateRowState &r) -> QPixmap {
        QString badgeTxt;
        QColor badgeCol;
        makeBadgeInfo(r.creative, &badgeTxt, &badgeCol);
        const QString overlayPreview = (r.creative.format == QLatin1String("video") && r.addVideoText)
            ? r.videoOverlayText
            : QString{};
        return renderCreativeCardPixmap(
            r.creative.previewImagePath,
            r.creative.shortCode,
            r.creative.productSummary,
            badgeTxt,
            badgeCol,
            QSize(72, 112),
            overlayPreview,
            r.videoTextStyle);
    };

    QList<CandidateRowState> initialRows;
    QSet<QString> seenCodes;

    // 1. Place AI-selected items at the top in the AI's planned order
    for (int i = 0; i < m_queue.size(); ++i)
    {
        const ScheduledItem &scheduled = m_queue[i];
        const QString codeUpper = scheduled.creative.shortCode.toUpper();
        if (seenCodes.contains(codeUpper))
        {
            continue;
        }
        seenCodes.insert(codeUpper);

        CandidateRowState row;
        row.creative = candIdxByCode.contains(codeUpper)
            ? enrichedCandidates[candIdxByCode.value(codeUpper)]
            : scheduled.creative;
        row.checked = true;
        row.aiInitialRank = i + 1;
        row.addVideoText = scheduled.addVideoText;
        row.aiSuggestedVideoText = scheduled.aiSuggestedVideoText;
        row.videoTextStyle = normalizeVideoTextStyleId(scheduled.videoTextStyle);
        row.videoOverlayText = !scheduled.videoOverlayText.isEmpty()
            ? scheduled.videoOverlayText
            : cleanShortHookForOverlay(row.creative.hook, row.creative.productSummary);
        row.aiReason = scheduled.aiReason;
        row.targetIndex = 0;
        for (int t = 0; t < config.targets.size(); ++t)
        {
            if (config.targets[t].accountUrl == scheduled.target.accountUrl
                && config.targets[t].platformId == scheduled.target.platformId)
            {
                row.targetIndex = t;
                break;
            }
        }
        row.customDelayMinutes = scheduled.delayMinutesFromPrevious > 0
            ? scheduled.delayMinutesFromPrevious
            : qMax(5, baseIntervalMins);
        row.rowThumb = makeRowThumb(row);
        initialRows.append(row);
    }

    // 2. Append all remaining eligible candidates below so the user can inspect, swap, or add any of them
    int extraIdx = 0;
    for (const EligibleCreative &c : enrichedCandidates)
    {
        const QString codeUpper = c.shortCode.toUpper();
        if (seenCodes.contains(codeUpper))
        {
            continue;
        }
        seenCodes.insert(codeUpper);

        CandidateRowState row;
        row.creative = c;
        row.checked = false;
        row.aiInitialRank = 0;
        if (c.format == QLatin1String("video"))
        {
            row.addVideoText = ((extraIdx % 2) == 0);
            row.aiSuggestedVideoText = row.addVideoText;
            const auto &styles = curatedVideoTextStyleIds();
            row.videoTextStyle = styles[extraIdx % styles.size()];
            row.videoOverlayText = cleanShortHookForOverlay(c.hook, c.productSummary);
        }
        row.aiReason = tr("Not picked by AI planner (available candidate)");
        row.targetIndex = (m_queue.size() + extraIdx) % config.targets.size();
        if (config.spacingMode == SpacingMode::DaytimeSpread12h)
        {
            const int maxJitter = qMin(15, qMax(2, baseIntervalMins / 5));
            const int jitter = (((m_queue.size() + extraIdx + 1) * 37) % (2 * maxJitter + 1)) - maxJitter;
            row.customDelayMinutes = qMax(15, baseIntervalMins + jitter);
        }
        else
        {
            row.customDelayMinutes = qMax(5, baseIntervalMins);
        }
        ++extraIdx;
        row.rowThumb = makeRowThumb(row);
        initialRows.append(row);
    }

    QList<CandidateRowState> rows = initialRows;

    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("dialogReviewPublishPlan"));
    dlg.setWindowTitle(tr("Review & Customize Publish Selection and Order"));
    dlg.setMaximumWidth(1600);
    dlg.resize(1140, 750);

    auto *mainLayout = new QVBoxLayout(&dlg);

    QString bannerHtml = tr(
        "The AI planner selected <b>%1</b> of <b>%2</b> eligible creative(s) in recommended publish order. "
        "Check/uncheck candidates, reorder with <b>Move Up / Move Down / Move to #1</b>, and customize "
        "each video's optional native TikTok text overlay (<i>Aa Texte</i>).")
            .arg(m_queue.size())
            .arg(rows.size());
    if (!experimentSummaryLine.trimmed().isEmpty())
    {
        bannerHtml += QStringLiteral("<br><span style='color:#93c5fd; font-size:11px;'>📊 %1</span>")
            .arg(experimentSummaryLine.toHtmlEscaped());
    }
    auto *banner = new QLabel(bannerHtml);
    banner->setObjectName(QStringLiteral("labelReviewBanner"));
    banner->setWordWrap(true);
    mainLayout->addWidget(banner);

    auto *splitter = new QSplitter(Qt::Horizontal);
    mainLayout->addWidget(splitter, 1);

    // Left panel: Candidate table + ordering toolbar
    auto *leftBox = new QWidget;
    auto *leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setContentsMargins(0, 0, 4, 0);

    auto *table = new QTableWidget(0, 4);
    table->setObjectName(QStringLiteral("tableReviewCandidates"));
    table->setHorizontalHeaderLabels({
        tr("Publish / Order"),
        tr("Preview"),
        tr("Creative Details (Product, Hook & AI Note)"),
        tr("Target & Timing")
    });
    table->setIconSize(QSize(72, 112));
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(true);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(122);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table->setColumnWidth(1, 86);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    table->horizontalHeader()->resizeSection(3, 195);
    leftLayout->addWidget(table, 1);

    auto *orderBar = new QHBoxLayout;
    auto *btnToggleCheck = new QPushButton(tr("Check / Uncheck"));
    btnToggleCheck->setObjectName(QStringLiteral("buttonToggleCandidateCheck"));
    auto *btnMoveUp = new QPushButton(tr("▲ Move Up"));
    btnMoveUp->setObjectName(QStringLiteral("buttonMoveCandidateUp"));
    auto *btnMoveDown = new QPushButton(tr("▼ Move Down"));
    btnMoveDown->setObjectName(QStringLiteral("buttonMoveCandidateDown"));
    auto *btnMoveTop = new QPushButton(tr("⤒ Move to #1"));
    btnMoveTop->setObjectName(QStringLiteral("buttonMoveCandidateTop"));
    auto *btnGroupTop = new QPushButton(tr("Group Selected at Top"));
    btnGroupTop->setObjectName(QStringLiteral("buttonSortCheckedTop"));
    auto *btnResetAi = new QPushButton(tr("↺ Reset to AI Plan"));
    btnResetAi->setObjectName(QStringLiteral("buttonResetAiSelection"));

    orderBar->addWidget(btnToggleCheck);
    orderBar->addWidget(btnMoveUp);
    orderBar->addWidget(btnMoveDown);
    orderBar->addWidget(btnMoveTop);
    orderBar->addWidget(btnGroupTop);
    orderBar->addWidget(btnResetAi);
    orderBar->addStretch();

    auto *summaryLbl = new QLabel;
    summaryLbl->setObjectName(QStringLiteral("labelSelectedPublishSummary"));
    orderBar->addWidget(summaryLbl);
    leftLayout->addLayout(orderBar);
    splitter->addWidget(leftBox);

    // Right panel: Large visual preview + carousel slide switcher + full creative details & slot controls
    auto *rightBox = new QWidget;
    rightBox->setMinimumWidth(310);
    auto *rightLayout = new QVBoxLayout(rightBox);
    rightLayout->setContentsMargins(6, 0, 0, 0);

    auto *previewTitle = new QLabel(tr("Selected Candidate Preview & Details:"));
    previewTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    rightLayout->addWidget(previewTitle);

    auto *previewStack = new QStackedWidget;
    previewStack->setObjectName(QStringLiteral("stackedReviewPreview"));
    previewStack->setFixedSize(236, 360);

    auto *largePreviewLbl = new QLabel;
    largePreviewLbl->setObjectName(QStringLiteral("labelReviewLargePreview"));
    largePreviewLbl->setAlignment(Qt::AlignCenter);
    largePreviewLbl->setMinimumSize(236, 360);
    largePreviewLbl->setStyleSheet(QStringLiteral(
        "QLabel { background: #141821; border: 1px solid #2b3245; border-radius: 10px; padding: 4px; }"));
    previewStack->addWidget(largePreviewLbl);

    auto *videoWidget = new QVideoWidget;
    videoWidget->setObjectName(QStringLiteral("videoReviewPlayerWidget"));
    videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    videoWidget->setStyleSheet(QStringLiteral("background-color: #0d1117; border-radius: 10px;"));
    previewStack->addWidget(videoWidget);

    rightLayout->addWidget(previewStack, 0, Qt::AlignHCenter);

    auto *audioOutput = new QAudioOutput(&dlg);
    audioOutput->setVolume(0.8f);
    auto *mediaPlayer = new QMediaPlayer(&dlg);
    mediaPlayer->setObjectName(QStringLiteral("playerReviewVideo"));
    mediaPlayer->setAudioOutput(audioOutput);
    mediaPlayer->setVideoOutput(videoWidget);
    mediaPlayer->setLoops(QMediaPlayer::Infinite);

    // Inline video playback controls (visible whenever a video candidate is selected)
    auto *videoTransportWidget = new QWidget;
    videoTransportWidget->setObjectName(QStringLiteral("widgetVideoTransportBar"));
    auto *videoTransportLayout = new QVBoxLayout(videoTransportWidget);
    videoTransportLayout->setContentsMargins(0, 2, 0, 2);
    videoTransportLayout->setSpacing(4);

    auto *videoBtnRow = new QHBoxLayout;
    auto *btnPlayPauseVideo = new QPushButton(tr("▶ Play Video"));
    btnPlayPauseVideo->setObjectName(QStringLiteral("buttonPlayPauseCandidateVideo"));
    btnPlayPauseVideo->setStyleSheet(QStringLiteral("QPushButton { font-weight: 600; padding: 4px 10px; }"));
    auto *videoTimeLbl = new QLabel(QStringLiteral("0:00 / 0:00"));
    videoTimeLbl->setObjectName(QStringLiteral("labelCandidateVideoTime"));
    videoTimeLbl->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    auto *btnMuteVideo = new QPushButton(tr("🔊"));
    btnMuteVideo->setObjectName(QStringLiteral("buttonMuteCandidateVideo"));
    btnMuteVideo->setCheckable(true);
    btnMuteVideo->setFixedWidth(34);
    btnMuteVideo->setToolTip(tr("Mute / unmute preview audio"));

    videoBtnRow->addWidget(btnPlayPauseVideo, 1);
    videoBtnRow->addWidget(videoTimeLbl);
    videoBtnRow->addWidget(btnMuteVideo);
    videoTransportLayout->addLayout(videoBtnRow);

    auto *videoPosSlider = new QSlider(Qt::Horizontal);
    videoPosSlider->setObjectName(QStringLiteral("sliderCandidateVideoPosition"));
    videoPosSlider->setRange(0, 0);
    videoTransportLayout->addWidget(videoPosSlider);
    videoTransportWidget->setVisible(false);
    rightLayout->addWidget(videoTransportWidget);

    auto *slideStripWidget = new QWidget;
    slideStripWidget->setObjectName(QStringLiteral("widgetReviewSlideStrip"));
    auto *slideStripLayout = new QHBoxLayout(slideStripWidget);
    slideStripLayout->setContentsMargins(0, 2, 0, 2);
    slideStripLayout->setSpacing(4);
    slideStripWidget->setVisible(false);
    rightLayout->addWidget(slideStripWidget);

    auto *btnOpenMedia = new QPushButton(tr("Open Media File Externally"));
    btnOpenMedia->setObjectName(QStringLiteral("buttonOpenCandidateMedia"));
    rightLayout->addWidget(btnOpenMedia);

    auto *slotForm = new QFormLayout;
    auto *targetCombo = new QComboBox;
    targetCombo->setObjectName(QStringLiteral("comboReviewCandidateTarget"));
    for (const AccountTarget &t : config.targets)
    {
        targetCombo->addItem(QStringLiteral("%1 — %2").arg(t.platformName, t.accountUrl));
    }
    targetCombo->setEnabled(config.targets.size() > 1);
    slotForm->addRow(tr("Target account:"), targetCombo);

    auto *delaySpin = new QSpinBox;
    delaySpin->setObjectName(QStringLiteral("spinReviewCandidateDelay"));
    delaySpin->setRange(0, 4320);
    delaySpin->setSuffix(tr(" min after prev"));
    slotForm->addRow(tr("Spacing delay:"), delaySpin);
    rightLayout->addLayout(slotForm);

    // Optional native TikTok video text track ('Aa Texte') controls — AI suggests ON/OFF + editable hook!
    auto *videoTextBox = new QWidget;
    videoTextBox->setObjectName(QStringLiteral("widgetReviewVideoTextControls"));
    auto *videoTextLayout = new QVBoxLayout(videoTextBox);
    videoTextLayout->setContentsMargins(0, 2, 0, 4);
    videoTextLayout->setSpacing(4);

    auto *chkVideoText = new QCheckBox(tr("Add native TikTok text over video (\"Aa Texte\")"));
    chkVideoText->setObjectName(QStringLiteral("checkReviewVideoTextOverlay"));
    chkVideoText->setToolTip(tr("Uses TikTok Studio's native 'Aa Texte' track in the upper safe zone so TikTok indexes it for SEO while keeping the shoes unobstructed."));
    videoTextLayout->addWidget(chkVideoText);

    auto *comboVideoStyle = new QComboBox;
    comboVideoStyle->setObjectName(QStringLiteral("comboReviewVideoTextStyle"));
    for (const QString &sid : curatedVideoTextStyleIds())
    {
        comboVideoStyle->addItem(videoTextStyleDisplayName(sid), sid);
    }
    comboVideoStyle->setToolTip(tr("Choose from 4 fashion-safe native TikTok Studio text styles (AI selects & A/B-tests performance over time)."));
    videoTextLayout->addWidget(comboVideoStyle);

    auto *editVideoText = new QLineEdit;
    editVideoText->setObjectName(QStringLiteral("editReviewVideoOverlayText"));
    editVideoText->setMaxLength(80);
    editVideoText->setPlaceholderText(tr("Short 4–9 word hook overlaid near top of video..."));
    videoTextLayout->addWidget(editVideoText);
    videoTextBox->setVisible(false);
    rightLayout->addWidget(videoTextBox);

    auto *detailsBox = new QPlainTextEdit;
    detailsBox->setObjectName(QStringLiteral("textReviewCandidateDetails"));
    detailsBox->setReadOnly(true);
    detailsBox->setMinimumHeight(130);
    detailsBox->setStyleSheet(QStringLiteral("QPlainTextEdit { font-size: 12px; }"));
    rightLayout->addWidget(detailsBox, 1);

    splitter->addWidget(rightBox);
    splitter->setSizes({760, 340});

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto *okBtn = buttons->button(QDialogButtonBox::Ok);
    if (okBtn)
    {
        okBtn->setObjectName(QStringLiteral("buttonConfirmPublishPlan"));
    }
    mainLayout->addWidget(buttons);

    bool updatingUi = false;
    bool scrubbingSlider = false;
    bool autoPlayVideosOnSelect = false;
    QString loadedVideoPath;

    const auto formatMsMmSs = [](qint64 ms) -> QString {
        const qint64 totalSecs = qMax<qint64>(0, ms / 1000);
        return QStringLiteral("%1:%2")
            .arg(totalSecs / 60)
            .arg(totalSecs % 60, 2, 10, QLatin1Char('0'));
    };

    const auto stopInlineVideo = [&]() {
        mediaPlayer->stop();
        mediaPlayer->setSource(QUrl{});
        loadedVideoPath.clear();
        previewStack->setCurrentWidget(largePreviewLbl);
        btnPlayPauseVideo->setText(tr("▶ Play Video"));
        videoPosSlider->setRange(0, 0);
        videoTimeLbl->setText(QStringLiteral("0:00 / 0:00"));
    };

    const auto startInlineVideoForRow = [&](int row) {
        if (row < 0 || row >= rows.size())
        {
            return;
        }
        const EligibleCreative &c = rows[row].creative;
        if (c.format != QLatin1String("video") || c.mediaPaths.isEmpty())
        {
            return;
        }
        const QString vPath = c.mediaPaths.first();
        if (loadedVideoPath != vPath)
        {
            loadedVideoPath = vPath;
            mediaPlayer->setSource(QUrl::fromLocalFile(vPath));
        }
        previewStack->setCurrentWidget(videoWidget);
        mediaPlayer->play();
        autoPlayVideosOnSelect = true;
    };

    connect(mediaPlayer, &QMediaPlayer::playbackStateChanged, &dlg,
            [&](QMediaPlayer::PlaybackState state) {
        btnPlayPauseVideo->setText(state == QMediaPlayer::PlayingState
            ? tr("⏸ Pause")
            : tr("▶ Play Video"));
    });

    connect(mediaPlayer, &QMediaPlayer::durationChanged, &dlg, [&](qint64 dur) {
        videoPosSlider->setRange(0, int(qMax<qint64>(0, dur)));
        videoTimeLbl->setText(QStringLiteral("%1 / %2")
            .arg(formatMsMmSs(mediaPlayer->position()), formatMsMmSs(dur)));
    });

    connect(mediaPlayer, &QMediaPlayer::positionChanged, &dlg, [&](qint64 pos) {
        if (!scrubbingSlider)
        {
            videoPosSlider->setValue(int(qMax<qint64>(0, pos)));
        }
        videoTimeLbl->setText(QStringLiteral("%1 / %2")
            .arg(formatMsMmSs(pos), formatMsMmSs(mediaPlayer->duration())));
    });

    connect(videoPosSlider, &QSlider::sliderPressed, &dlg, [&]() { scrubbingSlider = true; });
    connect(videoPosSlider, &QSlider::sliderReleased, &dlg, [&]() {
        scrubbingSlider = false;
        mediaPlayer->setPosition(videoPosSlider->value());
    });
    connect(videoPosSlider, &QSlider::sliderMoved, &dlg, [&](int val) {
        mediaPlayer->setPosition(val);
    });

    connect(btnPlayPauseVideo, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r < 0 || r >= rows.size() || rows[r].creative.format != QLatin1String("video"))
        {
            return;
        }
        if (mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
        {
            mediaPlayer->pause();
            autoPlayVideosOnSelect = false;
        }
        else
        {
            startInlineVideoForRow(r);
        }
    });

    connect(btnMuteVideo, &QPushButton::toggled, &dlg, [&](bool muted) {
        audioOutput->setMuted(muted);
        btnMuteVideo->setText(muted ? tr("🔇") : tr("🔊"));
    });

    const auto checkedOrderOfRow = [&](int targetRow) -> int {
        if (targetRow < 0 || targetRow >= rows.size() || !rows[targetRow].checked)
        {
            return 0;
        }
        int ord = 0;
        for (int i = 0; i <= targetRow; ++i)
        {
            if (rows[i].checked)
            {
                ++ord;
            }
        }
        return ord;
    };

    const auto refreshInspector = [&](int row) {
        if (row < 0 || row >= rows.size())
        {
            stopInlineVideo();
            largePreviewLbl->setPixmap(QPixmap{});
            videoTransportWidget->setVisible(false);
            videoTextBox->setVisible(false);
            slideStripWidget->setVisible(false);
            detailsBox->clear();
            btnOpenMedia->setEnabled(false);
            return;
        }

        const bool prevGuard = updatingUi;
        updatingUi = true;

        const CandidateRowState &st = rows[row];
        const EligibleCreative &c = st.creative;

        QString badgeTxt;
        QColor badgeCol;
        makeBadgeInfo(c, &badgeTxt, &badgeCol);
        const QString liveOverlayPreview = (c.format == QLatin1String("video") && st.addVideoText)
            ? st.videoOverlayText
            : QString{};
        largePreviewLbl->setPixmap(renderCreativeCardPixmap(
            c.previewImagePath,
            c.shortCode,
            c.productSummary,
            badgeTxt,
            badgeCol,
            QSize(228, 352),
            liveOverlayPreview,
            st.videoTextStyle));

        if (c.format == QLatin1String("video") && !c.mediaPaths.isEmpty())
        {
            videoTransportWidget->setVisible(true);
            videoTextBox->setVisible(true);
            const QString aiRecBadge = st.aiSuggestedVideoText
                ? tr(" (AI suggested: ON)")
                : tr(" (AI suggested: OFF)");
            chkVideoText->setText(tr("Add TikTok video text") + aiRecBadge);
            chkVideoText->setChecked(st.addVideoText);
            const int styleIdx = comboVideoStyle->findData(normalizeVideoTextStyleId(st.videoTextStyle));
            if (styleIdx >= 0)
            {
                comboVideoStyle->setCurrentIndex(styleIdx);
            }
            comboVideoStyle->setEnabled(st.addVideoText);
            if (editVideoText->text() != st.videoOverlayText)
            {
                editVideoText->setText(st.videoOverlayText);
            }
            editVideoText->setEnabled(st.addVideoText);
            if (loadedVideoPath != c.mediaPaths.first())
            {
                stopInlineVideo();
                if (autoPlayVideosOnSelect)
                {
                    startInlineVideoForRow(row);
                }
            }
        }
        else
        {
            stopInlineVideo();
            videoTransportWidget->setVisible(false);
            videoTextBox->setVisible(false);
        }

        // Rebuild slide strip for multi-image carousels so every slide can be inspected
        while (QLayoutItem *child = slideStripLayout->takeAt(0))
        {
            delete child->widget();
            delete child;
        }
        if (c.format == QLatin1String("carousel") && c.mediaPaths.size() >= 2)
        {
            slideStripLayout->addStretch();
            const int maxSlidesShown = qMin<int>(6, c.mediaPaths.size());
            for (int s = 0; s < maxSlidesShown; ++s)
            {
                const QString slidePath = c.mediaPaths[s];
                auto *sBtn = new QPushButton(tr("#%1").arg(s + 1), slideStripWidget);
                sBtn->setIcon(QIcon(renderCreativeCardPixmap(
                    slidePath, c.shortCode, QString{}, QString{}, badgeCol, QSize(32, 50))));
                sBtn->setIconSize(QSize(28, 44));
                sBtn->setToolTip(tr("Preview Slide %1 (%2)").arg(s + 1).arg(QFileInfo{slidePath}.fileName()));
                connect(sBtn, &QPushButton::clicked, &dlg, [&, slidePath, s, total = c.mediaPaths.size(), code = c.shortCode, prod = c.productSummary, badgeCol]() {
                    previewStack->setCurrentWidget(largePreviewLbl);
                    largePreviewLbl->setPixmap(renderCreativeCardPixmap(
                        slidePath,
                        code,
                        prod,
                        QStringLiteral("SLIDE %1/%2").arg(s + 1).arg(total),
                        badgeCol,
                        QSize(228, 352)));
                });
                slideStripLayout->addWidget(sBtn);
            }
            slideStripLayout->addStretch();
            slideStripWidget->setVisible(true);
        }
        else
        {
            slideStripWidget->setVisible(false);
        }

        btnOpenMedia->setEnabled(!c.mediaPaths.isEmpty());
        btnOpenMedia->setText(c.format == QLatin1String("video")
            ? tr("Open Video Externally (%1)").arg(QFileInfo{c.mediaPaths.first()}.fileName())
            : tr("Open Slide Folder (%1 files)").arg(c.mediaPaths.size()));

        targetCombo->setCurrentIndex(qBound(0, st.targetIndex, int(config.targets.size()) - 1));
        const int ord = checkedOrderOfRow(row);
        if (ord == 1)
        {
            delaySpin->setValue(0);
            delaySpin->setEnabled(false);
            delaySpin->setToolTip(tr("Slot #1 always publishes immediately (+0 min)."));
        }
        else
        {
            delaySpin->setValue(st.customDelayMinutes);
            delaySpin->setEnabled(true);
            delaySpin->setToolTip(tr("Minutes to wait after the previous scheduled post."));
        }

        QStringList fileNames;
        for (const QString &mp : c.mediaPaths)
        {
            fileNames << QFileInfo{mp}.fileName();
        }
        const QString aiLine = st.aiInitialRank > 0
            ? tr("AI Pick #%1 — %2").arg(st.aiInitialRank).arg(st.aiReason)
            : st.aiReason;
        const QString hookLine = !c.hook.trimmed().isEmpty()
            ? c.hook.trimmed()
            : tr("(Hook + #hashtags will be auto-generated before upload)");
        const QString capLine = !c.caption.trimmed().isEmpty()
            ? c.caption.trimmed()
            : tr("(Rich TikTok caption + #hashtags + tracking tag %1 will be auto-filled)")
                  .arg(codeTagFromShortCodePub(c.shortCode));

        detailsBox->setPlainText(
            tr("Code: %1  |  Format: %2 (%3)  |  Age: %4 day(s) old\n"
               "Product: %5\n"
               "Project: %6\n"
               "AI Planner: %7\n"
               "Hook: %8\n\n"
               "Caption / Hashtags:\n%9")
                .arg(c.shortCode,
                     c.format.toUpper(),
                     fileNames.join(QStringLiteral(", ")))
                .arg(c.ageDays)
                .arg(c.productSummary,
                     c.projectName,
                     aiLine,
                     hookLine,
                     capLine));

        updatingUi = prevGuard;
    };

    const auto rebuildTable = [&](int selectRow) {
        updatingUi = true;
        table->setRowCount(rows.size());

        int checkedCount = 0;
        QDateTime cursorTime = QDateTime::currentDateTime();

        for (int r = 0; r < rows.size(); ++r)
        {
            const CandidateRowState &st = rows[r];
            const EligibleCreative &c = st.creative;

            int ord = 0;
            int effectiveDelay = 0;
            QDateTime rowScheduledAt;
            if (st.checked)
            {
                ++checkedCount;
                ord = checkedCount;
                effectiveDelay = (ord == 1) ? 0 : qMax(1, st.customDelayMinutes);
                cursorTime = cursorTime.addSecs(qint64(effectiveDelay) * 60);
                rowScheduledAt = cursorTime;
            }

            // Col 0: Checkbox + Publish Order (#1, #2, ... or —)
            const QString orderText = st.checked
                ? tr("  Publish #%1").arg(ord)
                : tr("  Skip");
            auto *chkItem = new QTableWidgetItem(orderText);
            chkItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            chkItem->setCheckState(st.checked ? Qt::Checked : Qt::Unchecked);
            if (st.checked)
            {
                QFont f = chkItem->font();
                f.setBold(true);
                chkItem->setFont(f);
                chkItem->setForeground(QColor(QStringLiteral("#8fd19e")));
            }
            else
            {
                chkItem->setForeground(QColor(QStringLiteral("#9ca3af")));
            }
            table->setItem(r, 0, chkItem);

            // Col 1: Visual Preview Thumbnail
            auto *thumbItem = new QTableWidgetItem;
            thumbItem->setData(Qt::DecorationRole, st.rowThumb);
            thumbItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(r, 1, thumbItem);

            // Col 2: Multi-line Creative Details
            const QString fmtDesc = c.format == QLatin1String("carousel")
                ? tr("SLIDESHOW (%1 slides)").arg(c.mediaPaths.size())
                : c.format.toUpper();
            const QString hookSnippet = !c.hook.trimmed().isEmpty()
                ? QStringLiteral("\"%1\"").arg(c.hook.trimmed().left(85))
                : (!c.caption.trimmed().isEmpty()
                       ? QStringLiteral("\"%1\"").arg(c.caption.trimmed().left(85))
                       : tr("(Auto-caption + #hashtags on publish)"));
            const QString aiTag = st.aiInitialRank > 0
                ? tr("★ AI Pick #%1: %2").arg(st.aiInitialRank).arg(st.aiReason)
                : st.aiReason;
            QString videoTextRowNote;
            if (c.format == QLatin1String("video"))
            {
                videoTextRowNote = st.addVideoText
                    ? tr("\n📝 TikTok Text [%1]: \"%2\"")
                          .arg(videoTextStyleShortTag(st.videoTextStyle), st.videoOverlayText.left(60))
                    : tr("\n🎬 Clean Video (no TikTok text overlay)");
            }
            const QString detailsText = QStringLiteral("%1 · %2 · %3d old\n%4\nHook: %5%7\n%6")
                .arg(c.shortCode, fmtDesc)
                .arg(c.ageDays)
                .arg(c.productSummary, hookSnippet, aiTag, videoTextRowNote);

            auto *detailsItem = new QTableWidgetItem(detailsText);
            if (!st.checked)
            {
                detailsItem->setForeground(QColor(QStringLiteral("#9ca3af")));
            }
            table->setItem(r, 2, detailsItem);

            // Col 3: Target Account & Scheduled Timing
            const AccountTarget &tgt = config.targets[qBound(0, st.targetIndex, int(config.targets.size()) - 1)];
            QString timingText;
            if (st.checked)
            {
                const QString delayStr = (ord == 1)
                    ? tr("Immediate (+0m)")
                    : tr("+%1m (%2)").arg(effectiveDelay).arg(rowScheduledAt.toString(QStringLiteral("MM-dd HH:mm")));
                timingText = QStringLiteral("%1\n%2\n%3")
                    .arg(tgt.platformName, tgt.accountUrl, delayStr);
            }
            else
            {
                timingText = tr("%1\n(Not scheduled)").arg(tgt.platformName);
            }
            auto *timingItem = new QTableWidgetItem(timingText);
            if (!st.checked)
            {
                timingItem->setForeground(QColor(QStringLiteral("#9ca3af")));
            }
            table->setItem(r, 3, timingItem);
            table->setRowHeight(r, 122);
        }

        summaryLbl->setText(tr("Selected to publish: %1 of %2 candidate(s)")
            .arg(checkedCount).arg(rows.size()));
        summaryLbl->setStyleSheet(checkedCount > 0
            ? QStringLiteral("color: #8fd19e; font-weight: 600;")
            : QStringLiteral("color: #e89774; font-weight: 600;"));

        if (okBtn)
        {
            okBtn->setEnabled(checkedCount > 0);
            okBtn->setText(checkedCount > 0
                ? tr("Start Publishing (%1)").arg(checkedCount)
                : tr("Select at least 1 creative"));
        }

        const int clampedSel = rows.isEmpty() ? -1 : qBound(0, selectRow, int(rows.size()) - 1);
        if (clampedSel >= 0)
        {
            table->selectRow(clampedSel);
        }
        btnMoveUp->setEnabled(clampedSel > 0);
        btnMoveDown->setEnabled(clampedSel >= 0 && clampedSel + 1 < rows.size());
        btnMoveTop->setEnabled(clampedSel > 0);
        btnToggleCheck->setEnabled(clampedSel >= 0);

        updatingUi = false;
        refreshInspector(clampedSel);
    };

    connect(table, &QTableWidget::itemChanged, &dlg, [&](QTableWidgetItem *changed) {
        if (updatingUi || !changed || changed->column() != 0)
        {
            return;
        }
        const int r = changed->row();
        if (r < 0 || r >= rows.size())
        {
            return;
        }
        rows[r].checked = (changed->checkState() == Qt::Checked);
        rebuildTable(r);
    });

    connect(table, &QTableWidget::currentCellChanged, &dlg,
            [&](int currentRow, int, int, int) {
        if (updatingUi)
        {
            return;
        }
        btnMoveUp->setEnabled(currentRow > 0);
        btnMoveDown->setEnabled(currentRow >= 0 && currentRow + 1 < rows.size());
        btnMoveTop->setEnabled(currentRow > 0);
        btnToggleCheck->setEnabled(currentRow >= 0);
        refreshInspector(currentRow);
    });

    connect(btnToggleCheck, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size())
        {
            rows[r].checked = !rows[r].checked;
            rebuildTable(r);
        }
    });

    connect(btnMoveUp, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r > 0 && r < rows.size())
        {
            rows.swapItemsAt(r, r - 1);
            rebuildTable(r - 1);
        }
    });

    connect(btnMoveDown, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r >= 0 && r + 1 < rows.size())
        {
            rows.swapItemsAt(r, r + 1);
            rebuildTable(r + 1);
        }
    });

    connect(btnMoveTop, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r > 0 && r < rows.size())
        {
            CandidateRowState moved = rows.takeAt(r);
            moved.checked = true;
            rows.prepend(moved);
            rebuildTable(0);
        }
    });

    connect(btnGroupTop, &QPushButton::clicked, &dlg, [&]() {
        std::stable_partition(rows.begin(), rows.end(),
                              [](const CandidateRowState &st) { return st.checked; });
        rebuildTable(0);
    });

    connect(btnResetAi, &QPushButton::clicked, &dlg, [&]() {
        rows = initialRows;
        rebuildTable(0);
    });

    connect(targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [&](int idx) {
        if (updatingUi)
        {
            return;
        }
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size() && idx >= 0 && idx < config.targets.size())
        {
            rows[r].targetIndex = idx;
            rebuildTable(r);
        }
    });

    connect(delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), &dlg, [&](int val) {
        if (updatingUi)
        {
            return;
        }
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size() && checkedOrderOfRow(r) != 1)
        {
            rows[r].customDelayMinutes = qMax(1, val);
            rebuildTable(r);
        }
    });

    connect(chkVideoText, &QCheckBox::toggled, &dlg, [&](bool on) {
        if (updatingUi)
        {
            return;
        }
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size() && rows[r].creative.format == QLatin1String("video"))
        {
            rows[r].addVideoText = on;
            if (on && rows[r].videoOverlayText.trimmed().isEmpty())
            {
                rows[r].videoOverlayText = cleanShortHookForOverlay(
                    rows[r].creative.hook, rows[r].creative.productSummary);
            }
            rows[r].rowThumb = makeRowThumb(rows[r]);
            rebuildTable(r);
        }
    });

    connect(comboVideoStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [&](int idx) {
        if (updatingUi || idx < 0)
        {
            return;
        }
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size() && rows[r].creative.format == QLatin1String("video"))
        {
            rows[r].videoTextStyle = normalizeVideoTextStyleId(comboVideoStyle->itemData(idx).toString());
            rows[r].rowThumb = makeRowThumb(rows[r]);
            rebuildTable(r);
        }
    });

    connect(editVideoText, &QLineEdit::textEdited, &dlg, [&](const QString &txt) {
        if (updatingUi)
        {
            return;
        }
        const int r = table->currentRow();
        if (r >= 0 && r < rows.size() && rows[r].creative.format == QLatin1String("video"))
        {
            rows[r].videoOverlayText = txt;
            rows[r].rowThumb = makeRowThumb(rows[r]);
            if (auto *thumbCell = table->item(r, 1))
            {
                thumbCell->setData(Qt::DecorationRole, rows[r].rowThumb);
            }
            QString badgeTxt;
            QColor badgeCol;
            makeBadgeInfo(rows[r].creative, &badgeTxt, &badgeCol);
            largePreviewLbl->setPixmap(renderCreativeCardPixmap(
                rows[r].creative.previewImagePath,
                rows[r].creative.shortCode,
                rows[r].creative.productSummary,
                badgeTxt,
                badgeCol,
                QSize(228, 352),
                rows[r].addVideoText ? rows[r].videoOverlayText : QString{},
                rows[r].videoTextStyle));
            // Also keep row text snippet updated
            const EligibleCreative &c = rows[r].creative;
            const QString hookSnippet = !c.hook.trimmed().isEmpty()
                ? QStringLiteral("\"%1\"").arg(c.hook.trimmed().left(85))
                : tr("(Auto-caption + #hashtags on publish)");
            const QString aiTag = rows[r].aiInitialRank > 0
                ? tr("★ AI Pick #%1: %2").arg(rows[r].aiInitialRank).arg(rows[r].aiReason)
                : rows[r].aiReason;
            const QString videoTextRowNote = rows[r].addVideoText
                ? tr("\n📝 TikTok Text [%1]: \"%2\"")
                      .arg(videoTextStyleShortTag(rows[r].videoTextStyle), rows[r].videoOverlayText.left(60))
                : tr("\n🎬 Clean Video (no TikTok text overlay)");
            if (auto *detCell = table->item(r, 2))
            {
                detCell->setText(QStringLiteral("%1 · VIDEO · %2d old\n%3\nHook: %4%6\n%5")
                    .arg(c.shortCode)
                    .arg(c.ageDays)
                    .arg(c.productSummary, hookSnippet, aiTag, videoTextRowNote));
            }
        }
    });

    connect(table, &QTableWidget::cellDoubleClicked, &dlg, [&](int r, int col) {
        if (col == 0 || r < 0 || r >= rows.size() || rows[r].creative.format != QLatin1String("video"))
        {
            return;
        }
        btnPlayPauseVideo->click();
    });

    connect(btnOpenMedia, &QPushButton::clicked, &dlg, [&]() {
        const int r = table->currentRow();
        if (r < 0 || r >= rows.size() || rows[r].creative.mediaPaths.isEmpty())
        {
            return;
        }
        const EligibleCreative &c = rows[r].creative;
        const QString pathToOpen = c.format == QLatin1String("video")
            ? c.mediaPaths.first()
            : QFileInfo{c.mediaPaths.first()}.absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(pathToOpen));
    });

    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    rebuildTable(0);

    const int dialogRes = dlg.exec();
    mediaPlayer->stop();
    mediaPlayer->setSource(QUrl{});
    mediaPlayer->setVideoOutput(nullptr);

    if (dialogRes != QDialog::Accepted)
    {
        return false;
    }

    // Rebuild m_queue in the exact user-confirmed selection and top-to-bottom order
    QList<ScheduledItem> finalQueue;
    QDateTime cursorTime = QDateTime::currentDateTime();
    for (const CandidateRowState &st : rows)
    {
        if (!st.checked)
        {
            continue;
        }
        const int delayMins = finalQueue.isEmpty() ? 0 : qMax(1, st.customDelayMinutes);
        cursorTime = cursorTime.addSecs(qint64(delayMins) * 60);

        ScheduledItem item;
        item.creative = st.creative;
        item.target = config.targets[qBound(0, st.targetIndex, int(config.targets.size()) - 1)];
        item.delayMinutesFromPrevious = delayMins;
        item.scheduledAt = cursorTime;
        item.addVideoText = (st.creative.format == QLatin1String("video")) && st.addVideoText;
        item.aiSuggestedVideoText = st.aiSuggestedVideoText;
        item.videoTextStyle = normalizeVideoTextStyleId(st.videoTextStyle);
        item.videoOverlayText = st.videoOverlayText.trimmed();
        item.aiReason = st.aiInitialRank > 0
            ? st.aiReason
            : tr("Manually selected by user (%1)").arg(st.creative.format);
        item.status = QStringLiteral("Queued");
        finalQueue.append(item);
    }

    m_queue = finalQueue;
    return !m_queue.isEmpty();
}

void PanePublishing::_refreshQueueTable()
{
    m_queueTable->setRowCount(m_queue.size());
    for (int i = 0; i < m_queue.size(); ++i)
    {
        const auto &it = m_queue[i];
        const auto makeCell = [&](const QString &txt) {
            auto *cell = new QTableWidgetItem(txt);
            if (it.status == QLatin1String("Published") || it.status == QLatin1String("Dry-run OK"))
            {
                cell->setForeground(QColor(QStringLiteral("#8fd19e")));
            }
            else if (it.status == QLatin1String("Failed") || it.status == QLatin1String("Stopped"))
            {
                cell->setForeground(QColor(QStringLiteral("#e89774")));
            }
            else if (it.status == QLatin1String("Uploading"))
            {
                cell->setForeground(QColor(QStringLiteral("#58a6ff")));
            }
            return cell;
        };

        m_queueTable->setItem(i, 0, makeCell(QString::number(i + 1)));
        m_queueTable->setItem(i, 1, makeCell(it.creative.shortCode));
        m_queueTable->setItem(i, 2, makeCell(tr("%1d").arg(it.creative.ageDays)));
        m_queueTable->setItem(i, 3, makeCell(it.creative.format));
        m_queueTable->setItem(i, 4, makeCell(it.creative.productSummary));
        m_queueTable->setItem(i, 5, makeCell(QStringLiteral("%1 — %2")
            .arg(it.target.platformName, it.target.accountUrl)));
        m_queueTable->setItem(i, 6, makeCell(it.scheduledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        m_queueTable->setItem(i, 7, makeCell(QStringLiteral("%1 — %2").arg(it.status, it.aiReason)));
    }
}

void PanePublishing::_updateActionButtons()
{
    bool hasQueued = false;
    for (const auto &it : m_queue)
    {
        if (it.status == QLatin1String("Queued"))
        {
            hasQueued = true;
            break;
        }
    }
    if (m_btnNewJob) m_btnNewJob->setEnabled(!m_jobActive);
    if (m_btnStopJob) m_btnStopJob->setEnabled(m_jobActive);
    if (m_btnPublishNextNow) m_btnPublishNextNow->setEnabled(m_jobActive && !m_uploadingNow && hasQueued);
}

void PanePublishing::_scheduleNextTimer()
{
    if (!m_jobActive || m_uploadingNow)
    {
        return;
    }
    int nextIdx = -1;
    for (int i = 0; i < m_queue.size(); ++i)
    {
        if (m_queue[i].status == QLatin1String("Queued"))
        {
            nextIdx = i;
            break;
        }
    }
    if (nextIdx < 0)
    {
        m_jobActive = false;
        m_countdownUiTimer->stop();
        int pubCount = 0;
        for (const auto &it : m_queue)
        {
            if (it.status == QLatin1String("Published") || it.status == QLatin1String("Dry-run OK"))
            {
                ++pubCount;
            }
        }
        const QString doneMsg = tr("Auto-Publish job completed: %1/%2 post(s) succeeded.")
            .arg(pubCount).arg(m_queue.size());
        m_statusLabel->setText(doneMsg);
        _appendLog(QStringLiteral("[JOB-DONE] ") + doneMsg);
        _updateActionButtons();
        return;
    }

    const qint64 msUntil = QDateTime::currentDateTime().msecsTo(m_queue[nextIdx].scheduledAt);
    if (msUntil <= 1500)
    {
        m_countdownUiTimer->stop();
        // Queue via 0ms single-shot timer so we never replace m_uploadTask while
        // a previous _executeItemCoroutine frame is still unwinding before co_return.
        m_waitTimer->start(0);
    }
    else
    {
        // Cap single QTimer interval at 1 hour and re-evaluate on wakeup for multi-hour waits
        const int waitMs = int(qMin<qint64>(msUntil, 3'600'000));
        m_waitTimer->start(waitMs);
        m_countdownUiTimer->start();
        m_statusLabel->setText(
            tr("Scheduled: next post #%1 (%2 → %3) at %4 (%5 min remaining) — or click \"Publish Next Now\".")
                .arg(nextIdx + 1)
                .arg(m_queue[nextIdx].creative.shortCode,
                     m_queue[nextIdx].target.accountUrl,
                     m_queue[nextIdx].scheduledAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                .arg((msUntil + 59'999) / 60'000));
        _updateActionButtons();
    }
}

void PanePublishing::publishNextNow()
{
    if (!m_jobActive || m_uploadingNow)
    {
        return;
    }
    m_waitTimer->stop();
    m_countdownUiTimer->stop();
    for (int i = 0; i < m_queue.size(); ++i)
    {
        if (m_queue[i].status == QLatin1String("Queued"))
        {
            m_queue[i].scheduledAt = QDateTime::currentDateTime();
            _appendLog(tr("[MANUAL-TRIGGER] Publishing #%1 (%2) immediately...").arg(i + 1).arg(m_queue[i].creative.shortCode));
            break;
        }
    }
    _refreshQueueTable();
    m_waitTimer->start(0);
}

void PanePublishing::stopJob()
{
    if (!m_jobActive)
    {
        return;
    }
    if (m_cancelFlag)
    {
        *m_cancelFlag = true;
    }
    m_waitTimer->stop();
    m_countdownUiTimer->stop();
    for (auto &it : m_queue)
    {
        if (it.status == QLatin1String("Queued") || it.status == QLatin1String("Uploading"))
        {
            it.status = QStringLiteral("Stopped");
        }
    }
    m_jobActive = false;
    m_uploadingNow = false;
    m_statusLabel->setText(tr("Publish job stopped by user."));
    _appendLog(tr("[STOPPED] Publish job stopped by user."));
    _refreshQueueTable();
    _updateActionButtons();
}

void PanePublishing::_launchNextQueuedItem()
{
    if (!m_jobActive || m_uploadingNow)
    {
        return;
    }
    int nextIdx = -1;
    for (int i = 0; i < m_queue.size(); ++i)
    {
        if (m_queue[i].status == QLatin1String("Queued"))
        {
            nextIdx = i;
            break;
        }
    }
    if (nextIdx < 0)
    {
        _scheduleNextTimer();
        return;
    }
    if (QDateTime::currentDateTime().msecsTo(m_queue[nextIdx].scheduledAt) > 1500)
    {
        // Multi-hour timer chunk woke up early; re-arm remaining wait
        _scheduleNextTimer();
        return;
    }

    m_uploadTask = std::make_unique<QCoro::Task<void>>(_executeItemCoroutine(nextIdx));
}

QCoro::Task<void> PanePublishing::_executeItemCoroutine(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= m_queue.size() || !m_jobActive)
    {
        co_return;
    }

    m_currentIndex = itemIndex;
    m_uploadingNow = true;
    m_queue[itemIndex].status = QStringLiteral("Uploading");
    _refreshQueueTable();
    _updateActionButtons();

    const ScheduledItem item = m_queue[itemIndex];
    m_statusLabel->setText(tr("Publishing #%1/%2: %3 (%4) → %5...")
        .arg(itemIndex + 1)
        .arg(m_queue.size())
        .arg(item.creative.shortCode, item.creative.format, item.target.accountUrl));
    _appendLog(tr("------------------------------------------------------------"));
    _appendLog(tr("[PUBLISH #%1/%2] %3 (%4, %5 file(s)) → %6")
        .arg(itemIndex + 1)
        .arg(m_queue.size())
        .arg(item.creative.shortCode, item.creative.format)
        .arg(item.creative.mediaPaths.size())
        .arg(item.target.accountUrl));

    AbstractSocialAccount *platform = AbstractSocialAccount::ALL_SOCIAL_ACCOUNTS()
        .value(item.target.platformId);
    if (!platform || !platform->supportsAutoPublish())
    {
        m_uploadingNow = false;
        m_queue[itemIndex].status = QStringLiteral("Failed");
        m_queue[itemIndex].aiReason = tr("Platform auto-publisher unavailable");
        _appendLog(tr("[ERROR] Platform '%1' does not support auto-publishing.").arg(item.target.platformName));
        _refreshQueueTable();
        _scheduleNextTimer();
        co_return;
    }

    const auto cancelToken = m_cancelFlag;

    // Pre-publish caption & hashtag completeness check:
    // Never let an older generation (like QS-0001) go out with only "qs0001" and zero description/#hashtags!
    QString effectiveCaption = item.creative.caption;
    QString strippedCheck = effectiveCaption;
    strippedCheck.remove(QRegularExpression{QStringLiteral("(?:#\\s*)?\\bqs[-_]?[0-9a-z]+\\b"),
                                            QRegularExpression::CaseInsensitiveOption});
    strippedCheck = strippedCheck.trimmed();
    const bool missingDescOrTags = (strippedCheck.size() < 15) || !strippedCheck.contains(QLatin1Char('#'));
    if (missingDescOrTags)
    {
        const QString projKw = m_projects
            ? m_projects->data(m_projects->index(item.creative.projectRow, TableProjects::IND_KEYWORD)).toString().trimmed()
            : QString{};
        const QString seoKeyword = !projKw.isEmpty() ? projKw : item.creative.productSummary;
        _appendLog(tr("[CAPTION-AUTO] %1 was missing a full description or #hashtags — generating optimized hook + description + hashtags...")
            .arg(item.creative.shortCode));

        QString autoHook;
        QString autoDesc;
        if (m_activeConfig.plannerCli)
        {
            QString capPrompt = QStringLiteral(
                "Write ONE high-converting TikTok post hook and SEO description (with 4-5 relevant #hashtags) "
                "for this fashion/product creative:\n"
                "- Product / Topic: %1\n"
                "- Format: %2\n")
                .arg(item.creative.productSummary.isEmpty() ? seoKeyword : item.creative.productSummary,
                     item.creative.format);
            if (!seoKeyword.isEmpty())
            {
                capPrompt += QStringLiteral("- Target SEO keyword: %1\n").arg(seoKeyword);
            }
            if (m_hashtags)
            {
                capPrompt += m_hashtags->viralHashtagPromptSection(seoKeyword);
            }
            if (m_favoriteHooks)
            {
                capPrompt += m_favoriteHooks->asPromptSection();
            }
            // Include proven winning captions from account_top_posts.json if available
            const QDir wd = WorkingDirectoryManager::instance()->workingDir();
            QFile topFile{wd.filePath(QStringLiteral("account_top_posts.json"))};
            if (topFile.open(QIODevice::ReadOnly))
            {
                const QJsonArray topArr = QJsonDocument::fromJson(topFile.readAll())
                    .object().value(QStringLiteral("posts")).toArray();
                if (!topArr.isEmpty())
                {
                    capPrompt += QStringLiteral("\nProven top-performing posts on our TikTok account (imitate their hook style & hashtag mix):\n");
                    for (int ti = 0; ti < qMin<int>(4, topArr.size()); ++ti)
                    {
                        const QJsonObject tp = topArr[ti].toObject();
                        capPrompt += QStringLiteral("  - (%1 views, %2 likes) %3\n")
                            .arg(tp.value(QStringLiteral("views")).toInteger())
                            .arg(tp.value(QStringLiteral("likes")).toInteger())
                            .arg(tp.value(QStringLiteral("caption")).toString().left(180));
                    }
                }
            }
            capPrompt += QStringLiteral(
                "\nReply with STRICT JSON only (no markdown fences):\n"
                "{\"hook\": \"<short engaging question or hook>\", \"description\": \"<1-2 sentence caption ending with 4-5 #hashtags>\"}");

            CliRunResult capRes;
            capRes = co_await m_activeConfig.plannerCli->runPrompt(capPrompt, QDir::tempPath());
            if (cancelToken && *cancelToken)
            {
                m_uploadingNow = false;
                co_return;
            }
            const QJsonObject capObj = parseJsonObjFromText(capRes.output);
            autoHook = capObj.value(QStringLiteral("hook")).toString().trimmed();
            autoDesc = capObj.value(QStringLiteral("description")).toString().trimmed();
        }

        if (autoHook.isEmpty())
        {
            autoHook = !item.creative.hook.isEmpty()
                ? item.creative.hook
                : tr("How would you style this %1 look?")
                      .arg(seoKeyword.isEmpty() ? QStringLiteral("high heels") : seoKeyword.toLower());
        }
        if (autoDesc.isEmpty())
        {
            autoDesc = !strippedCheck.isEmpty()
                ? strippedCheck
                : tr("Elevated %1 styling for effortless confidence from day to night.")
                      .arg(item.creative.productSummary.isEmpty() ? QStringLiteral("high heels") : item.creative.productSummary);
        }
        if (m_hashtags && !m_hashtags->hashtags(false).isEmpty())
        {
            autoDesc = m_hashtags->optimizeDescriptionHashtags(autoDesc, seoKeyword);
        }
        if (!autoDesc.contains(QLatin1Char('#')))
        {
            QString kwTag = QString{seoKeyword.isEmpty() ? QStringLiteral("highheels") : seoKeyword}
                .remove(QRegularExpression{QStringLiteral("[^A-Za-z0-9]")}).toLower();
            if (kwTag.isEmpty()) kwTag = QStringLiteral("highheels");
            autoDesc = autoDesc.trimmed()
                + QStringLiteral(" #%1 #highheels #heels #fashionshoes #outfitinspo").arg(kwTag);
        }

        const QString codeTag = codeTagFromShortCodePub(item.creative.shortCode);
        QString persistedDesc = autoDesc;
        if (!codeTag.isEmpty() && !persistedDesc.contains(codeTag, Qt::CaseInsensitive))
        {
            persistedDesc += QLatin1Char(' ') + codeTag;
        }
        // Persist back to genDir/hook-description.txt so it is saved permanently
        if (m_projects && item.creative.projectRow >= 0)
        {
            const QDir genDir = m_projects->generationDir(item.creative.projectRow, item.creative.shortCode, true);
            QSaveFile hf{genDir.filePath(QStringLiteral("hook-description.txt"))};
            if (hf.open(QIODevice::WriteOnly))
            {
                hf.write((autoHook + QStringLiteral("\n\n") + persistedDesc + QLatin1Char('\n')).toUtf8());
                hf.commit();
            }
        }
        effectiveCaption = autoHook + QStringLiteral(" — ") + persistedDesc;
        m_queue[itemIndex].creative.hook = autoHook;
        m_queue[itemIndex].creative.caption = effectiveCaption;
        _appendLog(tr("[CAPTION-AUTO] Ready caption (%1 chars): %2")
            .arg(effectiveCaption.size()).arg(effectiveCaption.left(180)));
    }

    // Ensure multi-image Slideshows posted to TikTok ALWAYS have catchy hook text overlaid
    // (automatically upgrading any older carousels that were generated before text overlay was on!)
    if (item.creative.format == QLatin1String("carousel")
        && item.creative.mediaPaths.size() >= 2
        && m_projects && item.creative.projectRow >= 0)
    {
        const QDir genDir = m_projects->generationDir(item.creative.projectRow, item.creative.shortCode, false);
        const QString dnaPath = genDir.filePath(QStringLiteral("creative_dna.json"));
        QJsonObject curDna;
        {
            QFile df{dnaPath};
            if (df.open(QIODevice::ReadOnly))
            {
                curDna = QJsonDocument::fromJson(df.readAll()).object();
            }
        }
        if (!curDna.value(QStringLiteral("hasSlideTextOverlay")).toBool(false))
        {
            QString overlayHook = m_queue[itemIndex].creative.hook.trimmed();
            if (overlayHook.isEmpty())
            {
                const int sep = effectiveCaption.indexOf(QStringLiteral(" — "));
                overlayHook = (sep > 0 ? effectiveCaption.left(sep) : effectiveCaption.left(55)).trimmed();
            }
            if (!overlayHook.isEmpty())
            {
                const QString firstSlide = item.creative.mediaPaths.first();
                const QDir tempDir = m_projects->generationTempDir(item.creative.projectRow, item.creative.shortCode);
                QDir{}.mkpath(tempDir.absolutePath());
                const QString backupSlide = tempDir.filePath(QStringLiteral("slide1_before_overlay.png"));
                if (!QFileInfo::exists(backupSlide))
                {
                    QFile::copy(firstSlide, backupSlide);
                }
                drawHookTextOverSlidePub(firstSlide, overlayHook);
                curDna.insert(QStringLiteral("hasSlideTextOverlay"), true);
                QSaveFile sf{dnaPath};
                if (sf.open(QIODevice::WriteOnly))
                {
                    sf.write(QJsonDocument{curDna}.toJson(QJsonDocument::Indented));
                    sf.commit();
                }
                _appendLog(tr("[SLIDESHOW-TEXT] Overlaid hook text onto Slide 1: \"%1\"").arg(overlayHook));
            }
        }
    }

    AbstractSocialAccount::PublishRequest req;
    req.accountUrl = item.target.accountUrl;
    req.mediaPaths = item.creative.mediaPaths;
    req.caption = effectiveCaption;
    if (item.creative.format == QLatin1String("video") && item.addVideoText)
    {
        req.videoOverlayText = !item.videoOverlayText.trimmed().isEmpty()
            ? item.videoOverlayText.trimmed()
            : cleanShortHookForOverlay(m_queue[itemIndex].creative.hook, item.creative.productSummary);
        req.videoTextStyle = normalizeVideoTextStyleId(item.videoTextStyle);
    }
    req.shortCode = item.creative.shortCode;
    req.useSystemChromeProfile = m_activeConfig.useSystemChromeProfile;
    req.muteOriginalAudio = m_activeConfig.muteOriginalAudio;
    req.useSuggestedSound = m_activeConfig.useSuggestedSound;
    req.dryRun = m_activeConfig.dryRun;
    req.uploadUrlOverride = m_activeConfig.uploadUrlOverride;

    AbstractSocialAccount::PublishResult res;
    res = co_await platform->publishCreative(
        req,
        [this](const QString &logLine) {
            _appendLog(logLine);
        },
        cancelToken);

    m_uploadingNow = false;
    if (cancelToken && *cancelToken)
    {
        co_return;
    }

    if (res.dryRun && res.errorMessage.isEmpty())
    {
        m_queue[itemIndex].status = QStringLiteral("Dry-run OK");
        m_queue[itemIndex].aiReason = tr("Verified upload + caption in Chrome (not posted)");
        _refreshQueueTable();
        _scheduleNextTimer();
        co_return;
    }

    if (res.published)
    {
        m_videos->setPublished(item.creative.videoId, true);
        const QString recordedPostUrl = !res.postUrl.isEmpty()
            ? res.postUrl
            : QStringLiteral("%1#%2").arg(item.target.accountUrl, item.creative.shortCode);
        m_videos->addPublication(
            item.creative.videoId,
            item.target.platformId,
            recordedPostUrl,
            QDateTime::currentDateTimeUtc(),
            item.target.followers);

        // Record publishChannel = "linux-chrome" in creative_dna.json for Linux vs Phone A/B comparison
        const QDir genDir = m_projects->generationDir(item.creative.projectRow, item.creative.shortCode, false);
        const QString dnaPath = genDir.filePath(QStringLiteral("creative_dna.json"));
        QJsonObject dna;
        {
            QFile df{dnaPath};
            if (df.open(QIODevice::ReadOnly))
            {
                dna = QJsonDocument::fromJson(df.readAll()).object();
            }
        }
        dna.insert(QStringLiteral("publishChannel"), QStringLiteral("linux-chrome"));
        dna.insert(QStringLiteral("publishedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        dna.insert(QStringLiteral("publishedAccountUrl"), item.target.accountUrl);
        dna.insert(QStringLiteral("publishedCaption"), effectiveCaption);
        dna.insert(QStringLiteral("mutedOriginalAudio"), m_activeConfig.muteOriginalAudio);
        if (item.creative.format == QLatin1String("video"))
        {
            dna.insert(QStringLiteral("tiktokVideoTextAdded"), res.videoTextApplied);
            dna.insert(QStringLiteral("tiktokVideoText"),
                       res.videoTextApplied ? req.videoOverlayText : QString{});
            dna.insert(QStringLiteral("tiktokVideoTextStyle"),
                       res.videoTextApplied
                           ? (!res.videoTextStyle.isEmpty() ? res.videoTextStyle : req.videoTextStyle)
                           : QString{});
            dna.insert(QStringLiteral("aiSuggestedVideoText"), item.aiSuggestedVideoText);
        }
        if (!res.soundTitle.isEmpty())
        {
            dna.insert(QStringLiteral("tiktokSound"), res.soundTitle);
        }
        {
            QSaveFile sf{dnaPath};
            if (sf.open(QIODevice::WriteOnly))
            {
                sf.write(QJsonDocument{dna}.toJson(QJsonDocument::Indented));
                sf.commit();
            }
        }

        m_queue[itemIndex].status = QStringLiteral("Published");
        m_queue[itemIndex].aiReason = tr("Published via Chrome (%1)").arg(item.target.platformName);
        _refreshQueueTable();
        _scheduleNextTimer();
        co_return;
    }

    // Failure path
    m_queue[itemIndex].status = QStringLiteral("Failed");
    m_queue[itemIndex].aiReason = res.errorMessage.left(120);
    _appendLog(tr("[FAILURE] %1 failed: %2").arg(item.creative.shortCode, res.errorMessage));
    if (!res.logFilePath.isEmpty())
    {
        _appendLog(tr("[DEBUG] Detailed worker log: %1").arg(res.logFilePath));
    }
    if (!res.screenshotPath.isEmpty())
    {
        _appendLog(tr("[DEBUG] Failure screenshot: %1").arg(res.screenshotPath));
    }

    if (res.fatal)
    {
        // Stop remaining items so we never keep hammering a broken login / mismatched account / UI change
        for (int j = itemIndex + 1; j < m_queue.size(); ++j)
        {
            if (m_queue[j].status == QLatin1String("Queued"))
            {
                m_queue[j].status = QStringLiteral("Stopped");
            }
        }
        m_jobActive = false;
        m_waitTimer->stop();
        m_countdownUiTimer->stop();
        const QString stopMsg = tr("Job stopped automatically so you can inspect the issue: %1").arg(res.errorMessage);
        m_statusLabel->setText(stopMsg);
        _appendLog(QStringLiteral("[AUTO-STOP] ") + stopMsg);
        _refreshQueueTable();
        _updateActionButtons();
        co_return;
    }

    _refreshQueueTable();
    _scheduleNextTimer();
}
