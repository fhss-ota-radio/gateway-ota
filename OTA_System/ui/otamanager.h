#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include "itransport.h"  // ITransport — core/session과 같은 이유로 Qt 의존성 없음, 화면 헤더에
#include "otasession.h"  // 직접 include해도 안전함 (cstdint/functional/string/vector만 씀)

#include <QMainWindow>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui {
class OtaManager;
}
class QCloseEvent;
class QTimer;
QT_END_NAMESPACE

class OtaManager : public QMainWindow
{
    Q_OBJECT

public:
    explicit OtaManager(QWidget *parent = nullptr);
    ~OtaManager() override;

protected:
    // 창을 닫을 때 마지막 설정값(포트/청크크기/전송모드)을 저장하기 위해 오버라이드
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onConnectClicked();
    void onModeChanged();
    void onSelectFileClicked();
    void onStartClicked();
    void onPauseClicked();
    void onHopSeedRandomClicked();
    void onSessionTick(); // m_tickTimer가 주기 호출 — OtaSession::tick()으로 그대로 넘김

private:
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString &tag, const QString &message);
    void recalcChunkInfo();

    // OtaSession::setOnStateChanged()로 등록하는 콜백 본체 — 상태가 바뀔 때마다
    // 진행률바·로그·버튼 상태를 갱신 (fsm-design.md §1 "화면과 로직의 경계" 원칙)
    void handleSessionStateChanged(OtaSessionState state);
    // progress()를 읽어서 progressBar/ackStatusLabel을 갱신 — 상태 변화 콜백과
    // onSessionTick() 양쪽에서 공용으로 부름 (배치 도중에도 값이 계속 바뀌므로)
    void updateProgressUi();

    // otamanager.ui에서 setupUi()가 채워주는 위젯 트리 (driverCombo, portEdit,
    // connectButton, unicastRadio/broadcastRadio, targetCombo, selectFileButton,
    // chunkSizeValueLabel, progressBar, logView 등은 전부 Ui::OtaManager의 멤버)
    Ui::OtaManager *ui;

    bool m_connected = false;
    QString m_selectedFilePath;
    qint64 m_selectedFileSize = 0;

    // 화면이 소유하는 실제 전송 계층 + 세션. 둘 다 "연결"/"전송 시작" 버튼을
    // 누르기 전까지는 비어있다(nullptr) — otasession.h/itransport.h가 Qt 의존성
    // 없는 순수 C++이라 unique_ptr로 그대로 들고 있을 수 있음.
    std::unique_ptr<ITransport> m_transport;
    std::unique_ptr<OtaSession> m_session;
    QTimer *m_tickTimer = nullptr; // OtaSession::tick()을 10ms마다 호출 (otasession.h 84행 주석 그대로)
    int m_retransmitEventCount = 0; // ackStatusLabel의 "재전송 N회" 표시용 — Retransmitting 상태 진입 횟수 근사치
};
#endif // OTAMANAGER_H
