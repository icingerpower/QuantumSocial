#ifndef DIALOGPICKINTROVIDEO_H
#define DIALOGPICKINTROVIDEO_H

#include <QDialog>
#include <QList>
#include <QPointer>
#include "model/videogen/AbstractMultiVideoWorkflow.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QLabel;
class QMediaPlayer;
class QAudioOutput;
class QVideoWidget;
class QSpinBox;

class DialogPickIntroVideo : public QDialog
{
    Q_OBJECT

public:
    explicit DialogPickIntroVideo(const QList<AbstractMultiVideoWorkflow::TakeInfo> &introTakes,
                                  int defaultSelectedIndex = 0,
                                  QWidget *parent = nullptr);
    explicit DialogPickIntroVideo(int stepIndex,
                                  const QString &stepTitle,
                                  const QList<AbstractMultiVideoWorkflow::TakeInfo> &takes,
                                  int defaultSelectedIndex = 0,
                                  QWidget *parent = nullptr);
    ~DialogPickIntroVideo() override;

    int selectedTakeIndex() const;
    int stepIndex() const { return m_stepIndex; }
    bool isRegenerateRequested() const { return m_regenerateRequested; }
    int regenerateCount() const { return m_regenerateCount; }
    bool isResetRequested() const { return m_resetRequested; }

private slots:
    void _onTakeSelectionChanged();
    void _togglePlayPause();
    void _onPositionChanged(qint64 position);
    void _onDurationChanged(qint64 duration);
    void _onSliderMoved(int position);
    void _onGenerateAgainClicked();
    void _onResetStepClicked();

private:
    int m_stepIndex = 0;
    QString m_stepTitle;
    bool m_regenerateRequested = false;
    int m_regenerateCount = 1;
    bool m_resetRequested = false;
    QList<AbstractMultiVideoWorkflow::TakeInfo> m_introTakes;
    QListWidget *m_listTakes = nullptr;
    QVideoWidget *m_videoWidget = nullptr;
    QMediaPlayer *m_mediaPlayer = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QPushButton *m_buttonPlayPause = nullptr;
    QSlider *m_slider = nullptr;
    QLabel *m_labelTime = nullptr;
    QSpinBox *m_spinTakes = nullptr;
    QPushButton *m_buttonGenerateAgain = nullptr;
    QPushButton *m_buttonResetStep = nullptr;
    QPushButton *m_buttonContinue = nullptr;
    QPushButton *m_buttonPause = nullptr;
    int m_selectedTakeIndex = 0;
};

#endif // DIALOGPICKINTROVIDEO_H
