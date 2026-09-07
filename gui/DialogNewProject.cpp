#include "DialogNewProject.h"
#include "ui_DialogNewProject.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QStandardPaths>

DialogNewProject::DialogNewProject(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::DialogNewProject)
{
    ui->setupUi(this);

    connect(ui->buttonBrowse, &QPushButton::clicked, this, &DialogNewProject::_browseImage);
    connect(ui->buttonBrowse2, &QPushButton::clicked, this, &DialogNewProject::_browseImage2);
    connect(ui->editImage, &QLineEdit::textChanged, this, &DialogNewProject::_updateOkButton);
    _updateOkButton();
}

DialogNewProject::~DialogNewProject()
{
    delete ui;
}

QString DialogNewProject::imagePath() const
{
    return ui->editImage->text().trimmed();
}

QString DialogNewProject::imagePath2() const
{
    return ui->editImage2->text().trimmed();
}

QString DialogNewProject::keyword() const
{
    return ui->editKeyword->text().trimmed();
}

QString DialogNewProject::hookIdea() const
{
    return ui->editHook->text().trimmed();
}

void DialogNewProject::_browseImage()
{
    const QString startDir = imagePath().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
        : QFileInfo{imagePath()}.absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Select the source image"),
        startDir, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
    if (!path.isEmpty())
    {
        ui->editImage->setText(path);
    }
}

void DialogNewProject::_browseImage2()
{
    const QString startDir = imagePath2().isEmpty()
        ? (imagePath().isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
            : QFileInfo{imagePath()}.absolutePath())
        : QFileInfo{imagePath2()}.absolutePath();
    const QString path = QFileDialog::getOpenFileName(this,
        tr("Select the second image (optional)"), startDir,
        tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
    if (!path.isEmpty())
    {
        ui->editImage2->setText(path);
    }
}

void DialogNewProject::_updateOkButton()
{
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(
        QFileInfo{imagePath()}.isFile());
}
