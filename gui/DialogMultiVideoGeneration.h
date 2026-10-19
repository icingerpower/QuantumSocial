#ifndef DIALOGMULTIVIDEOGENERATION_H
#define DIALOGMULTIVIDEOGENERATION_H

#include <QDialog>
#include <QList>
#include <QUuid>

#include "DialogGenerationOptions.h"
#include "model/properties/TreeProperties.h"
#include "model/videogen/AbstractMultiVideoWorkflow.h"

class AbstractCli;
class MultiVideoStepWidget;
class SavedPrompts;
class TableVideos;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

// Dialog for configuring a multi-video project (1 Intro step + N After-intro steps):
// - Step count picker
// - 1 prompt for Intro + 1 prompt for After-intro steps (with save/load/edit via SavedPrompts)
// - A/B property selection for the Intro step prompt (step 1 only)
// - Takes count for Intro + After-intro steps
// - Up to 4 images per step with individual regeneration strategy
// - CLI selection (Image CLI, Decision CLI) and Video Generator selection
class DialogMultiVideoGeneration : public QDialog
{
    Q_OBJECT

public:
    struct PropertyRow
    {
        QUuid propertyId;
        QCheckBox *checkBox = nullptr;
        QComboBox *valueCombo = nullptr;
    };

    explicit DialogMultiVideoGeneration(const QList<AbstractCli *> &availableClis,
                                        QWidget *parent = nullptr);
    explicit DialogMultiVideoGeneration(const QList<AbstractCli *> &availableClis,
                                        SavedPrompts *savedPrompts,
                                        QWidget *parent = nullptr);
    explicit DialogMultiVideoGeneration(const QList<AbstractCli *> &availableClis,
                                        SavedPrompts *savedPrompts,
                                        TreeProperties *properties,
                                        TableVideos *videos,
                                        QWidget *parent = nullptr);
    ~DialogMultiVideoGeneration() override = default;

    QString keyword() const;
    void setKeyword(const QString &keyword);
    int totalSteps() const;
    QString introPrompt() const;
    QString rawIntroPrompt() const;
    void setIntroPrompt(const QString &prompt);
    int introTakes() const;
    QString afterIntroPrompt() const;
    void setAfterIntroPrompt(const QString &prompt);
    int afterIntroTakes() const;

    QList<QUuid> introPropertyValueIds() const;
    const QList<PropertyRow> &introPropertyRows() const { return m_propertyRows; }
    QLabel *introFinalPreviewLabel() const { return m_labelIntroFinalPreview; }

    QList<QList<AbstractMultiVideoWorkflow::ImageInput>> stepImages() const;

    QString generatorId() const;
    AbstractCli *imageCli() const;
    AbstractCli *decisionCli() const;
    QString videoFormatLabel() const;
    DialogGenerationOptions::VideoFormat videoFormat() const;

    QComboBox *savedIntroPromptCombo() const { return m_comboSavedIntroPrompt; }
    QPushButton *loadIntroPromptButton() const { return m_buttonLoadIntroPrompt; }
    QPushButton *editIntroPromptButton() const { return m_buttonEditIntroPrompt; }
    QPushButton *saveIntroPromptButton() const { return m_buttonSaveIntroPrompt; }

    QComboBox *savedAfterIntroPromptCombo() const { return m_comboSavedAfterIntroPrompt; }
    QPushButton *loadAfterIntroPromptButton() const { return m_buttonLoadAfterIntroPrompt; }
    QPushButton *editAfterIntroPromptButton() const { return m_buttonEditAfterIntroPrompt; }
    QPushButton *saveAfterIntroPromptButton() const { return m_buttonSaveAfterIntroPrompt; }

private slots:
    void _onStepCountChanged(int totalSteps);
    void _updateTotalCountLabel();
    void _updateOkButton();
    void _applyDefaultRegenToAllSlots();
    void _recomputeIntroFinal();

private:
    QList<AbstractCli *> m_availableClis;
    SavedPrompts *m_savedPrompts = nullptr;
    TreeProperties *m_properties = nullptr;
    TableVideos *m_videos = nullptr;

    QLineEdit *m_editKeyword = nullptr;
    QSpinBox *m_spinTotalSteps = nullptr;

    QComboBox *m_comboSavedIntroPrompt = nullptr;
    QPushButton *m_buttonLoadIntroPrompt = nullptr;
    QPushButton *m_buttonEditIntroPrompt = nullptr;
    QPushButton *m_buttonSaveIntroPrompt = nullptr;
    QPlainTextEdit *m_editIntroPrompt = nullptr;
    QSpinBox *m_spinIntroTakes = nullptr;

    QList<PropertyRow> m_propertyRows;
    QLabel *m_labelIntroFinalPreview = nullptr;

    QComboBox *m_comboSavedAfterIntroPrompt = nullptr;
    QPushButton *m_buttonLoadAfterIntroPrompt = nullptr;
    QPushButton *m_buttonEditAfterIntroPrompt = nullptr;
    QPushButton *m_buttonSaveAfterIntroPrompt = nullptr;
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
    void _refreshSavedPromptCombos();
    void _loadPrompt(QComboBox *combo, QPlainTextEdit *edit);
    void _editPrompt(QComboBox *combo, QPlainTextEdit *edit);
    void _savePrompt(QComboBox *combo, QPlainTextEdit *edit);
    QString _computeIntroFinal() const;
};

#endif // DIALOGMULTIVIDEOGENERATION_H
