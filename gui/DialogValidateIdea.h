#ifndef DIALOGVALIDATEIDEA_H
#define DIALOGVALIDATEIDEA_H

#include <QDialog>

class QPlainTextEdit;

class DialogValidateIdea : public QDialog
{
    Q_OBJECT

public:
    explicit DialogValidateIdea(const QString &contentType,
                                const QString &idea,
                                QWidget *parent = nullptr);
    ~DialogValidateIdea() override;

    QString ideaText() const;

private:
    QPlainTextEdit *m_textEdit;
};

#endif // DIALOGVALIDATEIDEA_H
