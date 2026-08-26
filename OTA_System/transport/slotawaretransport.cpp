#include "slotawaretransport.h"

#include <iostream>
#include <thread>

namespace {

constexpr int kSlotPollMs = 2;
constexpr int kPostSyncGuardMs = 25;
constexpr int kSlotGateTimeoutMs = 1200;
// [2026-08-24 추가, perf/fhss-slot-batch-tx] 슬롯 하나당 패킷 하나씩만
// 보내던 게 너무 느려서(슬롯 300ms마다 1개 = 7825청크면 40분 가까이) 같은
// 슬롯 안에서 여러 개를 연달아 보내도록 바꿈. 앞쪽(kPostSyncGuardMs)은
// 그대로 두고, 뒤쪽에도 이만큼(kTailGuardMs) 여유를 남겨서 다음 슬롯
// 경계(=다음 SYNC 송신 시점)와 안 겹치게 함.
// 상세: docs/note/design-notes-gateway-ota-es.md 55절.
constexpr int kTailGuardMs = 40;
// [2026-08-24 추가] 같은 슬롯 안에서 연속으로 보낼 때 패킷 사이에 최소한
// 이만큼은 띄운다 — CC1101 반이중(half-duplex) 실측 왕복시간 121ms에
// 여유를 얹은 값. 상세: docs/note/design-notes-gateway-ota-es.md 55절.
constexpr int kMinPacketGapMs = 150;
// [2026-08-25 추가] 재동기화 양보(quiet window) 조건. 상세:
// docs/note/design-notes-gateway-ota-es.md 58절.
constexpr int kQuietTriggerSends = 8;
constexpr int kResyncQuietMs = 4000;

} // namespace

SlotAwareTransport::SlotAwareTransport(
    Cc1101Transport &transport,
    uint32_t generation,
    uint32_t slotDurationUs
)
    : m_transport(transport), m_generation(generation),
      m_slotDurationMs(static_cast<int64_t>(slotDurationUs) / 1000)
{
}

bool SlotAwareTransport::open() { return m_transport.isOpen() || m_transport.open(); }
void SlotAwareTransport::close() { m_transport.close(); }
bool SlotAwareTransport::isOpen() const { return m_transport.isOpen(); }

bool SlotAwareTransport::send(const std::vector<uint8_t> &packet)
{
    if (!ensureSafeWindow())
        return false;

    const uint8_t type = packet.empty() ? 0 : packet.front();
    const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_windowOpenedAt).count();
    std::cout << "[fhss_ota][slot_tx] type=" << static_cast<unsigned>(type)
              << " slot=" << m_openWindowSlot
              << " window_elapsed_ms=" << elapsedMs << "\n";

    const bool ok = m_transport.send(packet);
    if (!ok) {
        // 실패했으면 이 창을 더 이상 못 믿음(예: 그 사이 동기화가
        // 깨졌을 수도 있음) — 다음 send() 호출은 처음부터 다시 게이팅.
        m_openWindowSlot = kNoWindow;
    } else {
        m_lastSentAt = std::chrono::steady_clock::now();
        ++m_sendsSinceLastRx;
    }
    return ok;
}

std::vector<uint8_t> SlotAwareTransport::recv()
{
    auto packet = m_transport.recv();
    if (!packet.empty())
        m_sendsSinceLastRx = 0;
    return packet;
}

bool SlotAwareTransport::usable(const Cc1101FhssStatus &status) const
{
    return status.enabled && status.synchronized &&
           status.role == static_cast<uint8_t>(Cc1101FhssRole::Master) &&
           status.generation == m_generation && status.lastError == 0;
}

void SlotAwareTransport::logGateFailure(const char *where, const Cc1101FhssStatus &status)
{
    std::cerr << "[fhss_ota][slot_tx] " << where
              << " enabled=" << status.enabled
              << " synchronized=" << status.synchronized
              << " role=" << static_cast<unsigned>(status.role)
              << " generation=" << status.generation
              << " slot=" << status.currentSlot
              << " error=" << status.lastError << "\n";
}

// 지금 바로 send()해도 안전한 상태인지 확인한다. 이미 이번 슬롯에서 안전
// 창을 열어둔 상태라면, 그 창이 아직 안 닫혔는지만 빠르게 확인하고 대기
// 없이 바로 통과시킨다 — 이게 "슬롯당 여러 개"의 핵심. 창이 없거나 이미
// 닫혔으면 다음 슬롯 경계까지 기다렸다가 새 창을 연다.
bool SlotAwareTransport::ensureSafeWindow()
{
    // [2026-08-25 추가] 재동기화 양보(quiet window). 상세: 58절.
    if (m_sendsSinceLastRx >= kQuietTriggerSends) {
        std::cout << "[fhss_ota][slot_tx] " << m_sendsSinceLastRx
                   << "회 연속 무응답 — ESP32 재동기화를 위해 "
                   << kResyncQuietMs << "ms 동안 송신 중단\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(kResyncQuietMs));
        m_sendsSinceLastRx = 0;
        // 조용히 있는 사이 슬롯이 여러 번 바뀌었으므로 창은 무효.
        m_openWindowSlot = kNoWindow;
    }

    // [2026-08-24 추가] 반이중 왕복 보호 — 직전 전송 이후 kMinPacketGapMs가
    // 안 지났으면 그만큼 기다린다. 상세: 55절.
    if (m_lastSentAt.time_since_epoch().count() != 0) {
        for (;;) {
            const int64_t sinceLastMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - m_lastSentAt).count();
            if (sinceLastMs >= kMinPacketGapMs)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(kSlotPollMs));
        }
    }

    if (m_openWindowSlot != kNoWindow) {
        const auto status = m_transport.getFhssStatus();
        const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_windowOpenedAt).count();
        const bool stillSameSlot = usable(status) && status.currentSlot == m_openWindowSlot;
        const bool stillHasRoom =
            elapsedMs + kTailGuardMs < m_slotDurationMs - kPostSyncGuardMs;
        if (stillSameSlot && stillHasRoom)
            return true; // 대기 없이 통과 — 슬롯당 여러 개 보내지는 지점

        m_openWindowSlot = kNoWindow; // 창 닫힘(슬롯 바뀜/여유 부족/동기화 깨짐) -> 새로 게이팅
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kSlotGateTimeoutMs);
    auto status = m_transport.getFhssStatus();
    if (!usable(status)) {
        logGateFailure("initial status", status);
        return false;
    }

    const uint64_t baselineSlot = status.currentSlot;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSlotPollMs));
        status = m_transport.getFhssStatus();
        if (!usable(status)) {
            logGateFailure("status while waiting", status);
            return false;
        }
        if (status.currentSlot == baselineSlot)
            continue;

        const uint64_t sendSlot = status.currentSlot;
        std::this_thread::sleep_for(std::chrono::milliseconds(kPostSyncGuardMs));
        const auto verified = m_transport.getFhssStatus();
        if (!usable(verified) || verified.currentSlot != sendSlot)
            continue;

        m_openWindowSlot = sendSlot;
        m_windowOpenedAt = std::chrono::steady_clock::now();
        std::cout << "[fhss_ota][slot_tx] new window slot=" << sendSlot
                  << " channel=" << static_cast<unsigned>(verified.currentChannel)
                  << " post_sync_ms=" << kPostSyncGuardMs << "\n";
        return true;
    }

    std::cerr << "[fhss_ota][slot_tx] safe slot timeout after "
              << kSlotGateTimeoutMs << "ms\n";
    return false;
}
