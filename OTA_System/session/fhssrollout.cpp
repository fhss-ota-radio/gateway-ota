#include "fhssrollout.h"

#include "itransport.h"
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <thread>

namespace {

// 기기 하나에 FHSS_CONFIG를 보내고 ACK을 기다린다 — otasession.cpp의
// tickHandshaking()과 같은 재시도 패턴(타임아웃되면 재전송, maxRetry
// 넘으면 포기)을 블로킹 루프로 옮긴 것. discoverDevices()처럼 이 파일도
// OtaSession 밖의 독립 흐름이라 tick(nowMs)이 아니라 그냥 블로킹으로 돈다.
bool sendConfigAndWaitAck(ITransport &transport, uint32_t sessionId,
                           uint32_t targetDeviceId, const FhssHopPolicy &policy,
                           int timeoutMs, int maxRetry)
{
    ota_fhss_config_fields_t fields{};
    fields.session_id = sessionId;
    fields.target_device_id = targetDeviceId;
    fields.generation = policy.generation;
    fields.algorithm_version = policy.algorithmVersion;
    fields.channel_profile_id = policy.channelProfileId;
    fields.first_channel = policy.firstChannel;
    fields.channel_count = policy.channelCount;
    fields.rendezvous_channel = policy.rendezvousChannel;
    fields.reserved_channel = policy.reservedChannel;
    fields.seed = policy.seed;
    fields.slot_duration_us = policy.slotDurationUs;
    fields.channel_switch_guard_us = policy.channelSwitchGuardUs;

    uint8_t packet[OTA_FHSS_CONFIG_PACKET_SIZE];
    const size_t written = ota_protocol_encode_fhss_config(packet, sizeof(packet), &fields);
    if (written == 0)
        return false; // 인코딩 실패(정책 값이 ota_fhss_config_is_valid를 못 지나감)

    for (int attempt = 0; attempt <= maxRetry; ++attempt) {
        if (!transport.send(std::vector<uint8_t>(packet, packet + written)))
            return false;

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto received = tryReceiveOnce(transport);
            const bool matches = received.sessionId == sessionId
                && received.acknowledgedType == static_cast<uint8_t>(OTA_PKT_FHSS_CONFIG);

            if (received.kind == ReceivedPacketKind::Ack && matches)
                return true;
            if (received.kind == ReceivedPacketKind::Nack && matches)
                break; // 이번 시도 포기, 바깥 for문에서 재전송

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // 타임아웃 또는 NACK -> 다음 attempt에서 재전송
    }
    return false;
}

// 기기 하나에 FHSS_ACTIVATE를 보내고 ACK을 기다린다 — 구조는 CONFIG와 동일.
bool sendActivateAndWaitAck(ITransport &transport, uint32_t sessionId,
                             uint32_t targetDeviceId, uint32_t generation,
                             int timeoutMs, int maxRetry)
{
    ota_fhss_activate_fields_t fields{};
    fields.session_id = sessionId;
    fields.target_device_id = targetDeviceId;
    fields.generation = generation;

    uint8_t packet[OTA_FHSS_ACTIVATE_PACKET_SIZE];
    const size_t written = ota_protocol_encode_fhss_activate(packet, sizeof(packet), &fields);
    if (written == 0)
        return false;

    for (int attempt = 0; attempt <= maxRetry; ++attempt) {
        if (!transport.send(std::vector<uint8_t>(packet, packet + written)))
            return false;

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto received = tryReceiveOnce(transport);
            const bool matches = received.sessionId == sessionId
                && received.acknowledgedType == static_cast<uint8_t>(OTA_PKT_FHSS_ACTIVATE);

            if (received.kind == ReceivedPacketKind::Ack && matches)
                return true;
            if (received.kind == ReceivedPacketKind::Nack && matches)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return false;
}

} // namespace

std::vector<FhssRolloutOutcome> rolloutFhssConfig(
    ITransport &transport, uint32_t sessionId,
    const std::vector<uint32_t> &targetDeviceIds, const FhssHopPolicy &policy,
    int timeoutMs, int maxRetry)
{
    std::vector<FhssRolloutOutcome> outcomes;
    outcomes.reserve(targetDeviceIds.size());

    // 1단계: 전부 CONFIG부터 끝낸다 (설계 이유는 fhssrollout.h 주석 참고 —
    // 한 기기라도 먼저 ACTIVATE해서 SYNCING에 들어가버리면 안 되기 때문에
    // ACTIVATE는 전원 CONFIG 완료를 확인한 뒤인 2단계에서만 보낸다).
    for (uint32_t deviceId : targetDeviceIds) {
        FhssRolloutOutcome outcome;
        outcome.deviceId = deviceId;
        outcome.stage = sendConfigAndWaitAck(transport, sessionId, deviceId, policy,
                                              timeoutMs, maxRetry)
            ? FhssRolloutStage::ConfigAcked
            : FhssRolloutStage::ConfigFailed;
        outcomes.push_back(outcome);
    }

    // 2단계: CONFIG에 성공한 기기에게만 ACTIVATE.
    for (auto &outcome : outcomes) {
        if (outcome.stage != FhssRolloutStage::ConfigAcked)
            continue;
        if (sendActivateAndWaitAck(transport, sessionId, outcome.deviceId, policy.generation,
                                    timeoutMs, maxRetry))
            outcome.stage = FhssRolloutStage::Activated;
        // 실패하면 ConfigAcked로 그대로 둔다 — "CONFIG는 됐는데 ACTIVATE는
        // 안 됨"이라는 정보 자체가 호출부(재시도 여부 판단)에 의미가 있다.
    }

    return outcomes;
}
