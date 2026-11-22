#include "WidgetCreativeReview.h"

#include "model/TableProjects.h"

#include <QAbstractItemView>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>

namespace {

QString formatMsMmSs(qint64 ms)
{
    const qint64 totalSecs = qMax<qint64>(0, ms / 1000);
    return QStringLiteral("%1:%2")
        .arg(totalSecs / 60)
        .arg(totalSecs % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

WidgetCreativeReview::WidgetCreativeReview(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_banner = new QLabel(this);
    m_banner->setObjectName(QStringLiteral("labelReviewBanner"));
    m_banner->setWordWrap(true);
    mainLayout->addWidget(m_banner);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter, 1);

    // Left panel: Candidate table + ordering toolbar
    auto *leftBox = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftBox);
    leftLayout->setContentsMargins(0, 0, 4, 0);

    m_table = new QTableWidget(0, 4, leftBox);
    m_table->setObjectName(QStringLiteral("tableReviewCandidates"));
    m_table->setHorizontalHeaderLabels({
        tr("Publish / Order"),
        tr("Preview"),
        tr("Creative Details (Product, Hook & AI Note)"),
        tr("Target & Timing")
    });
    m_table->setIconSize(QSize(72, 112));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(122);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_table->setColumnWidth(1, 86);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_table->horizontalHeader()->resizeSection(3, 195);
    leftLayout->addWidget(m_table, 1);

    auto *orderBar = new QHBoxLayout;
    m_btnToggleCheck = new QPushButton(tr("Check / Uncheck"), leftBox);
    m_btnToggleCheck->setObjectName(QStringLiteral("buttonToggleCandidateCheck"));
    m_btnMoveUp = new QPushButton(tr("▲ Move Up"), leftBox);
    m_btnMoveUp->setObjectName(QStringLiteral("buttonMoveCandidateUp"));
    m_btnMoveDown = new QPushButton(tr("▼ Move Down"), leftBox);
    m_btnMoveDown->setObjectName(QStringLiteral("buttonMoveCandidateDown"));
    m_btnMoveTop = new QPushButton(tr("⤒ Move to #1"), leftBox);
    m_btnMoveTop->setObjectName(QStringLiteral("buttonMoveCandidateTop"));
    m_btnGroupTop = new QPushButton(tr("Group Selected at Top"), leftBox);
    m_btnGroupTop->setObjectName(QStringLiteral("buttonSortCheckedTop"));
    m_btnResetAi = new QPushButton(tr("↺ Reset to AI Plan"), leftBox);
    m_btnResetAi->setObjectName(QStringLiteral("buttonResetAiSelection"));
    m_btnDeleteCreative = new QPushButton(tr("🗑 Delete"), leftBox);
    m_btnDeleteCreative->setObjectName(QStringLiteral("buttonDeleteCandidateCreative"));
    m_btnDeleteCreative->setToolTip(tr("Permanently delete this unpublished creative from disk."));
    m_btnDeleteCreative->setStyleSheet(QStringLiteral("color: #e89774;"));
    m_btnDeleteCreative->setVisible(false);

    orderBar->addWidget(m_btnToggleCheck);
    orderBar->addWidget(m_btnMoveUp);
    orderBar->addWidget(m_btnMoveDown);
    orderBar->addWidget(m_btnMoveTop);
    orderBar->addWidget(m_btnGroupTop);
    orderBar->addWidget(m_btnResetAi);
    orderBar->addWidget(m_btnDeleteCreative);
    orderBar->addStretch();

    m_summaryLbl = new QLabel(leftBox);
    m_summaryLbl->setObjectName(QStringLiteral("labelSelectedPublishSummary"));
    orderBar->addWidget(m_summaryLbl);
    leftLayout->addLayout(orderBar);
    splitter->addWidget(leftBox);

    // Right panel: Large visual preview + carousel slide switcher + full creative details & slot controls
    auto *rightBox = new QWidget(splitter);
    rightBox->setMinimumWidth(310);
    auto *rightLayout = new QVBoxLayout(rightBox);
    rightLayout->setContentsMargins(6, 0, 0, 0);

    auto *previewTitle = new QLabel(tr("Selected Candidate Preview & Details:"), rightBox);
    previewTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    rightLayout->addWidget(previewTitle);

    m_previewStack = new QStackedWidget(rightBox);
    m_previewStack->setObjectName(QStringLiteral("stackedReviewPreview"));
    m_previewStack->setFixedSize(236, 360);

    m_largePreviewLbl = new QLabel(m_previewStack);
    m_largePreviewLbl->setObjectName(QStringLiteral("labelReviewLargePreview"));
    m_largePreviewLbl->setAlignment(Qt::AlignCenter);
    m_largePreviewLbl->setMinimumSize(236, 360);
    m_largePreviewLbl->setStyleSheet(QStringLiteral(
        "QLabel { background: #141821; border: 1px solid #2b3245; border-radius: 10px; padding: 4px; }"));
    m_previewStack->addWidget(m_largePreviewLbl);

    m_videoWidget = new QVideoWidget(m_previewStack);
    m_videoWidget->setObjectName(QStringLiteral("videoReviewPlayerWidget"));
    m_videoWidget->setAspectRatioMode(Qt::KeepAspectRatio);
    m_videoWidget->setStyleSheet(QStringLiteral("background-color: #0d1117; border-radius: 10px;"));
    m_previewStack->addWidget(m_videoWidget);

    rightLayout->addWidget(m_previewStack, 0, Qt::AlignHCenter);

    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(0.8f);
    m_mediaPlayer = new QMediaPlayer(this);
    m_mediaPlayer->setObjectName(QStringLiteral("playerReviewVideo"));
    m_mediaPlayer->setAudioOutput(m_audioOutput);
    m_mediaPlayer->setVideoOutput(m_videoWidget);
    m_mediaPlayer->setLoops(QMediaPlayer::Infinite);

    m_videoTransportWidget = new QWidget(rightBox);
    m_videoTransportWidget->setObjectName(QStringLiteral("widgetVideoTransportBar"));
    auto *videoTransportLayout = new QVBoxLayout(m_videoTransportWidget);
    videoTransportLayout->setContentsMargins(0, 2, 0, 2);
    videoTransportLayout->setSpacing(4);

    auto *videoBtnRow = new QHBoxLayout;
    m_btnPlayPauseVideo = new QPushButton(tr("▶ Play Video"), m_videoTransportWidget);
    m_btnPlayPauseVideo->setObjectName(QStringLiteral("buttonPlayPauseCandidateVideo"));
    m_btnPlayPauseVideo->setStyleSheet(QStringLiteral("QPushButton { font-weight: 600; padding: 4px 10px; }"));
    m_videoTimeLbl = new QLabel(QStringLiteral("0:00 / 0:00"), m_videoTransportWidget);
    m_videoTimeLbl->setObjectName(QStringLiteral("labelCandidateVideoTime"));
    m_videoTimeLbl->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    m_btnMuteVideo = new QPushButton(tr("🔊"), m_videoTransportWidget);
    m_btnMuteVideo->setObjectName(QStringLiteral("buttonMuteCandidateVideo"));
    m_btnMuteVideo->setCheckable(true);
    m_btnMuteVideo->setFixedWidth(34);
    m_btnMuteVideo->setToolTip(tr("Mute / unmute preview audio"));

    videoBtnRow->addWidget(m_btnPlayPauseVideo, 1);
    videoBtnRow->addWidget(m_videoTimeLbl);
    videoBtnRow->addWidget(m_btnMuteVideo);
    videoTransportLayout->addLayout(videoBtnRow);

    m_videoPosSlider = new QSlider(Qt::Horizontal, m_videoTransportWidget);
    m_videoPosSlider->setObjectName(QStringLiteral("sliderCandidateVideoPosition"));
    m_videoPosSlider->setRange(0, 0);
    videoTransportLayout->addWidget(m_videoPosSlider);
    m_videoTransportWidget->setVisible(false);
    rightLayout->addWidget(m_videoTransportWidget);

    m_slideStripWidget = new QWidget(rightBox);
    m_slideStripWidget->setObjectName(QStringLiteral("widgetReviewSlideStrip"));
    m_slideStripLayout = new QHBoxLayout(m_slideStripWidget);
    m_slideStripLayout->setContentsMargins(0, 2, 0, 2);
    m_slideStripLayout->setSpacing(4);
    m_slideStripWidget->setVisible(false);
    rightLayout->addWidget(m_slideStripWidget);

    m_btnOpenMedia = new QPushButton(tr("Open Media File Externally"), rightBox);
    m_btnOpenMedia->setObjectName(QStringLiteral("buttonOpenCandidateMedia"));
    rightLayout->addWidget(m_btnOpenMedia);

    auto *slotForm = new QFormLayout;
    m_targetCombo = new QComboBox(rightBox);
    m_targetCombo->setObjectName(QStringLiteral("comboReviewCandidateTarget"));
    slotForm->addRow(tr("Target account:"), m_targetCombo);

    m_delaySpin = new QSpinBox(rightBox);
    m_delaySpin->setObjectName(QStringLiteral("spinReviewCandidateDelay"));
    m_delaySpin->setRange(0, 4320);
    m_delaySpin->setSuffix(tr(" min after prev"));
    slotForm->addRow(tr("Spacing delay:"), m_delaySpin);
    rightLayout->addLayout(slotForm);

    m_videoTextBox = new QWidget(rightBox);
    m_videoTextBox->setObjectName(QStringLiteral("widgetReviewVideoTextControls"));
    auto *videoTextLayout = new QVBoxLayout(m_videoTextBox);
    videoTextLayout->setContentsMargins(0, 2, 0, 4);
    videoTextLayout->setSpacing(4);

    m_chkVideoText = new QCheckBox(tr("Add native TikTok text over video (\"Aa Texte\")"), m_videoTextBox);
    m_chkVideoText->setObjectName(QStringLiteral("checkReviewVideoTextOverlay"));
    m_chkVideoText->setToolTip(tr("Uses TikTok Studio's native 'Aa Texte' track in the upper safe zone so TikTok indexes it for SEO while keeping the shoes unobstructed."));
    videoTextLayout->addWidget(m_chkVideoText);

    m_comboVideoStyle = new QComboBox(m_videoTextBox);
    m_comboVideoStyle->setObjectName(QStringLiteral("comboReviewVideoTextStyle"));
    for (const QString &sid : PanePublishing::curatedVideoTextStyleIds())
    {
        m_comboVideoStyle->addItem(PanePublishing::videoTextStyleDisplayName(sid), sid);
    }
    m_comboVideoStyle->setToolTip(tr("Choose from 4 fashion-safe native TikTok Studio text styles (AI selects & A/B-tests performance over time)."));
    videoTextLayout->addWidget(m_comboVideoStyle);

    m_editVideoText = new QLineEdit(m_videoTextBox);
    m_editVideoText->setObjectName(QStringLiteral("editReviewVideoOverlayText"));
    m_editVideoText->setMaxLength(80);
    m_editVideoText->setPlaceholderText(tr("Short 4–9 word hook overlaid near top of video..."));
    videoTextLayout->addWidget(m_editVideoText);
    m_videoTextBox->setVisible(false);
    rightLayout->addWidget(m_videoTextBox);

    m_detailsBox = new QPlainTextEdit(rightBox);
    m_detailsBox->setObjectName(QStringLiteral("textReviewCandidateDetails"));
    m_detailsBox->setReadOnly(true);
    m_detailsBox->setMinimumHeight(130);
    m_detailsBox->setStyleSheet(QStringLiteral("QPlainTextEdit { font-size: 12px; }"));
    rightLayout->addWidget(m_detailsBox, 1);

    splitter->addWidget(rightBox);
    splitter->setSizes({760, 340});

    connect(m_mediaPlayer, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
        m_btnPlayPauseVideo->setText(state == QMediaPlayer::PlayingState
            ? tr("⏸ Pause")
            : tr("▶ Play Video"));
    });

    connect(m_mediaPlayer, &QMediaPlayer::durationChanged, this, [this](qint64 dur) {
        m_videoPosSlider->setRange(0, int(qMax<qint64>(0, dur)));
        m_videoTimeLbl->setText(QStringLiteral("%1 / %2")
            .arg(formatMsMmSs(m_mediaPlayer->position()), formatMsMmSs(dur)));
    });

    connect(m_mediaPlayer, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        if (!m_scrubbingSlider)
        {
            m_videoPosSlider->setValue(int(qMax<qint64>(0, pos)));
        }
        m_videoTimeLbl->setText(QStringLiteral("%1 / %2")
            .arg(formatMsMmSs(pos), formatMsMmSs(m_mediaPlayer->duration())));
    });

    connect(m_videoPosSlider, &QSlider::sliderPressed, this, [this]() { m_scrubbingSlider = true; });
    connect(m_videoPosSlider, &QSlider::sliderReleased, this, [this]() {
        m_scrubbingSlider = false;
        m_mediaPlayer->setPosition(m_videoPosSlider->value());
    });
    connect(m_videoPosSlider, &QSlider::sliderMoved, this, [this](int val) {
        m_mediaPlayer->setPosition(val);
    });

    connect(m_btnPlayPauseVideo, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r < 0 || r >= m_rows.size() || m_rows[r].creative.format != QLatin1String("video"))
        {
            return;
        }
        if (m_mediaPlayer->playbackState() == QMediaPlayer::PlayingState)
        {
            m_mediaPlayer->pause();
            m_autoPlayVideosOnSelect = false;
        }
        else
        {
            _startInlineVideoForRow(r);
        }
    });

    connect(m_btnMuteVideo, &QPushButton::toggled, this, [this](bool muted) {
        m_audioOutput->setMuted(muted);
        m_btnMuteVideo->setText(muted ? tr("🔇") : tr("🔊"));
    });

    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *changed) {
        if (m_updatingUi || !changed || changed->column() != 0)
        {
            return;
        }
        const int r = changed->row();
        if (r < 0 || r >= m_rows.size())
        {
            return;
        }
        m_rows[r].checked = (changed->checkState() == Qt::Checked);
        _rebuildTable(r);
    });

    connect(m_table, &QTableWidget::currentCellChanged, this,
            [this](int currentRow, int, int, int) {
        if (m_updatingUi)
        {
            return;
        }
        m_btnMoveUp->setEnabled(currentRow > 0);
        m_btnMoveDown->setEnabled(currentRow >= 0 && currentRow + 1 < m_rows.size());
        m_btnMoveTop->setEnabled(currentRow > 0);
        m_btnToggleCheck->setEnabled(currentRow >= 0);
        m_btnDeleteCreative->setEnabled(currentRow >= 0 && currentRow < m_rows.size());
        _refreshInspector(currentRow);
    });

    connect(m_btnToggleCheck, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size())
        {
            m_rows[r].checked = !m_rows[r].checked;
            _rebuildTable(r);
        }
    });

    connect(m_btnMoveUp, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r > 0 && r < m_rows.size())
        {
            m_rows.swapItemsAt(r, r - 1);
            _rebuildTable(r - 1);
        }
    });

    connect(m_btnMoveDown, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r >= 0 && r + 1 < m_rows.size())
        {
            m_rows.swapItemsAt(r, r + 1);
            _rebuildTable(r + 1);
        }
    });

    connect(m_btnMoveTop, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r > 0 && r < m_rows.size())
        {
            CandidateRowState moved = m_rows.takeAt(r);
            moved.checked = true;
            m_rows.prepend(moved);
            _rebuildTable(0);
        }
    });

    connect(m_btnGroupTop, &QPushButton::clicked, this, [this]() {
        std::stable_partition(m_rows.begin(), m_rows.end(),
                              [](const CandidateRowState &st) { return st.checked; });
        _rebuildTable(0);
    });

    connect(m_btnResetAi, &QPushButton::clicked, this, [this]() {
        m_rows = m_initialRows;
        _rebuildTable(0);
    });

    connect(m_btnDeleteCreative, &QPushButton::clicked, this, [this]() {
        const QString code = currentShortCode();
        if (!code.isEmpty())
        {
            emit deleteCandidateRequested(code);
        }
    });

    connect(m_targetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_updatingUi)
        {
            return;
        }
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size() && idx >= 0 && idx < m_config.targets.size())
        {
            m_rows[r].targetIndex = idx;
            _rebuildTable(r);
        }
    });

    connect(m_delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int val) {
        if (m_updatingUi)
        {
            return;
        }
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size() && _checkedOrderOfRow(r) != 1)
        {
            m_rows[r].customDelayMinutes = qMax(1, val);
            _rebuildTable(r);
        }
    });

    connect(m_chkVideoText, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updatingUi)
        {
            return;
        }
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size() && m_rows[r].creative.format == QLatin1String("video"))
        {
            m_rows[r].addVideoText = on;
            if (on && m_rows[r].videoOverlayText.trimmed().isEmpty())
            {
                m_rows[r].videoOverlayText = PanePublishing::cleanShortHookForOverlay(
                    m_rows[r].creative.hook, m_rows[r].creative.productSummary);
            }
            m_rows[r].rowThumb = _makeRowThumb(m_rows[r]);
            _rebuildTable(r);
        }
    });

    connect(m_comboVideoStyle, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_updatingUi || idx < 0)
        {
            return;
        }
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size() && m_rows[r].creative.format == QLatin1String("video"))
        {
            m_rows[r].videoTextStyle = PanePublishing::normalizeVideoTextStyleId(
                m_comboVideoStyle->itemData(idx).toString());
            m_rows[r].rowThumb = _makeRowThumb(m_rows[r]);
            _rebuildTable(r);
        }
    });

    connect(m_editVideoText, &QLineEdit::textEdited, this, [this](const QString &txt) {
        if (m_updatingUi)
        {
            return;
        }
        const int r = m_table->currentRow();
        if (r >= 0 && r < m_rows.size() && m_rows[r].creative.format == QLatin1String("video"))
        {
            m_rows[r].videoOverlayText = txt;
            m_rows[r].rowThumb = _makeRowThumb(m_rows[r]);
            if (auto *thumbCell = m_table->item(r, 1))
            {
                thumbCell->setData(Qt::DecorationRole, m_rows[r].rowThumb);
            }
            QString badgeTxt;
            QColor badgeCol;
            _makeBadgeInfo(m_rows[r].creative, &badgeTxt, &badgeCol);
            m_largePreviewLbl->setPixmap(PanePublishing::renderCreativeCardPixmap(
                m_rows[r].creative.previewImagePath,
                m_rows[r].creative.shortCode,
                m_rows[r].creative.productSummary,
                badgeTxt,
                badgeCol,
                QSize(228, 352),
                m_rows[r].addVideoText ? m_rows[r].videoOverlayText : QString{},
                m_rows[r].videoTextStyle));
            const auto &c = m_rows[r].creative;
            const QString hookSnippet = !c.hook.trimmed().isEmpty()
                ? QStringLiteral("\"%1\"").arg(c.hook.trimmed().left(85))
                : tr("(Auto-caption + #hashtags on publish)");
            const QString aiTag = m_rows[r].aiInitialRank > 0
                ? tr("★ AI Pick #%1: %2").arg(m_rows[r].aiInitialRank).arg(m_rows[r].aiReason)
                : m_rows[r].aiReason;
            const QString videoTextRowNote = m_rows[r].addVideoText
                ? tr("\n📝 TikTok Text [%1]: \"%2\"")
                      .arg(PanePublishing::videoTextStyleShortTag(m_rows[r].videoTextStyle),
                           m_rows[r].videoOverlayText.left(60))
                : tr("\n🎬 Clean Video (no TikTok text overlay)");
            if (auto *detCell = m_table->item(r, 2))
            {
                detCell->setText(QStringLiteral("%1 · VIDEO · %2d old\n%3\nHook: %4%6\n%5")
                    .arg(c.shortCode)
                    .arg(c.ageDays)
                    .arg(c.productSummary, hookSnippet, aiTag, videoTextRowNote));
            }
            emit selectionChanged(checkedCount(), int(m_rows.size()));
        }
    });

    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int r, int col) {
        if (col == 0 || r < 0 || r >= m_rows.size() || m_rows[r].creative.format != QLatin1String("video"))
        {
            return;
        }
        m_btnPlayPauseVideo->click();
    });

    connect(m_btnOpenMedia, &QPushButton::clicked, this, [this]() {
        const int r = m_table->currentRow();
        if (r < 0 || r >= m_rows.size() || m_rows[r].creative.mediaPaths.isEmpty())
        {
            return;
        }
        const auto &c = m_rows[r].creative;
        const QString pathToOpen = c.format == QLatin1String("video")
            ? c.mediaPaths.first()
            : QFileInfo{c.mediaPaths.first()}.absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(pathToOpen));
    });
}

WidgetCreativeReview::~WidgetCreativeReview()
{
    stopPlayback();
}

void WidgetCreativeReview::setDeleteButtonVisible(bool visible)
{
    m_btnDeleteCreative->setVisible(visible);
}

void WidgetCreativeReview::setBannerText(const QString &html)
{
    m_banner->setText(html);
    m_banner->setVisible(!html.trimmed().isEmpty());
}

void WidgetCreativeReview::_makeBadgeInfo(const PanePublishing::EligibleCreative &c,
                                          QString *outBadge,
                                          QColor *outColor)
{
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
}

QPixmap WidgetCreativeReview::_makeRowThumb(const CandidateRowState &r)
{
    QString badgeTxt;
    QColor badgeCol;
    _makeBadgeInfo(r.creative, &badgeTxt, &badgeCol);
    const QString overlayPreview = (r.creative.format == QLatin1String("video") && r.addVideoText)
        ? r.videoOverlayText
        : QString{};
    return PanePublishing::renderCreativeCardPixmap(
        r.creative.previewImagePath,
        r.creative.shortCode,
        r.creative.productSummary,
        badgeTxt,
        badgeCol,
        QSize(72, 112),
        overlayPreview,
        r.videoTextStyle);
}

void WidgetCreativeReview::setCandidates(
    const PanePublishing::PublishJobConfig &config,
    const QList<PanePublishing::EligibleCreative> &allCandidates,
    const QList<PanePublishing::ScheduledItem> &initialPlannedQueue,
    int baseIntervalMins,
    const QString &experimentSummaryLine,
    TableProjects *projects)
{
    stopPlayback();
    m_config = config;
    if (m_config.targets.isEmpty())
    {
        m_config.targets.append(PanePublishing::AccountTarget{
            QStringLiteral("tiktok"), QStringLiteral("TikTok"), QStringLiteral("(Select account in Step 3)"), -1});
    }

    m_updatingUi = true;
    m_targetCombo->clear();
    for (const auto &t : m_config.targets)
    {
        m_targetCombo->addItem(QStringLiteral("%1 — %2").arg(t.platformName, t.accountUrl));
    }
    m_targetCombo->setEnabled(m_config.targets.size() > 1);
    m_updatingUi = false;

    QList<PanePublishing::EligibleCreative> enrichedCandidates = allCandidates;
    QHash<QString, int> candIdxByCode;
    for (int i = 0; i < enrichedCandidates.size(); ++i)
    {
        auto &c = enrichedCandidates[i];
        if (c.previewImagePath.isEmpty() && projects && c.projectRow >= 0)
        {
            const QDir genDir = projects->generationDir(c.projectRow, c.shortCode, false);
            const QDir tempDir = projects->generationTempDir(c.projectRow, c.shortCode);
            c.previewImagePath = PanePublishing::resolveCreativePreviewImagePath(
                genDir, tempDir, c.format, c.mediaPaths, true);
        }
        candIdxByCode.insert(c.shortCode.toUpper(), i);
    }

    QList<CandidateRowState> builtRows;
    QSet<QString> seenCodes;

    for (int i = 0; i < initialPlannedQueue.size(); ++i)
    {
        const auto &scheduled = initialPlannedQueue[i];
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
        row.videoTextStyle = PanePublishing::normalizeVideoTextStyleId(scheduled.videoTextStyle);
        row.videoOverlayText = !scheduled.videoOverlayText.isEmpty()
            ? scheduled.videoOverlayText
            : PanePublishing::cleanShortHookForOverlay(row.creative.hook, row.creative.productSummary);
        row.aiReason = scheduled.aiReason;
        row.targetIndex = 0;
        for (int t = 0; t < m_config.targets.size(); ++t)
        {
            if (m_config.targets[t].accountUrl == scheduled.target.accountUrl
                && m_config.targets[t].platformId == scheduled.target.platformId)
            {
                row.targetIndex = t;
                break;
            }
        }
        row.customDelayMinutes = scheduled.delayMinutesFromPrevious > 0
            ? scheduled.delayMinutesFromPrevious
            : qMax(5, baseIntervalMins);
        row.rowThumb = _makeRowThumb(row);
        builtRows.append(row);
    }

    int extraIdx = 0;
    for (const auto &c : enrichedCandidates)
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
            const auto &styles = PanePublishing::curatedVideoTextStyleIds();
            row.videoTextStyle = styles[extraIdx % styles.size()];
            row.videoOverlayText = PanePublishing::cleanShortHookForOverlay(c.hook, c.productSummary);
        }
        row.aiReason = tr("Available unpublished candidate");
        row.targetIndex = (initialPlannedQueue.size() + extraIdx) % m_config.targets.size();
        if (m_config.spacingMode == PanePublishing::SpacingMode::DaytimeSpread12h)
        {
            const int maxJitter = qMin(15, qMax(2, baseIntervalMins / 5));
            const int jitter = (((initialPlannedQueue.size() + extraIdx + 1) * 37) % (2 * maxJitter + 1)) - maxJitter;
            row.customDelayMinutes = qMax(15, baseIntervalMins + jitter);
        }
        else
        {
            row.customDelayMinutes = qMax(5, baseIntervalMins);
        }
        ++extraIdx;
        row.rowThumb = _makeRowThumb(row);
        builtRows.append(row);
    }

    m_initialRows = builtRows;
    m_rows = builtRows;

    QString bannerHtml = tr(
        "Selected <b>%1</b> of <b>%2</b> eligible creative(s) in publish order. "
        "Check/uncheck candidates, reorder with <b>Move Up / Move Down / Move to #1</b>, and customize "
        "each video's optional native TikTok text overlay (<i>Aa Texte</i>).")
            .arg(initialPlannedQueue.size())
            .arg(m_rows.size());
    if (!experimentSummaryLine.trimmed().isEmpty())
    {
        bannerHtml += QStringLiteral("<br><span style='color:#93c5fd; font-size:11px;'>📊 %1</span>")
            .arg(experimentSummaryLine.toHtmlEscaped());
    }
    setBannerText(bannerHtml);

    _rebuildTable(0);
}

void WidgetCreativeReview::updateScheduleConfig(const PanePublishing::PublishJobConfig &config)
{
    m_config = config;
    if (m_config.targets.isEmpty())
    {
        m_config.targets.append(PanePublishing::AccountTarget{
            QStringLiteral("tiktok"), QStringLiteral("TikTok"), QStringLiteral("(Select account in Step 3)"), -1});
    }

    m_updatingUi = true;
    m_targetCombo->clear();
    for (const auto &t : m_config.targets)
    {
        m_targetCombo->addItem(QStringLiteral("%1 — %2").arg(t.platformName, t.accountUrl));
    }
    m_targetCombo->setEnabled(m_config.targets.size() > 1);
    m_updatingUi = false;

    const int baseMins = PanePublishing::baseIntervalMinutesForConfig(m_config);
    for (int i = 0; i < m_rows.size(); ++i)
    {
        m_rows[i].targetIndex = qBound(0, m_rows[i].targetIndex, int(m_config.targets.size()) - 1);
        if (m_config.spacingMode == PanePublishing::SpacingMode::DaytimeSpread12h)
        {
            const int maxJitter = qMin(15, qMax(2, baseMins / 5));
            const int jitter = (((i + 1) * 37) % (2 * maxJitter + 1)) - maxJitter;
            m_rows[i].customDelayMinutes = qMax(15, baseMins + jitter);
        }
        else
        {
            m_rows[i].customDelayMinutes = qMax(5, baseMins);
        }
    }

    _rebuildTable(m_table->currentRow());
}

bool WidgetCreativeReview::removeCandidateByShortCode(const QString &shortCode)
{
    const QString upper = shortCode.trimmed().toUpper();
    if (upper.isEmpty())
    {
        return false;
    }
    const int curRow = m_table->currentRow();
    stopPlayback();

    bool removed = false;
    for (int i = int(m_rows.size()) - 1; i >= 0; --i)
    {
        if (m_rows[i].creative.shortCode.compare(upper, Qt::CaseInsensitive) == 0)
        {
            m_rows.removeAt(i);
            removed = true;
        }
    }
    for (int i = int(m_initialRows.size()) - 1; i >= 0; --i)
    {
        if (m_initialRows[i].creative.shortCode.compare(upper, Qt::CaseInsensitive) == 0)
        {
            m_initialRows.removeAt(i);
        }
    }
    if (removed)
    {
        _rebuildTable(curRow);
    }
    return removed;
}

QList<PanePublishing::ScheduledItem> WidgetCreativeReview::buildConfirmedQueue() const
{
    QList<PanePublishing::ScheduledItem> finalQueue;
    if (m_config.targets.isEmpty())
    {
        return finalQueue;
    }
    QDateTime cursorTime = QDateTime::currentDateTime();
    for (const CandidateRowState &st : m_rows)
    {
        if (!st.checked)
        {
            continue;
        }
        const int delayMins = finalQueue.isEmpty() ? 0 : qMax(1, st.customDelayMinutes);
        cursorTime = cursorTime.addSecs(qint64(delayMins) * 60);

        PanePublishing::ScheduledItem item;
        item.creative = st.creative;
        item.target = m_config.targets[qBound(0, st.targetIndex, int(m_config.targets.size()) - 1)];
        item.delayMinutesFromPrevious = delayMins;
        item.scheduledAt = cursorTime;
        item.addVideoText = (st.creative.format == QLatin1String("video")) && st.addVideoText;
        item.aiSuggestedVideoText = st.aiSuggestedVideoText;
        item.videoTextStyle = PanePublishing::normalizeVideoTextStyleId(st.videoTextStyle);
        item.videoOverlayText = st.videoOverlayText.trimmed();
        item.aiReason = st.aiInitialRank > 0
            ? st.aiReason
            : tr("Selected in review (%1)").arg(st.creative.format);
        item.status = QStringLiteral("Queued");
        finalQueue.append(item);
    }
    return finalQueue;
}

int WidgetCreativeReview::checkedCount() const
{
    int c = 0;
    for (const auto &st : m_rows)
    {
        if (st.checked)
        {
            ++c;
        }
    }
    return c;
}

QString WidgetCreativeReview::currentShortCode() const
{
    const int r = m_table ? m_table->currentRow() : -1;
    if (r < 0 || r >= m_rows.size())
    {
        return QString{};
    }
    return m_rows[r].creative.shortCode;
}

void WidgetCreativeReview::stopPlayback()
{
    _stopInlineVideo();
}

int WidgetCreativeReview::_checkedOrderOfRow(int targetRow) const
{
    if (targetRow < 0 || targetRow >= m_rows.size() || !m_rows[targetRow].checked)
    {
        return 0;
    }
    int ord = 0;
    for (int i = 0; i <= targetRow; ++i)
    {
        if (m_rows[i].checked)
        {
            ++ord;
        }
    }
    return ord;
}

void WidgetCreativeReview::_stopInlineVideo()
{
    if (!m_mediaPlayer)
    {
        return;
    }
    m_mediaPlayer->stop();
    m_mediaPlayer->setSource(QUrl{});
    m_loadedVideoPath.clear();
    if (m_previewStack && m_largePreviewLbl)
    {
        m_previewStack->setCurrentWidget(m_largePreviewLbl);
    }
    if (m_btnPlayPauseVideo)
    {
        m_btnPlayPauseVideo->setText(tr("▶ Play Video"));
    }
    if (m_videoPosSlider)
    {
        m_videoPosSlider->setRange(0, 0);
    }
    if (m_videoTimeLbl)
    {
        m_videoTimeLbl->setText(QStringLiteral("0:00 / 0:00"));
    }
}

void WidgetCreativeReview::_startInlineVideoForRow(int row)
{
    if (row < 0 || row >= m_rows.size())
    {
        return;
    }
    const auto &c = m_rows[row].creative;
    if (c.format != QLatin1String("video") || c.mediaPaths.isEmpty())
    {
        return;
    }
    const QString vPath = c.mediaPaths.first();
    if (m_loadedVideoPath != vPath)
    {
        m_loadedVideoPath = vPath;
        m_mediaPlayer->setSource(QUrl::fromLocalFile(vPath));
    }
    m_previewStack->setCurrentWidget(m_videoWidget);
    m_mediaPlayer->play();
    m_autoPlayVideosOnSelect = true;
}

void WidgetCreativeReview::_refreshInspector(int row)
{
    if (row < 0 || row >= m_rows.size())
    {
        _stopInlineVideo();
        m_largePreviewLbl->setPixmap(QPixmap{});
        m_videoTransportWidget->setVisible(false);
        m_videoTextBox->setVisible(false);
        m_slideStripWidget->setVisible(false);
        m_detailsBox->clear();
        m_btnOpenMedia->setEnabled(false);
        return;
    }

    const bool prevGuard = m_updatingUi;
    m_updatingUi = true;

    const CandidateRowState &st = m_rows[row];
    const auto &c = st.creative;

    QString badgeTxt;
    QColor badgeCol;
    _makeBadgeInfo(c, &badgeTxt, &badgeCol);
    const QString liveOverlayPreview = (c.format == QLatin1String("video") && st.addVideoText)
        ? st.videoOverlayText
        : QString{};
    m_largePreviewLbl->setPixmap(PanePublishing::renderCreativeCardPixmap(
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
        m_videoTransportWidget->setVisible(true);
        m_videoTextBox->setVisible(true);
        const QString aiRecBadge = st.aiSuggestedVideoText
            ? tr(" (AI suggested: ON)")
            : tr(" (AI suggested: OFF)");
        m_chkVideoText->setText(tr("Add TikTok video text") + aiRecBadge);
        m_chkVideoText->setChecked(st.addVideoText);
        const int styleIdx = m_comboVideoStyle->findData(
            PanePublishing::normalizeVideoTextStyleId(st.videoTextStyle));
        if (styleIdx >= 0)
        {
            m_comboVideoStyle->setCurrentIndex(styleIdx);
        }
        m_comboVideoStyle->setEnabled(st.addVideoText);
        if (m_editVideoText->text() != st.videoOverlayText)
        {
            m_editVideoText->setText(st.videoOverlayText);
        }
        m_editVideoText->setEnabled(st.addVideoText);
        if (m_loadedVideoPath != c.mediaPaths.first())
        {
            _stopInlineVideo();
            if (m_autoPlayVideosOnSelect)
            {
                _startInlineVideoForRow(row);
            }
        }
    }
    else
    {
        _stopInlineVideo();
        m_videoTransportWidget->setVisible(false);
        m_videoTextBox->setVisible(false);
    }

    while (QLayoutItem *child = m_slideStripLayout->takeAt(0))
    {
        delete child->widget();
        delete child;
    }
    if (c.format == QLatin1String("carousel") && c.mediaPaths.size() >= 2)
    {
        m_slideStripLayout->addStretch();
        const int maxSlidesShown = qMin<int>(6, c.mediaPaths.size());
        for (int s = 0; s < maxSlidesShown; ++s)
        {
            const QString slidePath = c.mediaPaths[s];
            auto *sBtn = new QPushButton(tr("#%1").arg(s + 1), m_slideStripWidget);
            sBtn->setIcon(QIcon(PanePublishing::renderCreativeCardPixmap(
                slidePath, c.shortCode, QString{}, QString{}, badgeCol, QSize(32, 50))));
            sBtn->setIconSize(QSize(28, 44));
            sBtn->setToolTip(tr("Preview Slide %1 (%2)").arg(s + 1).arg(QFileInfo{slidePath}.fileName()));
            connect(sBtn, &QPushButton::clicked, this,
                    [this, slidePath, s, total = c.mediaPaths.size(), code = c.shortCode, prod = c.productSummary, badgeCol]() {
                m_previewStack->setCurrentWidget(m_largePreviewLbl);
                m_largePreviewLbl->setPixmap(PanePublishing::renderCreativeCardPixmap(
                    slidePath,
                    code,
                    prod,
                    QStringLiteral("SLIDE %1/%2").arg(s + 1).arg(total),
                    badgeCol,
                    QSize(228, 352)));
            });
            m_slideStripLayout->addWidget(sBtn);
        }
        m_slideStripLayout->addStretch();
        m_slideStripWidget->setVisible(true);
    }
    else
    {
        m_slideStripWidget->setVisible(false);
    }

    m_btnOpenMedia->setEnabled(!c.mediaPaths.isEmpty());
    m_btnOpenMedia->setText(c.format == QLatin1String("video")
        ? tr("Open Video Externally (%1)").arg(QFileInfo{c.mediaPaths.first()}.fileName())
        : tr("Open Slide Folder (%1 files)").arg(c.mediaPaths.size()));

    if (!m_config.targets.isEmpty())
    {
        m_targetCombo->setCurrentIndex(qBound(0, st.targetIndex, int(m_config.targets.size()) - 1));
    }
    const int ord = _checkedOrderOfRow(row);
    if (ord == 1)
    {
        m_delaySpin->setValue(0);
        m_delaySpin->setEnabled(false);
        m_delaySpin->setToolTip(tr("Slot #1 always publishes immediately (+0 min)."));
    }
    else
    {
        m_delaySpin->setValue(st.customDelayMinutes);
        m_delaySpin->setEnabled(true);
        m_delaySpin->setToolTip(tr("Minutes to wait after the previous scheduled post."));
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
              .arg(PanePublishing::codeTagFromShortCode(c.shortCode));

    m_detailsBox->setPlainText(
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

    m_updatingUi = prevGuard;
}

void WidgetCreativeReview::_rebuildTable(int selectRow)
{
    m_updatingUi = true;
    m_table->setRowCount(m_rows.size());

    int checked = 0;
    QDateTime cursorTime = QDateTime::currentDateTime();

    for (int r = 0; r < m_rows.size(); ++r)
    {
        const CandidateRowState &st = m_rows[r];
        const auto &c = st.creative;

        int ord = 0;
        int effectiveDelay = 0;
        QDateTime rowScheduledAt;
        if (st.checked)
        {
            ++checked;
            ord = checked;
            effectiveDelay = (ord == 1) ? 0 : qMax(1, st.customDelayMinutes);
            cursorTime = cursorTime.addSecs(qint64(effectiveDelay) * 60);
            rowScheduledAt = cursorTime;
        }

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
        m_table->setItem(r, 0, chkItem);

        auto *thumbItem = new QTableWidgetItem;
        thumbItem->setData(Qt::DecorationRole, st.rowThumb);
        thumbItem->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(r, 1, thumbItem);

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
                      .arg(PanePublishing::videoTextStyleShortTag(st.videoTextStyle),
                           st.videoOverlayText.left(60))
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
        m_table->setItem(r, 2, detailsItem);

        const auto &tgt = m_config.targets[qBound(0, st.targetIndex, int(m_config.targets.size()) - 1)];
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
        m_table->setItem(r, 3, timingItem);
        m_table->setRowHeight(r, 122);
    }

    m_summaryLbl->setText(tr("Selected to publish: %1 of %2 candidate(s)")
        .arg(checked).arg(m_rows.size()));
    m_summaryLbl->setStyleSheet(checked > 0
        ? QStringLiteral("color: #8fd19e; font-weight: 600;")
        : QStringLiteral("color: #e89774; font-weight: 600;"));

    const int clampedSel = m_rows.isEmpty() ? -1 : qBound(0, selectRow, int(m_rows.size()) - 1);
    if (clampedSel >= 0)
    {
        m_table->selectRow(clampedSel);
    }
    m_btnMoveUp->setEnabled(clampedSel > 0);
    m_btnMoveDown->setEnabled(clampedSel >= 0 && clampedSel + 1 < m_rows.size());
    m_btnMoveTop->setEnabled(clampedSel > 0);
    m_btnToggleCheck->setEnabled(clampedSel >= 0);
    m_btnDeleteCreative->setEnabled(clampedSel >= 0 && clampedSel < m_rows.size());

    m_updatingUi = false;
    _refreshInspector(clampedSel);
    emit selectionChanged(checked, int(m_rows.size()));
}
