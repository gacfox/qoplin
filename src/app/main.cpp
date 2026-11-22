#include "ApplicationController.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setApplicationName(QStringLiteral("Qoplin"));
    QApplication::setOrganizationName(QStringLiteral("Qoplin"));
    QApplication::setApplicationDisplayName(QStringLiteral("Qoplin"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/qoplin.png")));

    ApplicationController controller;
    controller.start();
    return QApplication::exec();
}
