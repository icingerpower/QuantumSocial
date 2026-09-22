#include "MultiVideoStepWidget.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

MultiVideoStepWidget::MultiVideoStepWidget(int stepIndex, bool isIntro, QWidget *parent)
    : QGroupBox(parent)
    , m_stepIndex(stepIndex)
    , m_isIntro(isIntro)
{
    setTitle(isIntro ? tr("Step %1: Intro (hook / opener)").arg(stepIndex + 1)
                     : tr("Step %1: Product %2 (showcase)").arg(stepIndex + 1).arg(stepIndex));

    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(10);

    for (int i = 0; i < MAX_IMAGES_PER_STEP; ++i)
    {
        auto *slotBox = new QGroupBox(tr("Image %1").arg(i + 1), this);
        auto *slotLayout = new QVBoxLayout(slotBox);
        slotLayout->setContentsMargins(4, 4, 4, 4);
        slotLayout->setSpacing(4);

        auto *preview = new QLabel(slotBox);
        preview->setFixedSize(90, 90);
        preview->setStyleSheet(QStringLiteral(
            "border: 1px dashed #777; border-radius: 4px; background: #222; color: #888;"));
        preview->setAlignment(Qt::AlignCenter);
        preview->setText(tr("No image"));
        preview->setScaledContents(true);
        slotLayout->addWidget(preview, 0, Qt::AlignCenter);

        auto *pathLabel = new QLabel(slotBox);
        pathLabel->setAlignment(Qt::AlignCenter);
        pathLabel->setText(tr("(empty)"));
        pathLabel->setStyleSheet(QStringLiteral("font-size: 10px; color: #aaa;"));
        slotLayout->addWidget(pathLabel);

        auto *btnLayout = new QHBoxLayout();
        btnLayout->setSpacing(4);
        auto *browseBtn = new QPushButton(tr("Browse..."), slotBox);
        auto *clearBtn = new QPushButton(tr("✕"), slotBox);
        clearBtn->setFixedWidth(24);
        btnLayout->addWidget(browseBtn);
        btnLayout->addWidget(clearBtn);
        slotLayout->addLayout(btnLayout);

        auto *regenCombo = new QComboBox(slotBox);
        regenCombo->addItem(tr("Isolate on white"),
                            static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite));
        regenCombo->addItem(tr("Keep original"),
                            static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::KeepOriginal));
        regenCombo->addItem(tr("Regenerate with CLI"),
                            static_cast<int>(AbstractMultiVideoWorkflow::ImageRegenOption::Regenerate));
        const int defaultIdx = regenCombo->findData(static_cast<int>(m_defaultRegenOption));
        if (defaultIdx >= 0)
        {
            regenCombo->setCurrentIndex(defaultIdx);
        }
        slotLayout->addWidget(regenCombo);

        m_slots[i].labelPreview = preview;
        m_slots[i].labelPath = pathLabel;
        m_slots[i].buttonBrowse = browseBtn;
        m_slots[i].buttonClear = clearBtn;
        m_slots[i].comboRegen = regenCombo;

        connect(browseBtn, &QPushButton::clicked, this, [this, i]() {
            _browseSlot(i);
        });
        connect(clearBtn, &QPushButton::clicked, this, [this, i]() {
            _clearSlot(i);
        });
        connect(regenCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &MultiVideoStepWidget::changed);

        mainLayout->addWidget(slotBox);
    }
}

QList<AbstractMultiVideoWorkflow::ImageInput> MultiVideoStepWidget::imageInputs() const
{
    QList<AbstractMultiVideoWorkflow::ImageInput> inputs;
    for (int i = 0; i < MAX_IMAGES_PER_STEP; ++i)
    {
        if (!m_slots[i].imagePath.isEmpty() && QFileInfo::exists(m_slots[i].imagePath))
        {
            AbstractMultiVideoWorkflow::ImageInput input;
            input.originalPath = m_slots[i].imagePath;
            input.regenOption = static_cast<AbstractMultiVideoWorkflow::ImageRegenOption>(
                m_slots[i].comboRegen->currentData().toInt());
            inputs.append(input);
        }
    }
    return inputs;
}

void MultiVideoStepWidget::setImage(int slotIndex, const QString &path,
                                    AbstractMultiVideoWorkflow::ImageRegenOption option)
{
    if (slotIndex < 0 || slotIndex >= MAX_IMAGES_PER_STEP)
    {
        return;
    }
    m_slots[slotIndex].imagePath = path;
    const int comboIndex = m_slots[slotIndex].comboRegen->findData(static_cast<int>(option));
    if (comboIndex >= 0)
    {
        m_slots[slotIndex].comboRegen->setCurrentIndex(comboIndex);
    }
    _updateSlotPreview(slotIndex);
    emit changed();
}

void MultiVideoStepWidget::setSlotRegenOption(int slotIndex,
                                              AbstractMultiVideoWorkflow::ImageRegenOption option)
{
    if (slotIndex < 0 || slotIndex >= MAX_IMAGES_PER_STEP)
    {
        return;
    }
    const int idx = m_slots[slotIndex].comboRegen->findData(static_cast<int>(option));
    if (idx >= 0 && m_slots[slotIndex].comboRegen->currentIndex() != idx)
    {
        m_slots[slotIndex].comboRegen->setCurrentIndex(idx);
    }
}

void MultiVideoStepWidget::setAllSlotsRegenOption(AbstractMultiVideoWorkflow::ImageRegenOption option)
{
    m_defaultRegenOption = option;
    for (int i = 0; i < MAX_IMAGES_PER_STEP; ++i)
    {
        setSlotRegenOption(i, option);
    }
}

void MultiVideoStepWidget::setDefaultRegenOption(AbstractMultiVideoWorkflow::ImageRegenOption option)
{
    m_defaultRegenOption = option;
}

void MultiVideoStepWidget::_browseSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= MAX_IMAGES_PER_STEP)
    {
        return;
    }
    const QString filter = tr("Image files (*.png *.jpg *.jpeg *.webp *.bmp);;All files (*.*)");
    const QString chosen = QFileDialog::getOpenFileName(this, tr("Select image for %1").arg(title()),
                                                        QString{}, filter);
    if (!chosen.isEmpty())
    {
        m_slots[slotIndex].imagePath = chosen;
        _updateSlotPreview(slotIndex);
        emit changed();
    }
}

void MultiVideoStepWidget::_clearSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= MAX_IMAGES_PER_STEP)
    {
        return;
    }
    m_slots[slotIndex].imagePath.clear();
    _updateSlotPreview(slotIndex);
    emit changed();
}

void MultiVideoStepWidget::_updateSlotPreview(int slotIndex)
{
    auto &slot = m_slots[slotIndex];
    if (slot.imagePath.isEmpty() || !QFileInfo::exists(slot.imagePath))
    {
        slot.labelPreview->clear();
        slot.labelPreview->setText(tr("No image"));
        slot.labelPath->setText(tr("(empty)"));
        slot.labelPath->setToolTip(QString{});
        return;
    }

    const QPixmap pixmap{slot.imagePath};
    if (!pixmap.isNull())
    {
        slot.labelPreview->setPixmap(pixmap.scaled(
            slot.labelPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    else
    {
        slot.labelPreview->setText(tr("Invalid"));
    }
    const QString fileName = QFileInfo{slot.imagePath}.fileName();
    slot.labelPath->setText(fileName);
    slot.labelPath->setToolTip(slot.imagePath);
}
