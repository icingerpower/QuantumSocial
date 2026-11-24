#ifndef WIDGETCREATIVEREVIEW_H
#define WIDGETCREATIVEREVIEW_H

#include "panes/PanePublishing.h"

#include <QList>
#include <QPixmap>
#include <QString>
#include <QWidget>

class QAudioOutput;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QHideEvent;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTableWidgetItem;
class QVideoWidget;
class TableProjects;

class WidgetCreativeReview : public QWidget
{
    Q_OBJECT

public:
    explicit WidgetCreativeReview(QWidget *parent = nullptr);
    ~WidgetCreativeReview() override;

    void setDeleteButtonVisible(bool visible);
    void setBannerText(const QString &html);

    void setCandidates(const PanePublishing::PublishJobConfig &config,
                       const QList<PanePublishing::EligibleCreative> &allCandidates,
                       const QList<PanePublishing::ScheduledItem> &initialPlannedQueue,
                       int baseIntervalMins,
                       const QString &experimentSummaryLine = QString{},
                       TableProjects *projects = nullptr);

    void updateScheduleConfig(const PanePublishing::PublishJobConfig &config);
    bool removeCandidateByShortCode(const QString &shortCode);

    QList<PanePublishing::ScheduledItem> buildConfirmedQueue() const;

    int checkedCount() const;
    int totalCount() const { return int(m_rows.size()); }
    QString currentShortCode() const;
    void stopPlayback();

signals:
    void selectionChanged(int checkedCount, int totalCount);
    void deleteCandidateRequested(const QString &shortCode);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    struct CandidateRowState
    {
        PanePublishing::EligibleCreative creative;
        bool checked = false;
        int aiInitialRank = 0; // 1..K if AI selected, 0 if not initially picked
        bool addVideoText = false;
        bool aiSuggestedVideoText = false;
        QString videoOverlayText;
        QString videoTextStyle;
        QString aiReason;
        int targetIndex = 0;
        int customDelayMinutes = 180;
        QPixmap rowThumb;
    };

    static void _makeBadgeInfo(const PanePublishing::EligibleCreative &c,
                               QString *outBadge,
                               QColor *outColor);
    static QPixmap _makeRowThumb(const CandidateRowState &r);
    int _checkedOrderOfRow(int targetRow) const;
    void _stopInlineVideo();
    void _startInlineVideoForRow(int row);
    void _refreshInspector(int row);
    void _rebuildTable(int selectRow);

    PanePublishing::PublishJobConfig m_config;
    QList<CandidateRowState> m_initialRows;
    QList<CandidateRowState> m_rows;

    QLabel *m_banner = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_btnToggleCheck = nullptr;
    QPushButton *m_btnMoveUp = nullptr;
    QPushButton *m_btnMoveDown = nullptr;
    QPushButton *m_btnMoveTop = nullptr;
    QPushButton *m_btnGroupTop = nullptr;
    QPushButton *m_btnResetAi = nullptr;
    QPushButton *m_btnDeleteCreative = nullptr;
    QPushButton *m_btnDeleteInspector = nullptr;
    QLabel *m_summaryLbl = nullptr;

    QStackedWidget *m_previewStack = nullptr;
    QLabel *m_largePreviewLbl = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QMediaPlayer *m_mediaPlayer = nullptr;

    QWidget *m_videoTransportWidget = nullptr;
    QPushButton *m_btnPlayPauseVideo = nullptr;
    QLabel *m_videoTimeLbl = nullptr;
    QPushButton *m_btnMuteVideo = nullptr;
    QSlider *m_videoPosSlider = nullptr;

    QWidget *m_slideStripWidget = nullptr;
    QHBoxLayout *m_slideStripLayout = nullptr;
    QPushButton *m_btnOpenMedia = nullptr;

    QComboBox *m_targetCombo = nullptr;
    QSpinBox *m_delaySpin = nullptr;

    QWidget *m_videoTextBox = nullptr;
    QCheckBox *m_chkVideoText = nullptr;
    QComboBox *m_comboVideoStyle = nullptr;
    QLineEdit *m_editVideoText = nullptr;
    QPlainTextEdit *m_detailsBox = nullptr;

    bool m_updatingUi = false;
    bool m_scrubbingSlider = false;
    bool m_autoPlayVideosOnSelect = false;
    bool m_deleteEnabled = false;
    QString m_loadedVideoPath;
};

#endif // WIDGETCREATIVEREVIEW_H
