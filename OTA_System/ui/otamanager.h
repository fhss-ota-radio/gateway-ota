#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include "discovery.h"    // DiscoveredDevice/discoverDevices() — 마찬가지로 Qt 의존성 없음
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

// Cc1101Transport 전체 정의는 필요 없고 포인터 타입만 있으면 되므로 전방
// 선언만 함 — otamanager.cpp에서 "cc1101transport.h"를 include해야 실제로
// 이 타입의 멤버(stopFhss() 등)를 씀. fhssTransport() 존재 이유는 아래
// 주석 참고.
class Cc1101Transport;

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
    void onDiscoverClicked();
    void onFhssActivateClicked();
    void onFhssStopClicked();
    void onFhssStatusTick(); // m_fhssStatusTimer가 주기 호출 — getFhssStatus() 표시 갱신
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
    // onDiscoverClicked()가 백그라운드 QThread에서 discoverDevices() 결과를
    // 받아온 뒤, QMetaObject::invokeMethod(Qt::QueuedConnection)로 GUI
    // 스레드에서 이 함수를 호출해 targetCombo를 채움 (위젯은 GUI 스레드
    // 에서만 건드려야 하므로 — discoverDevices() 자체가 blocking이라
    // (discovery.h 주석) 워커 스레드로 뺀 이유는 design-notes 44절 참고)
    void handleDiscoveredDevices(const std::vector<DiscoveredDevice> &devices);
    // onFhssActivateClicked()가 백그라운드 QThread에서 rolloutFhssConfig()+
    // configureFhss()+startFhss()를 다 돌린 뒤, invokeMethod(Qt::QueuedConnection)로
    // GUI 스레드에서 이 함수를 불러 버튼/상태 라벨을 갱신함. Cc1101Status 등
    // CC1101 전용 타입을 헤더에 안 끌고 오려고(다른 곳과 같은 이유) 결과를
    // bool/문자열로만 넘김 — 상세는 design-notes 45절 참고
    void handleFhssActivationResult(bool activated, uint32_t sessionId, const QString &message);
    // m_transport는 std::unique_ptr<ITransport>(기반 클래스) 타입이라서
    // stopFhss()/configureFhss() 같은 CC1101 전용 메서드(ITransport 계약
    // 밖, cc1101transport.h의 Cc1101Transport에만 있음)를 m_transport->로
    // 직접 못 부른다 — 컴파일 에러("no member named 'stopFhss' in
    // 'ITransport'") 남. dynamic_cast로 실제 타입을 확인해서 돌려줌
    // (onConnectClicked()가 항상 Cc1101Transport만 만들어서 지금은 항상
    // 성공하지만, 나중에 다른 ITransport 구현체가 생기면 그때는 nullptr이
    // 돌아올 수 있어 호출부마다 null 체크 필요).
    Cc1101Transport *fhssTransport() const;

    // otamanager.ui에서 setupUi()가 채워주는 위젯 트리 (portEdit, connectButton,
    // unicastRadio/broadcastRadio, targetCombo, selectFileButton,
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
    bool m_discovering = false; // DISCOVER 워커 스레드가 도는 동안 true — 중복 클릭/m_transport 동시접근 방지

    // FHSS(주파수 도약) 상태 — targetCombo에서 고른 기기 하나와만 맺음(여러
    // 기기 동시 호핑은 fhssrollout.h 설계상 아직 미지원, otamanager.ui
    // fhssHintLabel 참고).
    bool m_fhssBusy = false;   // 활성화/중지 워커 스레드가 도는 동안 true — m_discovering과 같은 이유
    bool m_fhssActive = false; // 활성화 성공 + Gateway 커널 MASTER 호핑이 켜진 상태
    uint32_t m_fhssSessionId = 0;      // FHSS_CONFIG/ACTIVATE에 쓴 session_id — 이후 OtaSession::start()에 그대로 재사용
    uint32_t m_fhssTargetDeviceId = 0; // 활성화 당시 targetCombo에서 골랐던 대상(활성화 후 콤보가 바뀌어도 유지)
    QTimer *m_fhssStatusTimer = nullptr; // 활성화 중일 때만 getFhssStatus()를 주기 호출(500ms)해 fhssStatusLabel 갱신
};
#endif // OTAMANAGER_H
