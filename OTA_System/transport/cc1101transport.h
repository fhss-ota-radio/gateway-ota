#ifndef CC1101TRANSPORT_H
#define CC1101TRANSPORT_H

#include "cc1101_status.h"
#include "itransport.h"

#include <cstdint>
#include <functional>
#include <string>

// ITransport의 CC1101 구현체.
//
// 실제 SPI/레지스터 제어는 팀원 4·5의 커널 드라이버가 담당하고, 여기서는 그 드라이버가
// 만드는 /dev/cc1101 캐릭터 디바이스 노드를 open/write/poll+read/ioctl로 다루는
// "사용자 공간 래퍼"만 구현한다 (cc1101-radio-api.md 9절 매핑 표 기준):
//   채널 변경 -> ioctl() / RX 시작 -> ioctl() / 송신 -> write()
//   수신 -> poll() + read() / FIFO 초기화 -> ioctl()
//
// [의도적으로 Qt 의존성 없음] QString/QByteArray 대신 std::string/std::vector<uint8_t>
// 사용. 여기서 하는 일이 POSIX 시스템콜(open/write/poll/read/ioctl) 호출뿐이라
// Qt가 전혀 필요 없음 — Qt는 화면(ui/) 쪽에서만 필요합니다.
//
// [실구현 완료, 2026-08-11] 팀원분(`feature/ota-tx` 브랜치)이 실제 POSIX 구현을
// 채운 걸 이 브랜치(`ota-core-skeleton`)로 가져왔습니다. open()은 O_NONBLOCK으로
// /dev/cc1101을 열고, send()는 write(), recv()는 poll()+read()+ioctl(GET_STATUS)로
// RSSI/LQI/CRC까지 채웁니다. 자세한 내용은 cc1101transport.cpp 주석 및
// docs/note/design-notes-gateway-ota-es.md 참고.
class Cc1101Transport : public ITransport
{
public:
    explicit Cc1101Transport(const std::string &devicePath = "/dev/cc1101");
    ~Cc1101Transport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;

    bool send(const std::vector<uint8_t> &data) override;
    std::vector<uint8_t> recv() override;
    // [2026-08-19] ITransport::flushRx() 구현 — 반환값 없이 override해야 해서
    // (ITransport는 하드웨어 특정 타입인 Cc1101Status를 몰라야 함) void로 바꿈.
    // 성공/실패는 기존처럼 lastStatus()로 확인 가능(m_lastStatus는 그대로 갱신함).
    void flushRx() override;

    // cc1101-radio-api.md 5절 공통 API 대응 (라디오 API 담당 영역 — 팀원 4·5 드라이버가 실제 처리)
    Cc1101Status setChannel(uint8_t channel);
    Cc1101Status startRx();
    Cc1101Status flushTx();

    // [2026-08-22 추가] FHSS(주파수 도약) 제어 — 커널 cc1101_ioctl.h의
    // CC1101_IOC_FHSS_SET_CONFIG/START/STOP/GET_STATUS(ioctl 14~17)를 감싼다.
    // 기존 setChannel() 등과 같은 자리(ITransport 계약 밖의 CC1101 전용 추가
    // 메서드)에 둔다 — 무선칩 하나를 다루는 코드는 한 클래스에 모아둔다는
    // 원칙, 새 클래스로 쪼개지 않는 이유는 cc1101transport.cpp 주석 참고.
    //
    // "지금 호핑 중인지"를 이 클래스가 별도 변수로 기억하지 않는다 — 그러면
    // 실제 커널 상태와 어긋날 위험이 있어서, 필요할 때마다 getFhssStatus()로
    // 커널에 직접 물어보는 쪽을 택했다(커널이 유일한 진실 공급원).
    Cc1101Status configureFhss(const Cc1101FhssConfig &config);
    Cc1101Status startFhss(Cc1101FhssRole role);
    Cc1101Status stopFhss();
    Cc1101FhssStatus getFhssStatus();

    // [2026-08-26 추가] 비호핑 고정채널(0) 리셋 —
    // stopFhss()->setChannel(0)->flushRx()->startRx() 4단계를 한 곳에 모음.
    //
    // 왜 필요한가: 이 4단계가 원래 최소 4곳(ota_smoke_fhss_reset_main.cpp,
    // smoke_fhss_ota_transfer_main.cpp, otamanager.cpp의 prepareFixedOta()/
    // onFhssActivateClicked())에서 각자 조금씩 다르게(일부는 flushRx 누락)
    // 다시 구현되고 있었음 — "로직은 core에 한 곳, 호출부는 얇게" 원칙에
    // 어긋나서 여기 하나로 통합함(design-notes-gateway-ota-es.md 참고).
    //
    // 호핑 경로에도 그대로 쓴다 — "리셋"은 호핑/비호핑 공통 시작 단계이고,
    // 차이는 리셋 *다음*에 무엇을 하느냐뿐이다(비호핑은 여기서 끝, 호핑은
    // 이어서 configureFhss()+startFhss()를 호출해 다시 호핑 상태로 전환).
    //
    // 한 단계라도 실패하면 그 즉시 false를 반환한다(이후 단계는 시도 안 함) —
    // 이미 일관성이 깨진 상태에서 다음 단계를 계속해봐야 의미가 없어서.
    // stopFhss()만 예외: 이미 호핑이 꺼져 있어도 성공 취급되는 멱등 동작이라
    // 실패해도 무시하고 계속 진행함.
    //
    // onStageLog: 실패한 단계 이름을 알려주는 선택적 콜백(예: "setChannel(0)
    // 실패"). CLI는 stderr에, Qt는 appendLog()에 넘겨서 각자 방식으로
    // 로그를 남길 수 있게 함(rolloutFhssConfig()의 onLog 콜백과 같은 패턴).
    // nullptr로 두면 조용히 성공/실패(bool)만 돌려줌.
    bool resetToFixedChannel(const std::function<void(const std::string &)> &onStageLog = nullptr);

    // 마지막 recv() 성공 시 RSSI/LQI/CRC/수신시각 (cc1101-radio-api.md 4절)
    Cc1101RxMetadata lastRxMetadata() const { return m_lastRxMetadata; }
    // 마지막으로 실행한 동작의 상태값 (실패 원인 파악용)
    Cc1101Status lastStatus() const { return m_lastStatus; }

private:
    // errno(시스템 콜 실패 원인 번호)를 Cc1101Status로 변환 (예: EAGAIN -> NoData)
    Cc1101Status statusFromErrno(int err) const;

    std::string m_devicePath;
    int m_fd = -1;
    uint8_t m_channel = 0;
    Cc1101Status m_lastStatus = Cc1101Status::NotInitialized;
    Cc1101RxMetadata m_lastRxMetadata;
};

#endif // CC1101TRANSPORT_H
