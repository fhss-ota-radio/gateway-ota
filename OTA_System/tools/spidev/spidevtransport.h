#ifndef SPIDEVTRANSPORT_H
#define SPIDEVTRANSPORT_H

#include "itransport.h"

#include <cstdint>
#include <string>

// [진단 도구 — 제품 코드가 아닙니다]
//
// 커널 모듈을 완전히 건너뛰고 리눅스 표준 spidev(/dev/spidevX.Y) 유저공간
// 인터페이스로 CC1101을 직접 폴링하는 ITransport 구현체입니다.
//
// [정식 경로는 Cc1101Transport입니다] /dev/cc1101(커널 드라이버 기반)이 정식
// 경로이고 2026-08-16 실기기 검증을 통과했습니다. 이 클래스는 처음엔 그게
// 막혀있는 동안의 우회로로 만들었지만, 지금은 **통제 실험(control experiment)
// 도구**로 남겨둔 것입니다 — 커널·인터럽트·kfifo를 전부 우회하므로, 이걸로
// 되는지 여부만으로 "원인이 하드웨어냐 커널이냐"를 한 번에 가를 수 있습니다.
// 실제로 2026-08-16 디버깅에서 이 비교가 결정적이었습니다.
//
// ITransport 인터페이스가 동일하므로 session/simplesender.h·simplereceiver.h
// (performHandshake 등)는 손대지 않고 그대로 이 클래스로 검증할 수 있습니다.
//
// 존재 이유·사용 시점·삭제 조건은 tools/spidev/README.md 참고.
//
// [주소 필터를 끔] kernel-cc1101-spi의 기본 레지스터값(PKTCTRL1=0x0D)은
// CC1101 하드웨어 주소필터(ADR_CHK)가 켜져 있어서, 패킷의 첫 바이트를
// "목적지 주소"로 해석해 자기 ADDR 레지스터(기본 0x00)와 다르면 무선으로
// 도착해도 조용히 버립니다. 2026-08-14 실기기 테스트에서 이걸로 패킷이
// 계속 안 잡히는 문제를 겪었습니다 — 우리 ota_protocol.h 패킷은 첫 바이트가
// 주소가 아니라 패킷타입 등 프로토콜 자체 데이터라서, 이 필터를 켜두면
// device_id 값에 따라 우연히 패킷이 씹힐 수 있습니다. device_id 기반 수신
// 판단은 프로토콜(소프트웨어) 계층에서 하는 게 맞으므로, 여기서는
// PKTCTRL1의 ADR_CHK를 꺼서(0x0C) 하드웨어 필터를 비활성화합니다.
// [팀 공유 필요] kernel-cc1101-spi 드라이버(cc1101_core.c의
// cc1101_default_regs[])도 같은 값(0x0D)을 쓰고 있어 똑같은 문제를 겪을 수
// 있음 — 드라이버 담당자에게 공유해야 함.
//
// [실기기 전용] cc1101transport.h와 같은 이유로 리눅스(__linux__) 전용이며,
// 그 외 플랫폼(macOS 로컬 개발 등)에서는 항상 실패하는 스텁으로 대체됩니다.
class SpidevTransport : public ITransport
{
public:
    explicit SpidevTransport(const std::string &devicePath = "/dev/spidev0.0");
    ~SpidevTransport() override;

    // 연 다음 CC1101 리셋 + 기본 레지스터(433.92MHz/2-FSK/38.4kbps, 주소필터
    // 끔) 로드 + 캘리브레이션 + RX 진입까지 한 번에 처리합니다
    // (kernel-cc1101-spi의 probe()가 하던 일을 유저공간에서 대신 함).
    bool open() override;
    void close() override;
    bool isOpen() const override;

    // data 전체를 CC1101 TX FIFO에 [길이바이트][data...]로 쓰고 STX로 송신.
    // 완료(TXBYTES==0)까지 최대 약 200ms 블로킹 대기합니다.
    bool send(const std::vector<uint8_t> &data) override;
    // 논블로킹: 수신된 패킷이 있으면 그 바이트들을, 없으면 빈 벡터를 반환.
    std::vector<uint8_t> recv() override;

private:
    void xfer(uint8_t *tx, uint8_t *rx, int len);
    void strobe(uint8_t cmd);
    uint8_t readStatusReg(uint8_t addr);

    std::string m_devicePath;
    int m_fd = -1;
};

#endif // SPIDEVTRANSPORT_H
