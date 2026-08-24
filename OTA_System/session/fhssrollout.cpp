#include "fhssrollout.h"

#include "itransport.h"
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

// onLog가 nullptr이면 아무것도 안 하는 no-op — 호출부마다 매번 null 체크를
// 안 써도 되게 하는 흔한 패턴.
void logIfSet(const FhssRolloutLogFn &onLog, const std::string &msg)
{
    if (onLog)
        onLog(msg);
}

// ReceivedPacket 하나를 사람이 읽을 수 있는 한 줄로 요약한다 — kind/raw
// 길이/session_id/acknowledged_type까지, "매칭 안 되는 패킷이라도 뭐라도
// 왔는지"를 보려는 디버깅 목적이라 매칭 여부와 무관하게 최대한 그대로
// 보여준다.
std::string describeReceived(const ReceivedPacket &received)
{
    if (received.kind == ReceivedPacketKind::Unknown && received.raw.empty())
        return {}; // 이번 폴링엔 아무것도 안 옴 — 호출부에서 이 경우 자체를 건너뜀

    const char *kindName = "Unknown(decode 실패/미지원 타입)";
    switch (received.kind) {
    case ReceivedPacketKind::Start:       kindName = "Start"; break;
    case ReceivedPacketKind::Data:        kindName = "Data"; break;
    case ReceivedPacketKind::End:         kindName = "End"; break;
    case ReceivedPacketKind::Ack:         kindName = "Ack"; break;
    case ReceivedPacketKind::Nack:        kindName = "Nack"; break;
    case ReceivedPacketKind::Discover:    kindName = "Discover"; break;
    case ReceivedPacketKind::DiscoverAck: kindName = "DiscoverAck"; break;
    case ReceivedPacketKind::Unknown:     break;
    }

    char buf[192];
    std::snprintf(buf, sizeof(buf),
                   "kind=%s raw_len=%zu session_id=0x%x acknowledged_type=%u",
                   kindName, received.raw.size(),
                   static_cast<unsigned>(received.sessionId),
                   static_cast<unsigned>(received.acknowledgedType));
    return buf;
}

// 기기 하나에 FHSS_CONFIG를 보내고 ACK을 기다린다 — otasession.cpp의
// tickHandshaking()과 같은 재시도 패턴(타임아웃되면 재전송, maxRetry
// 넘으면 포기)을 블로킹 루프로 옮긴 것. discoverDevices()처럼 이 파일도
// OtaSession 밖의 독립 흐름이라 tick(nowMs)이 아니라 그냥 블로킹으로 돈다.
bool sendConfigAndWaitAck(ITransport &transport, uint32_t sessionId,
                           uint32_t targetDeviceId, const FhssHopPolicy &policy,
                           int timeoutMs, int maxRetry, const FhssRolloutLogFn &onLog)
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
    if (written == 0) {
        logIfSet(onLog, "CONFIG encode 실패 — policy 값이 ota_fhss_config_is_valid()를 "
                         "통과 못 함(algorithm_version/channel_count/slot_duration_us/"
                         "channel_switch_guard_us/rendezvous_channel/reserved_channel 중 "
                         "하나가 잘못됨). 이 경우 무선 전송 자체가 시도되지 않는다.");
        return false;
    }

    for (int attempt = 0; attempt <= maxRetry; ++attempt) {
        const bool sendOk = transport.send(std::vector<uint8_t>(packet, packet + written));
        logIfSet(onLog, "CONFIG 시도 " + std::to_string(attempt + 1) + "/"
                             + std::to_string(maxRetry + 1) + " — transport.send()="
                             + (sendOk ? "성공" : "실패(ioctl/드라이버 단계에서 거부)"));
        if (!sendOk)
            return false; // send 자체가 실패하면 재시도 없이 즉시 포기(드라이버/장치 레벨 오류)

        int packetsSeenThisAttempt = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto received = tryReceiveOnce(transport);
            const bool matches = received.sessionId == sessionId
                && received.acknowledgedType == static_cast<uint8_t>(OTA_PKT_FHSS_CONFIG);

            const std::string desc = describeReceived(received);
            if (!desc.empty()) {
                ++packetsSeenThisAttempt;
                logIfSet(onLog, "  수신: " + desc + (matches ? " (매칭됨)" : " (매칭 안 됨)"));
            }

            if (received.kind == ReceivedPacketKind::Ack && matches)
                return true;
            if (received.kind == ReceivedPacketKind::Nack && matches) {
                logIfSet(onLog, "  NACK 수신 — 이번 시도 포기하고 재전송");
                break; // 이번 시도 포기, 바깥 for문에서 재전송
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (packetsSeenThisAttempt == 0) {
            logIfSet(onLog, "  " + std::to_string(timeoutMs)
                                 + "ms 동안 아무 패킷도 안 들어옴(완전 무응답)");
        }
        // 타임아웃 또는 NACK -> 다음 attempt에서 재전송
    }
    return false;
}

// 기기 하나에 FHSS_ACTIVATE를 보내고 ACK을 기다린다 — 구조는 CONFIG와 동일.
bool sendActivateAndWaitAck(ITransport &transport, uint32_t sessionId,
                             uint32_t targetDeviceId, uint32_t generation,
                             int timeoutMs, int maxRetry, const FhssRolloutLogFn &onLog)
{
    ota_fhss_activate_fields_t fields{};
    fields.session_id = sessionId;
    fields.target_device_id = targetDeviceId;
    fields.generation = generation;

    uint8_t packet[OTA_FHSS_ACTIVATE_PACKET_SIZE];
    const size_t written = ota_protocol_encode_fhss_activate(packet, sizeof(packet), &fields);
    if (written == 0) {
        logIfSet(onLog, "ACTIVATE encode 실패 — policy 값이 유효성 검증을 통과 못 함.");
        return false;
    }

    for (int attempt = 0; attempt <= maxRetry; ++attempt) {
        const bool sendOk = transport.send(std::vector<uint8_t>(packet, packet + written));
        logIfSet(onLog, "ACTIVATE 시도 " + std::to_string(attempt + 1) + "/"
                             + std::to_string(maxRetry + 1) + " — transport.send()="
                             + (sendOk ? "성공" : "실패(ioctl/드라이버 단계에서 거부)"));
        if (!sendOk)
            return false;

        int packetsSeenThisAttempt = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto received = tryReceiveOnce(transport);
            const bool matches = received.sessionId == sessionId
                && received.acknowledgedType == static_cast<uint8_t>(OTA_PKT_FHSS_ACTIVATE);

            const std::string desc = describeReceived(received);
            if (!desc.empty()) {
                ++packetsSeenThisAttempt;
                logIfSet(onLog, "  수신: " + desc + (matches ? " (매칭됨)" : " (매칭 안 됨)"));
            }

            if (received.kind == ReceivedPacketKind::Ack && matches)
                return true;
            if (received.kind == ReceivedPacketKind::Nack && matches) {
                logIfSet(onLog, "  NACK 수신 — 이번 시도 포기하고 재전송");
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (packetsSeenThisAttempt == 0) {
            logIfSet(onLog, "  " + std::to_string(timeoutMs)
                                 + "ms 동안 아무 패킷도 안 들어옴(완전 무응답)");
        }
    }
    return false;
}

} // namespace

std::vector<FhssRolloutOutcome> rolloutFhssConfig(
    ITransport &transport, uint32_t sessionId,
    const std::vector<uint32_t> &targetDeviceIds, const FhssHopPolicy &policy,
    int timeoutMs, int maxRetry, const FhssRolloutLogFn &onLog)
{
    std::vector<FhssRolloutOutcome> outcomes;
    outcomes.reserve(targetDeviceIds.size());

    // 1단계: 전부 CONFIG부터 끝낸다 (설계 이유는 fhssrollout.h 주석 참고 —
    // 한 기기라도 먼저 ACTIVATE해서 SYNCING에 들어가버리면 안 되기 때문에
    // ACTIVATE는 전원 CONFIG 완료를 확인한 뒤인 2단계에서만 보낸다).
    for (uint32_t deviceId : targetDeviceIds) {
        {
            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "%x", deviceId);
            logIfSet(onLog, std::string("== device_id=0x") + idBuf + " CONFIG 시작 ==");
        }
        FhssRolloutOutcome outcome;
        outcome.deviceId = deviceId;
        outcome.stage = sendConfigAndWaitAck(transport, sessionId, deviceId, policy,
                                              timeoutMs, maxRetry, onLog)
            ? FhssRolloutStage::ConfigAcked
            : FhssRolloutStage::ConfigFailed;
        outcomes.push_back(outcome);
    }

    // 2단계: CONFIG에 성공한 기기에게만 ACTIVATE.
    for (auto &outcome : outcomes) {
        if (outcome.stage != FhssRolloutStage::ConfigAcked)
            continue;
        {
            char idBuf[16];
            std::snprintf(idBuf, sizeof(idBuf), "%x", outcome.deviceId);
            logIfSet(onLog, std::string("== device_id=0x") + idBuf + " ACTIVATE 시작 ==");
        }
        if (sendActivateAndWaitAck(transport, sessionId, outcome.deviceId, policy.generation,
                                    timeoutMs, maxRetry, onLog))
            outcome.stage = FhssRolloutStage::Activated;
        // 실패하면 ConfigAcked로 그대로 둔다 — "CONFIG는 됐는데 ACTIVATE는
        // 안 됨"이라는 정보 자체가 호출부(재시도 여부 판단)에 의미가 있다.
    }

    return outcomes;
}
