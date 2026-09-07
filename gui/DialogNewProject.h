#ifndef DIALOGNEWPROJECT_H
#define DIALOGNEWPROJECT_H

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui { class DialogNewProject; }
QT_END_NAMESPACE

// Collects what a new project needs: the source image (required — OK stays
// disabled until the path points to an existing file), an optional SECOND
// image (a different angle/reference of the same product — never itself
// regenerated/bootstrapped, always kept as extra reference material during
// generation), plus an optional keyword and hook idea.
class DialogNewProject : public QDialog
{
    Q_OBJECT

public:
    explicit DialogNewProject(QWidget *parent = nullptr);
    ~DialogNewProject();

    QString imagePath() const;
    // "" when no second image was picked.
    QString imagePath2() const;
    QString keyword() const;
    QString hookIdea() const;

private slots:
    void _browseImage();
    void _browseImage2();
    void _updateOkButton();

private:
    Ui::DialogNewProject *ui;
};

#endif // DIALOGNEWPROJECT_H
