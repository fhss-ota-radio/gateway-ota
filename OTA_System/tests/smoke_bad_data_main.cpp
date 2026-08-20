// 실기기(CC1101) 스모크테스트 — 일부러 잘못된 DATA를 보내서 수신측이
// 진짜 NACK을 돌려주는지 확인하는 CLI.
//
// [왜 필요한가] session/simplereceiver.cpp의 sendAckFor()가 resultCode와
// 무관하게 항상 OTA_PKT_ACK로만 인코딩하던 버그를 2026-08-19에 찾아
// 고쳤다(유닛테스트 tests/tst_simplereceiver.cpp로 검증). 근데 그건
// FakeTransport로 만든 바이트를 직접 검사한 것뿐이고, 실제로 CC1101 RF
// 링크로 "깨진 패킷 -> 진짜 NACK 응답"이 왕복하는지는 실기기로 한 번도
// 확인 안 됐다 — 이 파일이 그걸 확인하는 도구다.
//
// ota_smoke_session_send/OtaSession을 그대로 쓰지 않는 이유: OtaSession은
// 항상 올바른 CRC로 패킷을 만들기 때문에 "일부러 깨뜨리는" 시나리오를
// 만들 수가 없다. 그래서 이 파일은 OtaSession을 거치지 않고
// ota_protocol_encode_data()로 만든 패킷의 CRC 필드를 직접 손상시켜서
// 보낸다 — session/simplereceiver.h를 테스트하기 위한 전용 도구.
//
// 사용법:
//   ota_smoke_bad_data <device_path> [target_device_id_hex]
//
//   1) OTA_START(totalChunks=5, session_id 랜덤)를 정상적으로 보냄
//   2) seq=0 DATA를 CRC를 일부러 깨서 보냄 -> NACK(INVALID_CRC) 기대
//   3) seq=999(범위 밖) DATA를 정상 CRC로 보냄 -> NACK(INVALID_SEQUENCE) 기대
//   3초간 응답을 기다리며 받은 걸 전부 출력.
//
// 수신측(ota_smoke_recv)을 먼저 띄워둔 상태에서 실행할 것.

#include "cc1101transport.h"
#include "simplereceiver.h"
#include "simplesender.h" // generateSessionId()

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool parseHexDeviceId(const std::string &text, uint32_t *out)
{
    if (text.empty())
        return false;
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 16);
        if (consumed != text.size())
            return false;
        if (value != OTA_BROADCAST_DEVICE_ID && value > OTA_DEVICE_ID_MAX)
            return false;
        *out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

const char *kindToString(ReceivedPacketKind kind)
{
    switch (kind) {
    case ReceivedPacketKind::Ack:  return "OTA_ACK";
    case ReceivedPacketKind::Nack: return "OTA_NACK";
    default:                       return "그 외";
    }
}

const char *resultCodeToString(uint8_t code)
{
    switch (static_cast<ota_result_t>(code)) {
    case OTA_RESULT_OK:               return "OK";
    case OTA_RESULT_INVALID_TYPE:     return "INVALID_TYPE";
    case OTA_RESULT_INVALID_SESSION:  return "INVALID_SESSION";
    case OTA_RESULT_INVALID_TARGET:   return "INVALID_TARGET";
    case OTA_RESULT_INVALID_SIZE:     return "INVALID_SIZE";
    case OTA_RESULT_INVALID_SEQUENCE: return "INVALID_SEQUENCE";
    case OTA_RESULT_INVALID_CRC:      return "INVALID_CRC";
    case OTA_RESULT_WRITE_FAILED:     return "WRITE_FAILED";
    case OTA_RESULT_VERIFY_FAILED:    return "VERIFY_FAILED";
    case OTA_RESULT_BUSY:             return "BUSY";
    case OTA_RESULT_TIMEOUT:          return "TIMEOUT";
    default:                          return "?";
    }
}

// 응답을 최대 waitMs만큼 기다리며 도착하는 대로 출력. 어떤 seq/resultCode로
// 왔는지가 핵심 확인 대상.
void waitAndPrintResponses(ITransport &transport, int waitMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto packet = tryReceiveOnce(transport);
        if (packet.kind == ReceivedPacketKind::Ack || packet.kind == ReceivedPacketKind::Nack) {
            std::cout << "  <- " << kindToString(packet.kind) << " session=0x" << std::hex
                       << packet.sessionId << std::dec << " seq=" << packet.sequence
                       << " result=" << resultCodeToString(packet.resultCode) << "\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path> [target_device_id_hex]\n";
        return 1;
    }

    const std::string devicePath = argv[1];
    uint32_t targetDeviceId = OTA_BROADCAST_DEVICE_ID;
    if (argc >= 3 && !parseHexDeviceId(argv[2], &targetDeviceId)) {
        std::cerr << "target_device_id_hex 파싱 실패: " << argv[2] << "\n";
        return 1;
    }

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "transport open 실패: " << devicePath << "\n";
        return 1;
    }
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "startRx 실패 — 그래도 계속 진행\n";

    const uint32_t sessionId = generateSessionId();
    std::cout << "[bad_data] session_id=0x" << std::hex << sessionId << std::dec << "\n";

    // 1) 정상 OTA_START (totalChunks=5로 작게 — seq=999가 확실히 범위 밖이 되도록)
    {
        ota_start_fields_t fields{};
        fields.session_id = sessionId;
        fields.target_device_id = targetDeviceId;
        fields.image_size = 5 * OTA_MAX_PAYLOAD_SIZE;
        fields.total_chunks = 5;
        // image_sha256은 이 테스트 목적과 무관 — 0으로 둠(수신측은 END까지
        // 안 갈 거라 검증 자체가 안 일어남).

        uint8_t packet[OTA_START_PACKET_SIZE];
        const size_t written = ota_protocol_encode_start(packet, sizeof(packet), &fields);
        transport.send(std::vector<uint8_t>(packet, packet + written));
        std::cout << "[bad_data] OTA_START 전송 (totalChunks=5)\n";
    }
    waitAndPrintResponses(transport, 500); // START ACK 확인 (참고용)

    // 2) seq=0 DATA를 CRC 깨뜨려서 전송 -> NACK(INVALID_CRC) 기대
    {
        uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t packet[OTA_DATA_HEADER_SIZE + 8];
        const size_t written =
            ota_protocol_encode_data(packet, sizeof(packet), sessionId, /*sequence=*/0, payload, 8);
        // CRC16 필드(offset 10~11)를 일부러 뒤집음 — payload는 정상이지만
        // 이제 crc16 검증에 실패하게 됨.
        packet[10] ^= 0xFFu;
        packet[11] ^= 0xFFu;
        transport.send(std::vector<uint8_t>(packet, packet + written));
        std::cout << "[bad_data] seq=0 DATA (CRC 고의 손상) 전송 -> NACK(INVALID_CRC) 기대\n";
    }
    waitAndPrintResponses(transport, 1500);

    // 3) seq=999(범위 밖) DATA를 정상 CRC로 전송 -> NACK(INVALID_SEQUENCE) 기대
    {
        uint8_t payload[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        uint8_t packet[OTA_DATA_HEADER_SIZE + 8];
        const size_t written = ota_protocol_encode_data(packet, sizeof(packet), sessionId,
                                                          /*sequence=*/999, payload, 8);
        transport.send(std::vector<uint8_t>(packet, packet + written));
        std::cout << "[bad_data] seq=999(범위 밖) DATA 전송 -> NACK(INVALID_SEQUENCE) 기대\n";
    }
    waitAndPrintResponses(transport, 1500);

    transport.close();
    std::cout << "[bad_data] 종료\n";
    return 0;
}
