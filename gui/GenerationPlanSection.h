#ifndef GENERATIONPLANSECTION_H
#define GENERATIONPLANSECTION_H

#include <QGroupBox>
#include <QList>

QT_BEGIN_NAMESPACE
namespace Ui { class GenerationPlanSection; }
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;
class QWidget;
QT_END_NAMESPACE

// Designer-backed shell shared by every generation option. Instances are
// created dynamically because video generator backends are registered at
// runtime; only their data-driven property rows are built in code.
class GenerationPlanSection : public QGroupBox
{
public:
    explicit GenerationPlanSection(QWidget *parent = nullptr);
    ~GenerationPlanSection();

    QWidget *cliRow() const;
    int generationCount() const;
    void setGenerationCount(int count);
    QComboBox *cliCombo() const;
    QList<QRadioButton *> strategyButtons() const;
    QStackedWidget *strategyStack() const;
    QComboBox *savedPromptCombo(int strategy) const;
    QPushButton *loadPromptButton(int strategy) const;
    QPushButton *editPromptButton(int strategy) const;
    QPushButton *savePromptButton(int strategy) const;
    QPlainTextEdit *promptEdit(int strategy) const;
    QLabel *propertyLabel(int strategy) const;
    QScrollArea *propertyScroll(int strategy) const;
    QVBoxLayout *propertyLayout(int strategy) const;
    QLabel *previewLabel(int strategy) const;

private:
    Ui::GenerationPlanSection *ui;
};

#endif // GENERATIONPLANSECTION_H
