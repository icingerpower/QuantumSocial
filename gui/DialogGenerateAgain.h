#ifndef DIALOGGENERATEAGAIN_H
#define DIALOGGENERATEAGAIN_H

#include <QDialog>

namespace Ui { class DialogGenerateAgain; }

class DialogGenerateAgain : public QDialog
{
public:
    explicit DialogGenerateAgain(const QString &configuration, QWidget *parent = nullptr);
    ~DialogGenerateAgain();
    int generationCount() const;

private:
    Ui::DialogGenerateAgain *ui;
};

#endif
