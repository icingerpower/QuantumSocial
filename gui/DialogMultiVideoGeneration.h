#ifndef DIALOGMULTIVIDEOGENERATION_H
#define DIALOGMULTIVIDEOGENERATION_H

#include <QDialog>
#include <QList>

#include "DialogGenerationOptions.h"
#include "model/videogen/AbstractMultiVideoWorkflow.h"

class AbstractCli;
class MultiVideoStepWidget;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QVBoxLayout;

// Dialog for configuring a multi-video project (1 Intro step + N After-intro steps):
// - Step count picker
// - 1 prompt for Intro + 1 prompt for After-intro steps
// - Takes count for Intro + After-intro steps
// - Up to 4 images per step with individual regeneration strategy
// - CLI selection (Image CLI, Decision CLI) and Video Generator selection
class DialogMultiVideoGeneration : public QDialog
{
    Q_OBJECT

public:
    explicit DialogMultiVideoGeneration(const QList<AbstractCli *> &availableClis,
                                        QWidget *parent = nullptr);
    ~DialogMultiVideoGeneration() override = default;

    QString keyword() const;
    int totalSteps() const;
    QString introPrompt() const;
    int introTakes() const;
    QString afterIntroPrompt() const;
    int afterIntroTakes() const;

    QList<QList<AbstractMultiVideoWorkflow::ImageInput>> stepImages() const;

    QString generatorId() const;
    AbstractCli *imageCli() const;
    AbstractCli *decisionCli() const;
    QString videoFormatLabel() const;
    DialogGenerationOptions::VideoFormat videoFormat() const;

private slots:
    void _onStepCountChanged(int totalSteps);
    void _updateTotalCountLabel();
    void _updateOkButton();
    void _applyDefaultRegenToAllSlots();

private:
    QList<AbstractCli *> m_availableClis;

    QLineEdit *m_editKeyword = nullptr;
    QSpinBox *m_spinTotalSteps = nullptr;

    QPlainTextEdit *m_editIntroPrompt = nullptr;
    QSpinBox *m_spinIntroTakes = nullptr;

    QPlainTextEdit *m_editAfterIntroPrompt = nullptr;
    QSpinBox *m_spinAfterIntroTakes = nullptr;

    QLabel *m_labelTotalSummary = nullptr;

    QComboBox *m_comboFormat = nullptr;
    QComboBox *m_comboGenerator = nullptr;
    QComboBox *m_comboImageCli = nullptr;
    QComboBox *m_comboDecisionCli = nullptr;

    QComboBox *m_comboDefaultRegen = nullptr;
    QPushButton *m_buttonApplyAllRegen = nullptr;

    QVBoxLayout *m_stepsLayout = nullptr;
    QList<MultiVideoStepWidget *> m_stepWidgets;

    QPushButton *m_buttonGenerate = nullptr;

    void _rebuildStepWidgets(int totalSteps);
};

#endif // DIALOGMULTIVIDEOGENERATION_H
