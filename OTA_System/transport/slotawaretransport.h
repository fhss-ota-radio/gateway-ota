#ifndef SLOTAWARETRANSPORT_H
#define SLOTAWARETRANSPORT_H

#include "cc1101transport.h"
#include "itransport.h"

#include <chrono>
#include <cstdint>
#include <vector>

// [2026-08-25 추출, feature/qt-fhss-transfer] 원래 tests/smoke_fhss_ota_transfer_main.cpp
// 안에 익명 네임스페이스로만 있던 클래스를 여기로 뽑아냈다.
//
// [왜 뽑았나] Qt 화면(otamanager.cpp)에서도 "호핑 전송" 옵션을 만들려면 이
// 클래스가 필요한데, CLI 전용 .cpp 파일 안에 있으면 화면 쪽에서 재사용할
// 방법이 없다(같은 로직을 화면 쪽에 복붙하면 나중에 타이밍 버그를 고칠 때
// 두 군데를 따로 고쳐야 하는 위험이 생김 — 지금도 66/67절에서 이 타이밍
// 로직이 얼마나 미묘한지 확인했다). 그래서 CLI와 화면이 똑같은 구현체 하나를
// 공유하도록 transport/(다른 전송 계층들과 같은 자리)로 옮겼다.
//
// [원본] CLI 쪽 커밋 cef9cda(kMinPacketGapMs 도입)/517a8f5(quiet window)의
// 로직을 그대로 옮긴 것이며, perf/fhss-slot-gap-tuning 브랜치에서 진행 중인
// "격슬롯 스킵" 수정(design-notes 66/67절)은 아직 실기기 검증 전이라 이
// 파일에는 아직 반영하지 않았다 — 검증되면 이 파일에 병합할 예정.
//
// FHSS(주파수 도약) 호핑이 진행 중인 채널 위에서 여러 패킷을 안전하게
// 보낼 수 있도록, 슬롯 경계/반이중 왕복시간/재동기화 상황을 감안해
// send()를 게이팅하는 ITransport 구현체.
class SlotAwareTransport final : public ITransport
{
public:
    // slotDurationUs: FhssHopPolicy::slotDurationUs를 그대로 받음 — 안전
    // 창이 이번 슬롯 안에서 아직 안 닫혔는지 판단하려면 슬롯 길이를 알아야
    // 한다.
    SlotAwareTransport(Cc1101Transport &transport, uint32_t generation, uint32_t slotDurationUs);

    bool open() override;
    void close() override;
    bool isOpen() const override;

    bool send(const std::vector<uint8_t> &packet) override;

    // 무엇이든 하나라도 받으면 "상대가 살아있다"는 뜻이므로 침묵 카운터를
    // 리셋한다.
    std::vector<uint8_t> recv() override;

private:
    static constexpr uint64_t kNoWindow = ~static_cast<uint64_t>(0);

    bool usable(const Cc1101FhssStatus &status) const;
    static void logGateFailure(const char *where, const Cc1101FhssStatus &status);
    bool ensureSafeWindow();

    Cc1101Transport &m_transport;
    uint32_t m_generation;
    int64_t m_slotDurationMs;
    uint64_t m_openWindowSlot = kNoWindow;
    std::chrono::steady_clock::time_point m_windowOpenedAt{};
    // 기본 생성된 time_point는 epoch(=count() 0)이므로, "아직 한 번도 안
    // 보냄"을 별도 bool 없이 이걸로 구분한다(ensureSafeWindow() 참고).
    std::chrono::steady_clock::time_point m_lastSentAt{};
    // 마지막으로 뭔가 수신한 이후 보낸 패킷 수 — 재동기화 양보 판단용.
    int m_sendsSinceLastRx = 0;
};

#endif // SLOTAWARETRANSPORT_H
