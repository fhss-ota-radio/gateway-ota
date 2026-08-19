#ifndef CC1101TRANSPORT_H
#define CC1101TRANSPORT_H

#include "cc1101_status.h"
#include "itransport.h"

#include <cstdint>
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
