// 실기기(CC1101) 스모크테스트 — 핸드셰이크 + 송신 CLI.
//
// tst_binsplitter.cpp(ctest로 자동 실행되는 assert 기반 단위테스트)와 달리
// 이 파일은 실제 /dev/cc1101 디바이스가 있어야 동작하는 수동 실행 도구라서
// add_test()에 등록하지 않습니다 — CI/일반 빌드 검증에는 안 끼고, 실기기
// 앞에서 사람이 직접 실행할 때만 씀.
//
// 흐름 (docs/fsm-design.md의 HANDSHAKING 상태를 그대로 따름), 둘 다
// session/simplesender.h에 있음(별도 handshake 파일로 뺐다가 파일 개수를
// 늘리지 않기 위해 다시 합침):
//   1. performHandshake() — OTA_START를 보내고 응답이 올 때까지 기다림
//      (타임아웃 시 재시도, 기본 300ms x 5회)
//   2. 핸드셰이크 성공 시에만 sendDataAndEnd()로 DATA 전부 + OTA_END 전송
//      (이 단계는 여전히 청크마다 응답을 기다리지 않는 "단순" 전송 — 그건
//      다음 단계인 배치 확인/재전송 로직의 몫)
// 이 파일 자체는 위 두 함수를 순서대로 부르는 얇은 CLI 진입점일 뿐입니다.
//
// 사용법:
//   ota_smoke_send <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]
//
//   device_path        예: /dev/cc1101
//   bin_file            보낼 파일 경로
//   target_device_id_hex  생략 시 브로드캐스트(OTA_BROADCAST_DEVICE_ID).
//                          특정 기기를 지정하려면 "AABBCC"처럼 hex로 입력
//                          (DISCOVER_ACK로 알아낸 3byte device_id)
//   chunk_delay_ms       생략 시 10ms. DATA 청크 사이 대기 시간.
//   ack_listen_ms        생략 시 2000ms. DATA/END까지 다 보낸 뒤 받는 쪽이
//                         돌려보내는 ACK를 이 시간만큼 더 기다리며 화면에
//                         보여줌(그냥 보기만 함 — 재전송 안 함).

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

    // 핸드셰이크 응답(ACK)을 들으려면 START를 보내기 전부터 RX 상태여야
    // 함 — 이전(핸드셰이크 없던) 버전은 다 보낸 뒤에야 RX로 전환했지만,
    // 이제는 START 직후 응답을 기다려야 하므로 순서를 앞으로 옮김.
    // [확인 필요] CC1101이 RX 상태에서 send()(write())가 문제없이 동작하는지는
    // 드라이버 내부 구현에 달려 있어 이 코드만으로는 알 수 없음 — 실기기
    // 테스트에서 확인해야 함(안 되면 드라이버 담당에게 문의).
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[smoke_send] startRx 실패 — 응답 수신이 안 될 수 있음(계속 진행함)\n";

    // ---------- 1. 핸드셰이크 (OTA_START, 응답 대기) ----------
    std::cout << "[smoke_send] 핸드셰이크 시작 (OTA_START 전송, 최대 5회 재시도)...\n";
    const auto handshake = performHandshake(transport, binFile, targetDeviceId);

    if (!handshake.success) {
        std::cerr << "[smoke_send] 핸드셰이크 실패: " << handshake.errorMessage << "\n";
        transport.close();
        return 1;
    }
    std::cout << "[smoke_send] 핸드셰이크 성공 — session_id=0x" << std::hex << handshake.sessionId
              << std::dec << " totalChunks=" << handshake.totalChunks << "\n";

    // ---------- 2. DATA 전부 + OTA_END ----------
    const auto result = sendDataAndEnd(
        transport, binFile, handshake.sessionId, handshake.imageSize, handshake.totalChunks,
        /*chunkSize=*/-1, chunkDelayMs,
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
    // DATA/END까지 다 보낸 뒤, 받는 쪽이 돌려보내는 ACK가 더 있는지 잠깐
    // 들어봅니다. 여기서 뭘 판단하거나 재전송하지 않습니다 — 그냥 화면에
    // 보여주기만 함(그 판단은 OtaSession 몫).
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
