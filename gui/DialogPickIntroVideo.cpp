#include "DialogPickIntroVideo.h"

#include <QAudioOutput>
#include <QBoxLayout>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QUrl>
#include <QVideoWidget>

DialogPickIntroVideo::DialogPickIntroVideo(const QList<AbstractMultiVideoWorkflow::TakeInfo> &introTakes,
                                           int defaultSelectedIndex,
                                           QWidget *parent)
    : DialogPickIntroVideo(0, tr("Step 1 (Intro)"), introTakes, defaultSelectedIndex, parent)
{
}

DialogPickIntroVideo::DialogPickIntroVideo(int stepIndex,
                                           const QString &stepTitle,
                                           const QList<AbstractMultiVideoWorkflow::TakeInfo> &takes,
                                           int defaultSelectedIndex,
                                           QWidget *parent)
    : QDialog(parent)
    , m_stepIndex(stepIndex)
    , m_stepTitle(stepTitle)
    , m_introTakes(takes)
    , m_selectedTakeIndex(qBound(0, defaultSelectedIndex, qMax(0, takes.size() - 1)))
{
    const QString displayTitle = stepTitle.isEmpty()
        ? tr("Step %1").arg(stepIndex + 1)
        : stepTitle;

    if (stepIndex == 0)
    {
        setWindowTitle(tr("Select Base Intro Video"));
    }
    else
    {
        setWindowTitle(tr("Select Video for %1").arg(displayTitle));
    }
    resize(850, 580);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Header banner
    QLabel *labelTitle = nullptr;
    QLabel *labelDesc = nullptr;

    if (stepIndex == 0)
    {
        labelTitle = new QLabel(tr("<h3>Select Intro Video for Subsequent Steps</h3>"), this);
        labelDesc = new QLabel(
            tr("Multiple intro videos were generated. Please pick the best one to use as the base for the showcase.\n"
               "All subsequent steps will continue inside that video's conversation so the background, model, and lighting remain uniform."),
            this);
    }
    else
    {
        labelTitle = new QLabel(tr("<h3>Select Preferred Video for %1</h3>").arg(displayTitle), this);
        labelDesc = new QLabel(
            tr("Multiple takes were generated for %1. Please preview each take and select your preferred version for the showcase.")
                .arg(displayTitle),
            this);
    }
    mainLayout->addWidget(labelTitle);
    labelDesc->setWordWrap(true);
    labelDesc->setStyleSheet(QStringLiteral("color: #666; font-size: 12px; margin-bottom: 4px;"));
    mainLayout->addWidget(labelDesc);

    // Splitter: List on left, Player on right
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Left container: list of takes
    auto *leftGroup = new QGroupBox((stepIndex == 0) ? tr("Generated Intro Takes") : tr("Generated Takes"), splitter);
    auto *leftLayout = new QVBoxLayout(leftGroup);
    m_listTakes = new QListWidget(leftGroup);
    m_listTakes->setSelectionMode(QAbstractItemView::SingleSelection);

    for (int i = 0; i < m_introTakes.size(); ++i)
    {
        const auto &take = m_introTakes.at(i);
        QString sizeStr;
        QString resolvedPath = take.filePath;
        if ((resolvedPath.isEmpty() || !QFileInfo::exists(resolvedPath)) && !take.expectedFileName.isEmpty())
        {
            const QString baseDir = !take.filePath.isEmpty()
                ? QFileInfo{take.filePath}.absolutePath()
                : QString{};
            if (!baseDir.isEmpty())
            {
                const QDir dir{baseDir};
                if (dir.exists(take.expectedFileName))
                {
                    resolvedPath = dir.absoluteFilePath(take.expectedFileName);
                }
                else
                {
                    const auto subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const auto &sub : subDirs)
                    {
                        const QString subCandidate = dir.absoluteFilePath(sub + QLatin1Char('/') + take.expectedFileName);
                        if (QFileInfo::exists(subCandidate))
                        {
                            resolvedPath = subCandidate;
                            break;
                        }
                    }
                }
            }
        }
        if (!resolvedPath.isEmpty() && QFileInfo::exists(resolvedPath))
        {
            const QFileInfo fi(resolvedPath);
            sizeStr = QStringLiteral(" (%1 MB)").arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 1);
        }
        const QString itemText = tr("Take %1: %2%3")
            .arg(take.takeIndex + 1)
            .arg(take.expectedFileName)
            .arg(sizeStr);

        auto *item = new QListWidgetItem(itemText, m_listTakes);
        item->setData(Qt::UserRole, i);
    }

    if (m_introTakes.size() > 0)
    {
        m_listTakes->setCurrentRow(m_selectedTakeIndex);
    }
    leftLayout->addWidget(m_listTakes);
    splitter->addWidget(leftGroup);

    // Right container: video preview
    auto *rightGroup = new QGroupBox(tr("Video Preview"), splitter);
    auto *rightLayout = new QVBoxLayout(rightGroup);

    m_videoWidget = new QVideoWidget(rightGroup);
    m_videoWidget->setMinimumSize(320, 360);
    rightLayout->addWidget(m_videoWidget, 1);

    // Player controls
    auto *controlsLayout = new QHBoxLayout();
    m_buttonPlayPause = new QPushButton(tr("Play"), rightGroup);
    m_buttonPlayPause->setFixedWidth(75);
    controlsLayout->addWidget(m_buttonPlayPause);

    m_slider = new QSlider(Qt::Horizontal, rightGroup);
    m_slider->setRange(0, 0);
    controlsLayout->addWidget(m_slider, 1);

    m_labelTime = new QLabel(QStringLiteral("0:00 / 0:00"), rightGroup);
    controlsLayout->addWidget(m_labelTime);

    rightLayout->addLayout(controlsLayout);
    splitter->addWidget(rightGroup);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    mainLayout->addWidget(splitter, 1);

    // Setup media player
    m_mediaPlayer = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_mediaPlayer->setAudioOutput(m_audioOutput);
    m_mediaPlayer->setVideoOutput(m_videoWidget);

    connect(m_buttonPlayPause, &QPushButton::clicked, this, &DialogPickIntroVideo::_togglePlayPause);
    connect(m_slider, &QSlider::sliderMoved, this, &DialogPickIntroVideo::_onSliderMoved);
    connect(m_mediaPlayer, &QMediaPlayer::positionChanged, this, &DialogPickIntroVideo::_onPositionChanged);
    connect(m_mediaPlayer, &QMediaPlayer::durationChanged, this, &DialogPickIntroVideo::_onDurationChanged);
    connect(m_listTakes, &QListWidget::currentRowChanged, this, &DialogPickIntroVideo::_onTakeSelectionChanged);

    // Bottom action buttons
    auto *bottomLayout = new QHBoxLayout();

    auto *labelTakes = new QLabel(tr("New takes:"), this);
    bottomLayout->addWidget(labelTakes);

    m_spinTakes = new QSpinBox(this);
    m_spinTakes->setRange(1, 10);
    m_spinTakes->setValue(1);
    m_spinTakes->setToolTip(tr("Number of new takes/videos to generate for this step"));
    bottomLayout->addWidget(m_spinTakes);

    m_buttonGenerateAgain = new QPushButton(tr("Generate again"), this);
    m_buttonGenerateAgain->setToolTip(tr("Generate additional takes for this step without restarting from the beginning"));
    connect(m_buttonGenerateAgain, &QPushButton::clicked, this, &DialogPickIntroVideo::_onGenerateAgainClicked);
    bottomLayout->addWidget(m_buttonGenerateAgain);

    m_buttonResetStep = new QPushButton(tr("Reset this step"), this);
    m_buttonResetStep->setToolTip(tr("Delete existing takes for this step and reset it for clean regeneration"));
    connect(m_buttonResetStep, &QPushButton::clicked, this, &DialogPickIntroVideo::_onResetStepClicked);
    bottomLayout->addWidget(m_buttonResetStep);

    bottomLayout->addStretch(1);

    m_buttonPause = new QPushButton((stepIndex == 0) ? tr("Decide Later / Pause") : tr("Skip / Decide Later"), this);
    connect(m_buttonPause, &QPushButton::clicked, this, &QDialog::reject);
    bottomLayout->addWidget(m_buttonPause);

    const QString continueText = (stepIndex == 0)
        ? tr("Use This Video & Continue (Step 2)")
        : tr("Use This Video");
    m_buttonContinue = new QPushButton(continueText, this);
    m_buttonContinue->setDefault(true);
    m_buttonContinue->setStyleSheet(QStringLiteral("QPushButton { font-weight: bold; padding: 6px 16px; }"));
    connect(m_buttonContinue, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(m_buttonContinue);

    mainLayout->addLayout(bottomLayout);

    // Load initial video
    _onTakeSelectionChanged();
}

DialogPickIntroVideo::~DialogPickIntroVideo()
{
    if (m_mediaPlayer)
    {
        m_mediaPlayer->stop();
    }
}

int DialogPickIntroVideo::selectedTakeIndex() const
{
    return m_selectedTakeIndex;
}

void DialogPickIntroVideo::_onTakeSelectionChanged()
{
    const int row = m_listTakes->currentRow();
    if (row < 0 || row >= m_introTakes.size())
    {
        return;
    }

    m_selectedTakeIndex = row;
    const auto &take = m_introTakes.at(row);

    if (m_mediaPlayer)
    {
        m_mediaPlayer->stop();
        QString videoPath = take.filePath;
        if ((videoPath.isEmpty() || !QFileInfo::exists(videoPath)) && !take.expectedFileName.isEmpty())
        {
            const QString baseDir = !take.filePath.isEmpty()
                ? QFileInfo{take.filePath}.absolutePath()
                : QString{};
            if (!baseDir.isEmpty())
            {
                const QDir dir{baseDir};
                if (dir.exists(take.expectedFileName))
                {
                    videoPath = dir.absoluteFilePath(take.expectedFileName);
                }
                else
                {
                    const auto subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const auto &sub : subDirs)
                    {
                        const QString subCandidate = dir.absoluteFilePath(sub + QLatin1Char('/') + take.expectedFileName);
                        if (QFileInfo::exists(subCandidate))
                        {
                            videoPath = subCandidate;
                            break;
                        }
                    }
                }
            }
        }

        if (!videoPath.isEmpty() && QFileInfo::exists(videoPath))
        {
            m_mediaPlayer->setSource(QUrl::fromLocalFile(videoPath));
            m_mediaPlayer->play();
            m_buttonPlayPause->setText(tr("Pause"));
        }
        else
        {
            m_buttonPlayPause->setText(tr("Play"));
            m_slider->setRange(0, 0);
            m_labelTime->setText(QStringLiteral("0:00 / 0:00"));
        }
    }
}

void DialogPickIntroVideo::_togglePlayPause()
{
    if (!m_mediaPlayer)
    {
        return;
    }

    if (m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
    {
        m_mediaPlayer->pause();
        m_buttonPlayPause->setText(tr("Play"));
    }
    else
    {
        m_mediaPlayer->play();
        m_buttonPlayPause->setText(tr("Pause"));
    }
}

void DialogPickIntroVideo::_onPositionChanged(qint64 position)
{
    if (!m_slider->isSliderDown())
    {
        m_slider->setValue(static_cast<int>(position));
    }
    const qint64 duration = m_mediaPlayer->duration();
    const int posSec = static_cast<int>(position / 1000);
    const int durSec = static_cast<int>(duration / 1000);
    m_labelTime->setText(QStringLiteral("%1:%2 / %3:%4")
        .arg(posSec / 60).arg(posSec % 60, 2, 10, QLatin1Char('0'))
        .arg(durSec / 60).arg(durSec % 60, 2, 10, QLatin1Char('0')));
}

void DialogPickIntroVideo::_onDurationChanged(qint64 duration)
{
    m_slider->setRange(0, static_cast<int>(duration));
}

void DialogPickIntroVideo::_onSliderMoved(int position)
{
    if (m_mediaPlayer)
    {
        m_mediaPlayer->setPosition(position);
    }
}

void DialogPickIntroVideo::_onGenerateAgainClicked()
{
    m_regenerateRequested = true;
    m_regenerateCount = m_spinTakes ? m_spinTakes->value() : 1;
    accept();
}

void DialogPickIntroVideo::_onResetStepClicked()
{
    const QString displayTitle = m_stepTitle.isEmpty()
        ? tr("Step %1").arg(m_stepIndex + 1)
        : m_stepTitle;
    const auto reply = QMessageBox::question(
        this,
        tr("Reset Step"),
        tr("Are you sure you want to reset %1?\nThis will remove its generated video files so you can generate them again.").arg(displayTitle),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (reply == QMessageBox::Yes)
    {
        m_resetRequested = true;
        accept();
    }
}

