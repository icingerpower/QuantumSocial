#ifndef PANEGENERATION_H
#define PANEGENERATION_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class PaneGeneration; }
QT_END_NAMESPACE

class PaneGeneration : public QWidget
{
    Q_OBJECT

public:
    explicit PaneGeneration(QWidget *parent = nullptr);
    ~PaneGeneration();

private:
    Ui::PaneGeneration *ui;
};

#endif // PANEGENERATION_H
