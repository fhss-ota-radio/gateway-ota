// [임시 검증용, 2026-08-14] smoke_send_main.cpp와 완전히 동일한 로직이지만,
// Cc1101Transport(/dev/cc1101, 커널 드라이버 필요) 대신 SpidevTransport
// (/dev/spidevX.Y, 커널 드라이버 불필요)를 씁니다. 커널 드라이버가 이
// 라즈베리파이들에서 못 올라가는 동안 실제 gateway-ota 프로토콜 로직
// (performHandshake/sendDataAndEnd)을 실기기로 검증하기 위한 용도입니다.
// transport/spidevtransport.h 상단 주석 참고.
//
// 사용법:
//   ota_smoke_spidev_send <spidev_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]
//   예: ota_smoke_spidev_send /dev/spidev0.0 firmware.bin

#include "spidevtransport.h"
#include "simplereceiver.h"
#include "simplesender.h"

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdlib>
#include <iostream>
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
        if (value > OTA_DEVICE_ID_MAX)
            return false;
        *out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

const char *ackKindToString(ReceivedPacketKind kind)
{
    switch (kind) {
    case ReceivedPacketKind::Ack:  return "ACK";
    case ReceivedPacketKind::Nack: return "NACK";
    default:                       return "?";
    }
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 3) {
        std::cerr << "사용법: " << argv[0]
                  << " <spidev_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]\n"
                  << "  예: " << argv[0] << " /dev/spidev0.0 firmware.bin\n";
        return 1;
    }

    const std::string devicePath = argv[1];
    const std::string binFile = argv[2];

    uint32_t targetDeviceId = OTA_BROADCAST_DEVICE_ID;
    if (argc >= 4 && !parseHexDeviceId(argv[3], &targetDeviceId)) {
        std::cerr << "target_device_id_hex 파싱 실패: " << argv[3] << "\n";
        return 1;
    }

    int chunkDelayMs = 10;
    if (argc >= 5)
        chunkDelayMs = std::atoi(argv[4]);

    int ackListenMs = 2000;
    if (argc >= 6)
        ackListenMs = std::atoi(argv[5]);

    std::cout << "[smoke_spidev_send] device=" << devicePath << " file=" << binFile
              << " target=0x" << std::hex << targetDeviceId << std::dec
              << " chunkDelayMs=" << chunkDelayMs << " ackListenMs=" << ackListenMs << "\n";

    SpidevTransport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_spidev_send] transport open 실패: " << devicePath << "\n";
        return 1;
    }
    std::cout << "[smoke_spidev_send] transport open 성공 (open() 안에서 리셋+설정+RX진입까지 끝남)\n";

    // ---------- 1. 핸드셰이크 (OTA_START, 응답 대기) ----------
    std::cout << "[smoke_spidev_send] 핸드셰이크 시작 (OTA_START 전송, 최대 5회 재시도)...\n";
    const auto handshake = performHandshake(transport, binFile, targetDeviceId);

    if (!handshake.success) {
        std::cerr << "[smoke_spidev_send] 핸드셰이크 실패: " << handshake.errorMessage << "\n";
        transport.close();
        return 1;
    }
    std::cout << "[smoke_spidev_send] 핸드셰이크 성공 — session_id=0x" << std::hex << handshake.sessionId
              << std::dec << " totalChunks=" << handshake.totalChunks << "\n";

    // ---------- 2. DATA 전부 + OTA_END ----------
    const auto result = sendDataAndEnd(
        transport, binFile, handshake.sessionId, handshake.imageSize, handshake.totalChunks,
        /*chunkSize=*/-1, chunkDelayMs,
        [](const SimpleSendProgress &progress) {
            std::cout << "[smoke_spidev_send] 전송 중: " << progress.sentChunks << "/"
                      << progress.totalChunks << "\r" << std::flush;
        });

    std::cout << "\n";

    if (!result.success) {
        std::cerr << "[smoke_spidev_send] 실패: " << result.errorMessage << "\n";
        transport.close();
        return 1;
    }

    std::cout << "[smoke_spidev_send] 완료 — session_id=0x" << std::hex << result.sessionId << std::dec
              << " totalChunks=" << result.totalChunks << "\n";

    std::cout << "[smoke_spidev_send] 응답(ACK) 확인 중 (최대 " << ackListenMs << "ms)...\n";
    int ackSeen = 0;
    const auto listenUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(ackListenMs);
    while (std::chrono::steady_clock::now() < listenUntil) {
        const auto packet = tryReceiveOnce(transport);

        if (packet.kind == ReceivedPacketKind::Ack || packet.kind == ReceivedPacketKind::Nack) {
            ++ackSeen;
            std::cout << "[smoke_spidev_send]   " << ackKindToString(packet.kind)
                      << " session=0x" << std::hex << packet.sessionId << std::dec
                      << " seq=" << packet.sequence
                      << " result=" << static_cast<int>(packet.resultCode) << "\n";
            continue;
        }
        if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::cout << "[smoke_spidev_send] 응답 확인 종료 — 총 " << ackSeen << "개 수신\n";

    transport.close();
    return 0;
}
