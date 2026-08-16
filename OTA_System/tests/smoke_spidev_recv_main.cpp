// [임시 검증용, 2026-08-14] smoke_recv_main.cpp와 완전히 동일한 로직이지만,
// Cc1101Transport(/dev/cc1101, 커널 드라이버 필요) 대신 SpidevTransport
// (/dev/spidevX.Y, 커널 드라이버 불필요)를 씁니다. transport/spidevtransport.h
// 상단 주석 참고.
//
// 사용법:
//   ota_smoke_spidev_recv <spidev_path>
//   예: ota_smoke_spidev_recv /dev/spidev0.0

#include "spidevtransport.h"
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
        std::cerr << "사용법: " << argv[0] << " <spidev_path>\n"
                  << "  예: " << argv[0] << " /dev/spidev0.0\n";
        return 1;
    }

    const std::string devicePath = argv[1];

    SpidevTransport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_spidev_recv] transport open 실패: " << devicePath << "\n";
        return 1;
    }

    std::cout << "[smoke_spidev_recv] 수신 대기 시작 (device=" << devicePath << ", Ctrl+C로 종료)\n";

    uint32_t dataCount = 0;
    uint32_t totalChunksHint = 0;

    while (true) {
        const auto packet = tryReceiveOnce(transport);

        if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (packet.kind == ReceivedPacketKind::Unknown) {
            std::cout << "[smoke_spidev_recv] 디코딩 실패 (" << packet.raw.size() << "byte): ";
            printHex(packet.raw);
            continue;
        }

        switch (packet.kind) {
        case ReceivedPacketKind::Start:
            dataCount = 0;
            totalChunksHint = packet.totalChunks;
            std::cout << "[smoke_spidev_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " target=0x" << std::hex << packet.targetDeviceId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks << "\n";
            break;
        case ReceivedPacketKind::Data: {
            ++dataCount;
            std::cout << "[smoke_spidev_recv] " << kindToString(packet.kind)
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
            std::cout << "[smoke_spidev_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks
                       << " (실제 받은 DATA 개수=" << dataCount << ")\n";
            break;
        default:
            std::cout << "[smoke_spidev_recv] " << kindToString(packet.kind) << "\n";
            break;
        }

        if (packet.kind == ReceivedPacketKind::Start || packet.kind == ReceivedPacketKind::Data
            || packet.kind == ReceivedPacketKind::End) {
            const bool acked = sendAckFor(transport, packet);
            std::cout << "[smoke_spidev_recv]   -> ACK " << (acked ? "전송함" : "전송 실패") << "\n";
        }
    }
}
