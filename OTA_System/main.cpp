#include "ui/otamanager.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    OtaManager w;
    w.show();
    return QApplication::exec();
}
