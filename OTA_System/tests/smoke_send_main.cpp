// 실기기(CC1101) 스모크테스트 — 단순 송신 CLI.
//
// tst_binsplitter.cpp(ctest로 자동 실행되는 assert 기반 단위테스트)와 달리
// 이 파일은 실제 /dev/cc1101 디바이스가 있어야 동작하는 수동 실행 도구라서
// add_test()에 등록하지 않습니다 — CI/일반 빌드 검증에는 안 끼고, 실기기
// 앞에서 사람이 직접 실행할 때만 씀.
//
// 핵심 전송 로직(OTA_START -> DATA 전부 -> OTA_END)은 core/simplesender.h의
// simpleSendFile()에 있고, 이 파일은 그 함수를 호출하는 얇은 CLI 진입점일
// 뿐입니다 — 나중에 OtaSession(FSM)이 같은 core 로직을 재사용할 수 있도록
// 로직과 진입점을 분리해뒀습니다.
//
// 사용법:
//   ota_smoke_send <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]
//
//   device_path        예: /dev/cc1101
//   bin_file            보낼 파일 경로
//   target_device_id_hex  생략 시 브로드캐스트(OTA_BROADCAST_DEVICE_ID).
//                          특정 기기를 지정하려면 "AABBCC"처럼 hex로 입력
//                          (DISCOVER_ACK로 알아낸 3byte device_id)
//   chunk_delay_ms       생략 시 10ms. 청크 사이 대기 시간.
//   ack_listen_ms        생략 시 2000ms. 전부 보낸 뒤 받는 쪽(ota_smoke_recv)이
//                         돌려보내는 ACK를 이 시간만큼 기다리며 화면에 보여줌
//                         (기다리는 동안 재전송 같은 건 안 함 — 그냥 보기만 함).

#include "cc1101transport.h"
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

// "AABBCC" 같은 hex 문자열을 uint32_t로 파싱. 실패하면 false.
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

void printUsage(const char *argv0)
{
    std::cerr << "사용법: " << argv0
              << " <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin AABBCC 10 2000\n";
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
        printUsage(argv[0]);
        return 1;
    }

    const std::string devicePath = argv[1];
    const std::string binFile = argv[2];

    uint32_t targetDeviceId = OTA_BROADCAST_DEVICE_ID;
    if (argc >= 4) {
        if (!parseHexDeviceId(argv[3], &targetDeviceId)) {
            std::cerr << "target_device_id_hex 파싱 실패: " << argv[3] << "\n";
            return 1;
        }
    }

    int chunkDelayMs = 10;
    if (argc >= 5)
        chunkDelayMs = std::atoi(argv[4]);

    int ackListenMs = 2000;
    if (argc >= 6)
        ackListenMs = std::atoi(argv[5]);

    std::cout << "[smoke_send] device=" << devicePath << " file=" << binFile
              << " target=0x" << std::hex << targetDeviceId << std::dec
              << " chunkDelayMs=" << chunkDelayMs << " ackListenMs=" << ackListenMs << "\n";

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_send] transport open 실패: " << devicePath << "\n";
        return 1;
    }
    std::cout << "[smoke_send] transport open 성공\n";

    const auto result = simpleSendFile(
        transport, binFile, targetDeviceId, /*sessionId=*/0, /*chunkSize=*/-1, chunkDelayMs,
        [](const SimpleSendProgress &progress) {
            std::cout << "[smoke_send] 전송 중: " << progress.sentChunks << "/"
                      << progress.totalChunks << "\r" << std::flush;
        });

    std::cout << "\n";

    if (!result.success) {
        std::cerr << "[smoke_send] 실패: " << result.errorMessage << "\n";
        transport.close();
        return 1;
    }

    std::cout << "[smoke_send] 완료 — session_id=0x" << std::hex << result.sessionId << std::dec
              << " totalChunks=" << result.totalChunks << "\n";

    // ---- 여기부터는 "확인용" ----
    // 다 보낸 뒤, 받는 쪽(ota_smoke_recv)이 즉시 돌려보내는 ACK가 있는지
    // 잠깐 들어봅니다. 여기서 뭘 판단하거나 재전송하지 않습니다 — 그냥
    // 화면에 보여주기만 함(그 판단은 OtaSession 몫). 다 보내고 나서야
    // 듣기 시작하므로, 보내는 도중에는 칩 상태를 안 건드립니다.
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[smoke_send] startRx 실패 — ACK 확인을 못 할 수 있음\n";

    std::cout << "[smoke_send] 응답(ACK) 확인 중 (최대 " << ackListenMs << "ms)...\n";
    int ackSeen = 0;
    const auto listenUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(ackListenMs);
    while (std::chrono::steady_clock::now() < listenUntil) {
        const auto packet = tryReceiveOnce(transport);

        if (packet.kind == ReceivedPacketKind::Ack || packet.kind == ReceivedPacketKind::Nack) {
            ++ackSeen;
            std::cout << "[smoke_send]   " << ackKindToString(packet.kind)
                      << " session=0x" << std::hex << packet.sessionId << std::dec
                      << " seq=" << packet.sequence
                      << " result=" << static_cast<int>(packet.resultCode) << "\n";
            continue;
        }
        if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // ACK/NACK이 아닌 다른 패킷은 이 프로그램 관심사가 아니므로 무시.
    }
    std::cout << "[smoke_send] 응답 확인 종료 — 총 " << ackSeen << "개 수신\n";

    transport.close();
    return 0;
}
