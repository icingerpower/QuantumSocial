#include "DialogGenerationOptions.h"
#include "ui_DialogGenerationOptions.h"

#include <QEvent>
#include <QSettings>

#include "AbstractCli.h"

namespace {
const QString SETTING_MODE = QStringLiteral("generation/imageMode");
const QString SETTING_CLI = QStringLiteral("generation/imageCli");
const QString SETTING_FORMAT = QStringLiteral("generation/videoFormat");
const QString SETTING_WHITE_BACKGROUND = QStringLiteral("generation/whiteBackgroundProduct");
const QString SETTING_APPLY_BOTH = QStringLiteral("generation/applyToBothImages");
}

DialogGenerationOptions::DialogGenerationOptions(
    const QList<AbstractCli *> &availableClis, bool hasImage,
    bool hasPreviousGenerated, const QString &previousGeneratedImagePath,
    bool hasSecondaryImage,
    QWidget *parent,
    bool hasResearchReference,
    const QString &researchReferencePreviewPath)
    : QDialog(parent)
    , ui(new Ui::DialogGenerationOptions)
    , m_hasSecondaryImage(hasSecondaryImage)
    , m_hasResearchReference(hasResearchReference)
{
    ui->setupUi(this);
    setMaximumWidth(1600);

    // No input image: nothing to decide about it, only the format is asked
    // (imageMode() stays KeepInput through the hidden default radio).
    ui->groupInputImage->setVisible(hasImage);
    ui->radioReusePrevious->setVisible(hasPreviousGenerated);
    ui->radioReplaceReferenceProduct->setVisible(hasResearchReference);
    ui->lineEditSwapInstruction->setVisible(hasResearchReference);

    if (hasPreviousGenerated)
    {
        m_previousImagePixmap = QPixmap{previousGeneratedImagePath};
    }
    if (hasResearchReference && !researchReferencePreviewPath.isEmpty())
    {
        m_researchReferencePixmap = QPixmap{researchReferencePreviewPath};
    }
    if (!m_previousImagePixmap.isNull() || !m_researchReferencePixmap.isNull())
    {
        ui->labelPreviousImagePreview->installEventFilter(this);
    }

    for (AbstractCli *cli : availableClis)
    {
        if (cli->canGenImages())
        {
            ui->comboBoxCli->addItem(cli->getName(), QVariant::fromValue(cli));
        }
    }
    if (ui->comboBoxCli->count() == 0)
    {
        // Without an image-capable CLI only "keep as it is" can work.
        const QString why = tr("No image-capable CLI is available.");
        ui->radioReplaceReferenceProduct->setEnabled(false);
        ui->radioReplaceReferenceProduct->setToolTip(why);
        ui->radioRegenerate->setEnabled(false);
        ui->radioRegenerate->setToolTip(why);
        ui->radioBootstrap->setEnabled(false);
        ui->radioBootstrap->setToolTip(why);
    }

    QSettings settings;
    const int savedMode = settings.value(SETTING_MODE, 0).toInt();
    if (hasPreviousGenerated)
    {
        // A regenerated image already exists: preselect its reuse so a
        // re-run does not silently redo the (slow) image step.
        ui->radioReusePrevious->setChecked(true);
    }
    else if (hasResearchReference && ui->radioReplaceReferenceProduct->isEnabled())
    {
        // Research project with a Pinterest/social reference: preselect replacing
        // the reference's product with our product before animating.
        ui->radioReplaceReferenceProduct->setChecked(true);
    }
    else if (savedMode == static_cast<int>(ImageMode::RegenerateInput)
             && ui->radioRegenerate->isEnabled())
    {
        ui->radioRegenerate->setChecked(true);
    }
    else if (savedMode == static_cast<int>(ImageMode::BootstrapImage)
             && ui->radioBootstrap->isEnabled())
    {
        ui->radioBootstrap->setChecked(true);
    }
    ui->checkBoxWhiteBackground->setChecked(
        settings.value(SETTING_WHITE_BACKGROUND, false).toBool());
    ui->checkBoxApplyBoth->setVisible(hasSecondaryImage);
    ui->checkBoxApplyBoth->setChecked(
        hasSecondaryImage && settings.value(SETTING_APPLY_BOTH, false).toBool());
    const int savedCli = ui->comboBoxCli->findText(settings.value(SETTING_CLI).toString());
    if (savedCli >= 0)
    {
        ui->comboBoxCli->setCurrentIndex(savedCli);
    }
    const int savedFormat = settings.value(
        SETTING_FORMAT, static_cast<int>(VideoFormat::Vertical916)).toInt();
    if (savedFormat == static_cast<int>(VideoFormat::Horizontal169))
    {
        ui->radioHorizontal->setChecked(true);
    }
    else if (savedFormat == static_cast<int>(VideoFormat::Square))
    {
        ui->radioSquare->setChecked(true);
    }

    connect(ui->radioKeep, &QRadioButton::toggled,
            this, &DialogGenerationOptions::_updateCliEnabled);
    connect(ui->radioReusePrevious, &QRadioButton::toggled,
            this, &DialogGenerationOptions::_updateCliEnabled);
    connect(ui->radioReplaceReferenceProduct, &QRadioButton::toggled,
            this, &DialogGenerationOptions::_updateCliEnabled);
    connect(ui->radioRegenerate, &QRadioButton::toggled,
            this, &DialogGenerationOptions::_updateCliEnabled);
    connect(ui->radioBootstrap, &QRadioButton::toggled,
            this, &DialogGenerationOptions::_updateCliEnabled);
    _updateCliEnabled();
}

DialogGenerationOptions::~DialogGenerationOptions()
{
    delete ui;
}

bool DialogGenerationOptions::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->labelPreviousImagePreview && event->type() == QEvent::Resize)
    {
        _rescalePreviousImagePreview();
    }
    return QDialog::eventFilter(watched, event);
}

void DialogGenerationOptions::_rescalePreviousImagePreview()
{
    const bool showRef = ui->radioReplaceReferenceProduct->isChecked()
        && !m_researchReferencePixmap.isNull();
    const QPixmap &activePixmap = showRef
        ? m_researchReferencePixmap
        : (!m_previousImagePixmap.isNull() ? m_previousImagePixmap : m_researchReferencePixmap);
    if (activePixmap.isNull())
    {
        ui->groupPreviousImagePreview->setVisible(false);
        return;
    }
    ui->groupPreviousImagePreview->setTitle(
        (showRef || m_previousImagePixmap.isNull())
            ? tr("Reference scene to adapt")
            : tr("Previously regenerated image"));
    ui->groupPreviousImagePreview->setVisible(true);
    ui->labelPreviousImagePreview->setPixmap(activePixmap.scaled(
        ui->labelPreviousImagePreview->size(), Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
}

QString DialogGenerationOptions::formatLabel(VideoFormat format)
{
    switch (format)
    {
    case VideoFormat::Horizontal169:
        return QStringLiteral("horizontal (16:9)");
    case VideoFormat::Square:
        return QStringLiteral("square (1:1)");
    case VideoFormat::Vertical916:
        break;
    }
    return QStringLiteral("vertical (9:16)");
}

DialogGenerationOptions::VideoFormat DialogGenerationOptions::videoFormat() const
{
    if (ui->radioHorizontal->isChecked())
    {
        return VideoFormat::Horizontal169;
    }
    if (ui->radioSquare->isChecked())
    {
        return VideoFormat::Square;
    }
    return VideoFormat::Vertical916;
}

DialogGenerationOptions::ImageMode DialogGenerationOptions::imageMode() const
{
    if (!ui->groupInputImage->isVisibleTo(this))
    {
        // No input image: the restored radios were never shown.
        return ImageMode::KeepInput;
    }
    if (ui->radioReusePrevious->isChecked())
    {
        return ImageMode::ReusePrevious;
    }
    if (m_hasResearchReference && ui->radioReplaceReferenceProduct->isChecked())
    {
        return ImageMode::ReplaceReferenceProduct;
    }
    if (ui->radioRegenerate->isChecked())
    {
        return ImageMode::RegenerateInput;
    }
    if (ui->radioBootstrap->isChecked())
    {
        return ImageMode::BootstrapImage;
    }
    return ImageMode::KeepInput;
}

AbstractCli *DialogGenerationOptions::imageCli() const
{
    const ImageMode mode = imageMode();
    if (mode == ImageMode::KeepInput || mode == ImageMode::ReusePrevious)
    {
        return nullptr;
    }
    return ui->comboBoxCli->currentData().value<AbstractCli *>();
}

void DialogGenerationOptions::accept()
{
    QSettings settings;
    settings.setValue(SETTING_FORMAT, static_cast<int>(videoFormat()));
    // The image choices were not shown without an image — don't let the
    // hidden defaults overwrite the remembered ones.
    if (ui->groupInputImage->isVisibleTo(this))
    {
        if (imageMode() != ImageMode::ReplaceReferenceProduct)
        {
            settings.setValue(SETTING_MODE, static_cast<int>(imageMode()));
        }
        settings.setValue(SETTING_WHITE_BACKGROUND, ui->checkBoxWhiteBackground->isChecked());
        if (m_hasSecondaryImage)
        {
            settings.setValue(SETTING_APPLY_BOTH, ui->checkBoxApplyBoth->isChecked());
        }
        if (ui->comboBoxCli->currentIndex() >= 0)
        {
            settings.setValue(SETTING_CLI, ui->comboBoxCli->currentText());
        }
    }
    QDialog::accept();
}

void DialogGenerationOptions::_updateCliEnabled()
{
    const ImageMode mode = imageMode();
    ui->comboBoxCli->setEnabled((mode == ImageMode::ReplaceReferenceProduct
                                 || mode == ImageMode::RegenerateInput
                                 || mode == ImageMode::BootstrapImage)
                                && ui->comboBoxCli->count() > 0);
    ui->labelCli->setEnabled(ui->comboBoxCli->isEnabled());
    ui->lineEditSwapInstruction->setEnabled(
        m_hasResearchReference && mode == ImageMode::ReplaceReferenceProduct);
    // Only makes sense for RegenerateInput — disabled (and left unchecked
    // when not applicable) rather than hidden, so its state isn't lost by
    // toggling between radios back and forth.
    ui->checkBoxWhiteBackground->setEnabled(mode == ImageMode::RegenerateInput);
    ui->checkBoxApplyBoth->setEnabled(m_hasSecondaryImage && mode == ImageMode::RegenerateInput);
    _rescalePreviousImagePreview();
}

QString DialogGenerationOptions::swapInstruction() const
{
    return (m_hasResearchReference && ui->radioReplaceReferenceProduct->isChecked())
        ? ui->lineEditSwapInstruction->text().trimmed()
        : QString{};
}

bool DialogGenerationOptions::whiteBackgroundProduct() const
{
    return ui->radioRegenerate->isChecked() && ui->checkBoxWhiteBackground->isChecked();
}

bool DialogGenerationOptions::applyToBothImages() const
{
    return m_hasSecondaryImage
        && ui->radioRegenerate->isChecked()
        && ui->checkBoxApplyBoth->isChecked();
}
