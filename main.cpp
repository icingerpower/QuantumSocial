#include "../common/workingdirectory/WorkingDirectoryManager.h"
#include "../common/workingdirectory/DialogOpenConfig.h"

#include "gui/MainWindow.h"

#include <QApplication>

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

    MainWindow w;
    w.show();
    return a.exec();
}
