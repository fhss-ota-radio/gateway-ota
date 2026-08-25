#include "ui/otamanager.h"
#include <QFontDatabase>
#include <QFont>
#include <QCoreApplication>
#include <QDebug>

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

     QString fontPath =
        QCoreApplication::applicationDirPath()
        + "/NotoSansCJK-Regular.ttc";

    int fontId = QFontDatabase::addApplicationFont(fontPath);

    if (fontId >= 0) {
        QStringList families =
            QFontDatabase::applicationFontFamilies(fontId);

        if (!families.isEmpty()) {
            a.setFont(QFont(families.first()));
            qDebug() << "Loaded font:" << families.first();
        }
    } else {
        qDebug() << "Failed to load font:" << fontPath;
    }
    OtaManager w;
    w.show();
    return QApplication::exec();
}
