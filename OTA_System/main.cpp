#include "ui/otamanager.h"

#include "build_info.h" // cmake가 생성 — OTA_SYSTEM_GIT_HASH 등 (build_info.h.in 참고)

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QString>

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

// [2026-08-25] 창 제목에 커밋 해시를 넣어서, 실기기(라즈베리파이)에서
// "지금 이 바이너리가 최신 커밋으로 재빌드된 게 맞는지" 창만 봐도 바로
// 확인 가능하게 함 — 096dcdc로 빌드된 걸 최신인 줄 알고 테스트하다가
// 뒤늦게 안 됨 문제가 실제로 있었음(design-notes 참고). ESP32 쪽이
// 부팅 로그에 "App version: <git hash>"를 자동으로 찍는 것과 같은 목적.
// build_info.h는 cmake가 재빌드할 때마다 자동 생성하므로(build_info.h.in
// 참고) 커밋마다 사람이 손으로 바꿀 게 없다.
static QString buildVersionString()
{
    QString version = QStringLiteral(OTA_SYSTEM_GIT_HASH);
    if (OTA_SYSTEM_GIT_DIRTY)
        version += QStringLiteral("-dirty");
    version += QStringLiteral(" (") + QStringLiteral(OTA_SYSTEM_GIT_BRANCH) + QStringLiteral(")");
    return version;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    loadKoreanFont();
    OtaManager w;
    w.setWindowTitle(w.windowTitle() + QStringLiteral(" — ") + buildVersionString());
    w.show();
    return QApplication::exec();
}
