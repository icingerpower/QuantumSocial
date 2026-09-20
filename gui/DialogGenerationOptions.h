#ifndef DIALOGGENERATIONOPTIONS_H
#define DIALOGGENERATIONOPTIONS_H

#include <QDialog>
#include <QList>
#include <QPixmap>

QT_BEGIN_NAMESPACE
namespace Ui { class DialogGenerationOptions; }
QT_END_NAMESPACE

class AbstractCli;

// Asked before generating: the video format (9:16 / 16:9 / square), what to
// do with the project's input image, and which CLI performs that image step.
// This CLI is deliberately separate from the prompt-suggestion CLI selected
// at the top of the Generation pane — image work and prompt writing may be
// best served by different tools. Only image-capable CLIs (canGenImages) are
// offered; the last choices are remembered across sessions. Without an input
// image (hasImage == false) only the video format is asked.
class DialogGenerationOptions : public QDialog
{
    Q_OBJECT

public:
    enum class ImageMode
    {
        KeepInput,        // use the source image untouched
        RegenerateInput,  // same image, cleaned (no social icons/text) + quality
        BootstrapImage,   // a close but different image is generated
        ReusePrevious,    // reuse generation_source.png from an earlier run
    };

    enum class VideoFormat
    {
        Vertical916,
        Horizontal169,
        Square,
    };

    // hasPreviousGenerated: a generation_source.png from an earlier run
    // exists — offers (and preselects) "reuse it", precious during
    // development to avoid re-running the image step on every attempt.
    // previousGeneratedImagePath: its path, shown as a preview next to the
    // "reuse" option so the choice isn't a leap of faith; ignored (no
    // preview shown) when hasPreviousGenerated is false.
    explicit DialogGenerationOptions(const QList<AbstractCli *> &availableClis,
                                     bool hasImage,
                                     bool hasPreviousGenerated,
                                     const QString &previousGeneratedImagePath,
                                     bool hasSecondaryImage = false,
                                     QWidget *parent = nullptr);
    ~DialogGenerationOptions();

    // Human-readable form used inside prompts, e.g. "vertical (9:16)".
    static QString formatLabel(VideoFormat format);

    ImageMode imageMode() const;
    VideoFormat videoFormat() const;
    // The CLI for the image step; nullptr when KeepInput is selected.
    AbstractCli *imageCli() const;
    // Only meaningful when imageMode() == RegenerateInput: isolate the
    // product alone on a plain white background instead of a cleaned-up
    // version of the same photo (no model/human at all) — a dedicated base
    // shot useful as its own asset and as an unambiguous reference for
    // later generations.
    bool whiteBackgroundProduct() const;
    // When 2 input images are configured and imageMode() == RegenerateInput,
    // whether to apply the regeneration step to both images instead of only
    // the primary one.
    bool applyToBothImages() const;

    void accept() override;

protected:
    // Rescales the preview pixmap when the dialog (and so the preview
    // label) is resized — the label only ever holds a scaled copy, never
    // the original, so this must re-derive it from m_previousImagePixmap
    // rather than re-scaling an already-downscaled pixmap.
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void _updateCliEnabled();

private:
    Ui::DialogGenerationOptions *ui;
    QPixmap m_previousImagePixmap;
    bool m_hasSecondaryImage = false;

    void _rescalePreviousImagePreview();
};

#endif // DIALOGGENERATIONOPTIONS_H
