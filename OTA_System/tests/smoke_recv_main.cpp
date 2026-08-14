// 실기기(CC1101) 스모크테스트 — 단순 수신 CLI (스텁).
//
// "스텁"(stub, 나중에 채울 임시 뼈대)인 이유: 받은 패킷을 콘솔에 보여주기만
// 하고, 보낸 쪽에 ACK/NACK을 돌려보내지 않습니다. 실제 프로토콜 대화(ACK
// 응답, 재전송 요청)는 나중에 만들 OtaSession(FSM)이 담당할 영역이고,
// 지금은 "송신측(ota_smoke_send)이 실제로 뭔가를 보내면 여기서 보이는지"만
// 확인하는 목적입니다.
//
// tst_binsplitter.cpp(ctest로 자동 실행)와 달리 실제 /dev/cc1101 디바이스가
// 있어야 동작하는 수동 실행 도구라서 add_test()에 등록하지 않습니다.
//
// 핵심 수신/디코딩 로직은 session/simplereceiver.h의 tryReceiveOnce()에
// 있고, 이 파일은 그 함수를 반복 호출하며 콘솔에 출력만 하는 얇은 CLI
// 진입점입니다 — smoke_send_main.cpp와 같은 구조.
//
// 사용법:
//   ota_smoke_recv <device_path>
//
//   Ctrl+C로 종료할 때까지 계속 수신 대기하며, 패킷이 도착할 때마다
//   종류와 필드를 콘솔에 출력합니다.

#include "cc1101transport.h"
#include "simplereceiver.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

namespace {

const char *kindToString(ReceivedPacketKind kind)
{
    switch (kind) {
    case ReceivedPacketKind::Start:       return "OTA_START";
    case ReceivedPacketKind::Data:        return "OTA_DATA";
    case ReceivedPacketKind::End:         return "OTA_END";
    case ReceivedPacketKind::Ack:         return "OTA_ACK";
    case ReceivedPacketKind::Nack:        return "OTA_NACK";
    case ReceivedPacketKind::Discover:    return "OTA_DISCOVER";
    case ReceivedPacketKind::DiscoverAck: return "OTA_DISCOVER_ACK";
    case ReceivedPacketKind::Unknown:
    default:
        return "UNKNOWN";
    }
}

void printHex(const std::vector<uint8_t> &data)
{
    for (uint8_t byte : data)
        std::printf("%02X ", byte);
    std::printf("\n");
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path>\n"
                  << "  예: " << argv[0] << " /dev/cc1101\n";
        return 1;
    }

    const std::string devicePath = argv[1];

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_recv] transport open 실패: " << devicePath << "\n";
        return 1;
    }
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[smoke_recv] startRx 실패 — 그래도 수신 대기는 계속 시도함\n";

    std::cout << "[smoke_recv] 수신 대기 시작 (device=" << devicePath << ", Ctrl+C로 종료)\n";

    uint32_t dataCount = 0;
    uint32_t totalChunksHint = 0;

    while (true) {
        const auto packet = tryReceiveOnce(transport);

        // 아직 아무것도 안 왔음 — 에러 아님, 잠깐 쉬고 다시 확인
        if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 뭔가 왔는데 디코딩은 실패함 (길이 이상, CRC 불일치 등)
        if (packet.kind == ReceivedPacketKind::Unknown) {
            std::cout << "[smoke_recv] 디코딩 실패 (" << packet.raw.size() << "byte): ";
            printHex(packet.raw);
            continue;
        }

        switch (packet.kind) {
        case ReceivedPacketKind::Start:
            dataCount = 0;
            totalChunksHint = packet.totalChunks;
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " target=0x" << std::hex << packet.targetDeviceId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks << "\n";
            break;
        case ReceivedPacketKind::Data: {
            ++dataCount;
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " seq=" << packet.sequence
                       << " len=" << static_cast<int>(packet.payloadLength)
                       << " (누적 " << dataCount;
            if (totalChunksHint > 0)
                std::cout << "/" << totalChunksHint;
            std::cout << ")\n";
            break;
        }
        case ReceivedPacketKind::End:
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks
                       << " (실제 받은 DATA 개수=" << dataCount << ")\n";
            break;
        default:
            std::cout << "[smoke_recv] " << kindToString(packet.kind) << "\n";
            break;
        }

        // Start/Data/End만 응답 대상 — 받았다는 확인(ACK)을 바로 돌려보냄.
        // 재전송 판단·대기 없이 "이거 받았다"만 반사적으로 알려주는 것
        // (session/simplereceiver.h의 sendAckFor 주석 참고).
        if (packet.kind == ReceivedPacketKind::Start || packet.kind == ReceivedPacketKind::Data
            || packet.kind == ReceivedPacketKind::End) {
            const bool acked = sendAckFor(transport, packet);
            std::cout << "[smoke_recv]   -> ACK " << (acked ? "전송함" : "전송 실패") << "\n";
        }
    }
}
