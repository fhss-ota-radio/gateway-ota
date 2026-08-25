#include "ui/otamanager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>

// [2026-08-25] 한글 폰트를 실행 파일과 함께 배포해서 강제 로드한다.
//
// 왜 필요한가: 화면(otamanager.cpp/ui)의 라벨·버튼·로그가 전부 한글인데,
// 라즈베리파이의 욕토(Yocto) 임베디드 이미지는 GUI용으로 설계되지 않아
// CJK(Chinese/Japanese/Korean, 한중일 통합 문자권) 폰트가 아예 없을 수
// 있다(149 환경 점검, design-notes 69절). 그 상태로 그냥 띄우면 한글이
// 전부 깨진 네모(tofu)로만 보인다 — OS에 뭐가 깔려있든 상관없게, 폰트
// 파일 자체를 앱과 함께 배포하고 시작 시 명시적으로 등록한다.
//
// 필요 파일: resources/fonts/NotoSansCJK-Regular.ttc — Google Noto
// Fonts 공식 배포본에서 받아 이 경로에 추가해야 함(레포에 아직 없음).
// CMakeLists.txt가 빌드 시 이 파일을 실행 파일과 같은 폴더로 복사한다.
//
// 실패 시(파일이 없거나 addApplicationFont 실패) 조용히 Qt 기본 폰트로
// 폴백한다 — 한글은 깨지지만 앱 자체가 죽지는 않는다.
static void loadKoreanFont()
{
    const QString fontPath =
        QCoreApplication::applicationDirPath() + "/NotoSansCJK-Regular.ttc";
    const int fontId = QFontDatabase::addApplicationFont(fontPath);
    if (fontId < 0) {
        qDebug() << "한글 폰트 로드 실패, 기본 폰트로 계속 진행:" << fontPath;
        return;
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    if (families.isEmpty()) {
        qDebug() << "폰트 파일은 읽었지만 family 이름이 비어 있음:" << fontPath;
        return;
    }
    qApp->setFont(QFont(families.first()));
    qDebug() << "한글 폰트 로드 완료:" << families.first();
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    loadKoreanFont();
    OtaManager w;
    w.show();
    return QApplication::exec();
}
