#include "../common/workingdirectory/WorkingDirectoryManager.h"
#include "../common/workingdirectory/DialogOpenConfig.h"

#include "gui/MainWindow.h"

#include "model/PreferredHashtags.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>

namespace {

// Same subfolder name PaneGeneration looks in (see REFERENCE_IMAGES_SUBDIR)
// to ground the very first A/B property catalog in real images instead of
// the suggestion CLI inventing values from nothing.
const QString REFERENCE_IMAGES_SUBDIR = QStringLiteral("reference_images");

// A freshly picked/created working directory has nothing in it yet — that
// is the one moment it makes sense to offer this one-time setup step
// (asking again on every later launch would be noise).
bool isEmptyWorkingDir(const QDir &dir)
{
    return dir.exists()
        && dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
}

// Copies every image file found at the top level of sourceFolder into
// <workingDir>/reference_images/. Returns how many were actually copied.
int copyReferenceImages(const QString &sourceFolder, const QDir &workingDir)
{
    QDir referenceDir{workingDir.absoluteFilePath(REFERENCE_IMAGES_SUBDIR)};
    referenceDir.mkpath(QStringLiteral("."));
    const QDir source{sourceFolder};
    int copied = 0;
    for (const QFileInfo &entry : source.entryInfoList(
             {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
              QStringLiteral("*.jpeg"), QStringLiteral("*.webp")},
             QDir::Files))
    {
        if (QFile::copy(entry.absoluteFilePath(),
                         referenceDir.absoluteFilePath(entry.fileName())))
        {
            ++copied;
        }
    }
    return copied;
}

// Offers to seed the new working directory's A/B property catalog with real
// images: asks for a folder, copies whatever it finds, and — since skipping
// this silently would leave every future generation guessing — makes the
// user explicitly confirm skipping it instead of just cancelling through.
void offerReferenceImageSetup(const QDir &workingDir)
{
    while (true)
    {
        const QString sourceFolder = QFileDialog::getExistingDirectory(nullptr,
            QObject::tr("Pick a folder of source images for property "
                        "initialization"),
            QString{}, QFileDialog::ShowDirsOnly);
        if (!sourceFolder.isEmpty())
        {
            const int copied = copyReferenceImages(sourceFolder, workingDir);
            if (copied == 0)
            {
                QMessageBox::warning(nullptr, QObject::tr("No images found"),
                    QObject::tr("No image files (.png/.jpg/.jpeg/.webp) were "
                        "found directly in that folder — pick another folder "
                        "or cancel to continue without reference images."));
                continue;
            }
            QMessageBox::information(nullptr, QObject::tr("Reference images copied"),
                QObject::tr("%1 image(s) copied — they will be used to seed "
                    "the A/B property catalog the first time you generate.")
                    .arg(copied));
            return;
        }
        if (QMessageBox::question(nullptr, QObject::tr("No reference images"),
                QObject::tr("Continue WITHOUT reference images? The A/B "
                    "property catalog will then start empty and be built up "
                    "from the CLI's own suggestions instead (you can still "
                    "add reference images later from the properties list — "
                    "right-click → \"Bootstrap from reference images...\")."))
            == QMessageBox::Yes)
        {
            return;
        }
        // No: the user wants to pick a folder after all — loop back.
    }
}

// Also asked once at first-run setup (editable any time after from Settings
// — see PaneSettings) because CLI-invented hashtags are pure guesswork with
// no engagement data behind them; a curated list up front means the very
// first suggested hooks already draw from something the user trusts.
void offerPreferredHashtagsSetup(const QDir &workingDir)
{
    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(nullptr,
        QObject::tr("Preferred hashtags"),
        QObject::tr("Optional: enter the hashtags you want the CLI to prefer "
            "when writing hooks/descriptions, instead of it inventing ones "
            "with no engagement data behind them (one per line, or separated "
            "by spaces/commas — editable any time later in Settings):"),
        QString{}, &accepted);
    if (!accepted)
    {
        return;
    }
    PreferredHashtags hashtags{workingDir.absolutePath()};
    hashtags.setHashtags(PreferredHashtags::parse(text));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName("GRDF");
    QCoreApplication::setOrganizationDomain("grdf.fr");
    QCoreApplication::setApplicationName("QuantumSocial");
    QApplication a(argc, argv);
    WorkingDirectoryManager::instance()->installDarkOrangePalette();

    DialogOpenConfig dialog;
    dialog.exec();
    if (dialog.wasRejected())
    {
        return 0;
    }

    if (isEmptyWorkingDir(WorkingDirectoryManager::instance()->workingDir()))
    {
        offerReferenceImageSetup(WorkingDirectoryManager::instance()->workingDir());
        offerPreferredHashtagsSetup(WorkingDirectoryManager::instance()->workingDir());
    }

    MainWindow w;
    w.show();
    return a.exec();
}
