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
//   ota_smoke_send <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms]
//
//   device_path        예: /dev/cc1101
//   bin_file            보낼 파일 경로
//   target_device_id_hex  생략 시 브로드캐스트(OTA_BROADCAST_DEVICE_ID).
//                          특정 기기를 지정하려면 "AABBCC"처럼 hex로 입력
//                          (DISCOVER_ACK로 알아낸 3byte device_id)
//   chunk_delay_ms       생략 시 10ms. 청크 사이 대기 시간.

#include "cc1101transport.h"
#include "simplesender.h"

extern "C" {
#include "ota_protocol.h"
}

#include <cstdlib>
#include <iostream>

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
              << " <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms]\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin AABBCC 10\n";
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

    std::cout << "[smoke_send] device=" << devicePath << " file=" << binFile
              << " target=0x" << std::hex << targetDeviceId << std::dec
              << " chunkDelayMs=" << chunkDelayMs << "\n";

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
    transport.close();

    if (!result.success) {
        std::cerr << "[smoke_send] 실패: " << result.errorMessage << "\n";
        return 1;
    }

    std::cout << "[smoke_send] 완료 — session_id=0x" << std::hex << result.sessionId << std::dec
              << " totalChunks=" << result.totalChunks << "\n";
    return 0;
}
