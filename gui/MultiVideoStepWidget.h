#ifndef MULTIVIDEOSTEPWIDGET_H
#define MULTIVIDEOSTEPWIDGET_H

#include <QGroupBox>
#include <QList>

#include "model/videogen/AbstractMultiVideoWorkflow.h"

class QLabel;
class QPushButton;
class QComboBox;

// UI widget representing one step in a multi-video project (Intro or After-intro).
// Allows choosing up to 4 images for the step, each with its own regeneration option:
// - Keep original
// - Regenerate with CLI
// - Isolate on white
class MultiVideoStepWidget : public QGroupBox
{
    Q_OBJECT

public:
    static constexpr int MAX_IMAGES_PER_STEP = 4;

    explicit MultiVideoStepWidget(int stepIndex, bool isIntro, QWidget *parent = nullptr);
    ~MultiVideoStepWidget() override = default;

    int stepIndex() const { return m_stepIndex; }
    bool isIntro() const { return m_isIntro; }

    QList<AbstractMultiVideoWorkflow::ImageInput> imageInputs() const;
    void setImage(int slotIndex, const QString &path,
                  AbstractMultiVideoWorkflow::ImageRegenOption option
                      = AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite);

    void setSlotRegenOption(int slotIndex, AbstractMultiVideoWorkflow::ImageRegenOption option);
    void setAllSlotsRegenOption(AbstractMultiVideoWorkflow::ImageRegenOption option);
    void setDefaultRegenOption(AbstractMultiVideoWorkflow::ImageRegenOption option);
    AbstractMultiVideoWorkflow::ImageRegenOption defaultRegenOption() const { return m_defaultRegenOption; }

signals:
    void changed();

private slots:
    void _browseSlot(int slotIndex);
    void _clearSlot(int slotIndex);

private:
    struct SlotUi
    {
        QLabel *labelPreview = nullptr;
        QLabel *labelPath = nullptr;
        QPushButton *buttonBrowse = nullptr;
        QPushButton *buttonClear = nullptr;
        QComboBox *comboRegen = nullptr;
        QString imagePath;
    };

    int m_stepIndex = 0;
    bool m_isIntro = false;
    AbstractMultiVideoWorkflow::ImageRegenOption m_defaultRegenOption
        = AbstractMultiVideoWorkflow::ImageRegenOption::IsolateOnWhite;
    SlotUi m_slots[MAX_IMAGES_PER_STEP];

    void _updateSlotPreview(int slotIndex);
};

#endif // MULTIVIDEOSTEPWIDGET_H
