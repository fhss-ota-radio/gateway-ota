// 실기기(CC1101) 스모크테스트 — OtaSession(FSM) 기반 송신 CLI.
//
// smoke_send_main.cpp(ota_smoke_send)와 하는 일은 같지만(파일을 실기기로
// 전송), 안쪽 로직이 다릅니다:
//   - smoke_send_main.cpp: performHandshake() + sendDataAndEnd() — 핸드셰이크는
//     응답을 기다리지만, DATA는 ACK 없이 순서대로 쏘기만 하는 "단순 전송"
//   - 이 파일: session/otasession.h의 OtaSession — 배치(윈도우) 단위로 개별
//     ACK/NACK을 확인하고, 누락된 청크만 선택적으로 재전송하는 신뢰성 있는 전송
//     (docs/fsm-design.md §6 알고리즘)
//
// OtaSession은 지금까지 tests/tst_otasession.cpp에서 FakeTransport(인메모리)로만
// 검증됐습니다 — 이 파일이 실기기(CC1101)로 처음 돌려보는 진입점입니다.
//
// 수신측은 새 프로그램이 필요 없습니다 — 기존 ota_smoke_recv(smoke_recv_main.cpp)가
// 이미 DATA/START/END를 받을 때마다 개별 ACK를 반사적으로 보내고 있어서
// (session/simplereceiver.h의 sendAckFor()), OtaSession이 기대하는 응답 방식과
// 그대로 맞습니다.
//
// 사용법:
//   ota_smoke_session_send <device_path> <bin_file> [target_device_id_hex] [batchSize] [chunkDelayMs]
//
//   target_device_id_hex  생략 시 브로드캐스트(ffffffff)
//   batchSize             생략 시 5 (docs/fsm-design.md 결정값)
//   chunkDelayMs          생략 시 40 (2026-08-17 실기기 검증으로 확정된 값)

#include "cc1101transport.h"
#include "otasession.h"
#include "teelogger.h" // gw_log_YYYYMMDD_HHMMSS.txt 자동 저장 (teelogger.h 상단 주석 참고)

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

// smoke_send_main.cpp의 parseHexDeviceId()와 동일 (2026-08-17에 브로드캐스트
// 파싱 버그를 고친 그 로직) — 파일마다 중복이지만, 얇은 CLI 진입점끼리
// 헤더 하나를 공유하게 만들 만큼 무겁지 않다고 판단해 각자 둠(simplesender.h
// 쪽 함수들과 성격이 다름 — 이건 CLI 파싱 유틸일 뿐 세션 로직이 아님).
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

void printUsage(const char *argv0)
{
    std::cerr << "사용법: " << argv0
              << " <device_path> <bin_file> [target_device_id_hex] [batchSize] [chunkDelayMs]"
                 " [timeoutMs] [maxRetry]\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin\n"
              << "  예: " << argv0 << " /dev/cc1101 firmware.bin ffffffff 5 40\n"
              << "  예(여유값, 2026-08-20 실기기에서 stale NACK로 재시도 한도 초과 관찰돼\n"
              << "     추가됨): " << argv0 << " /dev/cc1101 firmware.bin ffffffff 5 40 600 8\n";
}

} // namespace

int main(int argc, char *argv[])
{
    TeeLogger logger("gw_log"); // 맨 처음(사용법 오류 포함) — smoke_fhss_ota_transfer_main.cpp와 동일 이유

    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string devicePath = argv[1];
    const std::string binFile = argv[2];

    uint32_t targetDeviceId = OTA_BROADCAST_DEVICE_ID;
    if (argc >= 4 && !parseHexDeviceId(argv[3], &targetDeviceId)) {
        std::cerr << "target_device_id_hex 파싱 실패: " << argv[3] << "\n";
        return 1;
    }

    const int batchSize = (argc >= 5) ? std::atoi(argv[4]) : 5;
    const int chunkDelayMs = (argc >= 6) ? std::atoi(argv[5]) : 40;
    // [2026-08-20 추가] 기본값(300ms/5회, fsm-design.md 결정값)은 ESP32의
    // "아직 못 받음" NACK 재전송 주기(500ms)보다 짧아서, 실기기에서 둘의
    // 리듬이 어긋나며 재시도 예산을 너무 빨리 써버리는 경우가 관찰됨
    // (design-notes 37절). 여유값이 필요하면 인자로 넘길 수 있게 함.
    const int timeoutMs = (argc >= 7) ? std::atoi(argv[6]) : 300;
    const int maxRetry = (argc >= 8) ? std::atoi(argv[7]) : 5;

    std::cout << "[smoke_session_send] device=" << devicePath << " file=" << binFile
              << " target=0x" << std::hex << targetDeviceId << std::dec
              << " batchSize=" << batchSize << " chunkDelayMs=" << chunkDelayMs
              << " timeoutMs=" << timeoutMs << " maxRetry=" << maxRetry << "\n";

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_session_send] transport open 실패: " << devicePath << "\n";
        return 1;
    }
    std::cout << "[smoke_session_send] transport open 성공\n";

    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[smoke_session_send] startRx 실패 — 응답 수신이 안 될 수 있음(계속 진행함)\n";

    OtaSession session(transport, batchSize, timeoutMs, maxRetry, chunkDelayMs);

    // 상태 전이마다 로그 — fsm-design.md §1 "화면과 로직의 경계" 원칙대로
    // OtaSession 자신은 콜백만 올리고, 여기(CLI, 화면 대신)서 로그로 소비함.
    session.setOnStateChanged([](OtaSessionState s) {
        std::cout << "[smoke_session_send] 상태 -> " << otaSessionStateName(s) << "\n";
    });
    // [2026-08-20 추가] ACK/NACK 수신·재전송·타임아웃마다 찍는 상세 로그 —
    // 상태 전이 로그만으로는 WaitingBatchAck 안에서 무슨 일이 있었는지
    // (몇 번 seq가 NACK/타임아웃으로 재전송됐는지) 전혀 안 보여서, 실기기
    // 테스트에서 "어디서 막혔는지"를 바로 확인하기 위해 추가함.
    session.setOnLog([](const std::string &msg) {
        std::cout << "[smoke_session_send][log] " << msg << "\n";
    });

    if (!session.start(binFile, targetDeviceId)) {
        std::cerr << "[smoke_session_send] start() 실패: " << session.errorMessage() << "\n";
        transport.close();
        return 1;
    }

    std::cout << "[smoke_session_send] 세션 시작 session_id=0x" << std::hex << session.sessionId()
              << std::dec << "\n";

    // tick()을 10ms 간격으로 반복 호출 — 실제 Qt 앱이라면 QTimer(10ms)가 이
    // 역할을 함. Completed/Failed에 도달할 때까지 반복.
    uint32_t lastAcked = 0;
    while (session.state() != OtaSessionState::Completed
           && session.state() != OtaSessionState::Failed) {
        session.tick(otaSessionNowMs());

        const auto progress = session.progress();
        if (progress.ackedChunks != lastAcked) {
            std::cout << "[smoke_session_send] 진행 " << progress.ackedChunks << "/"
                       << progress.totalChunks << " (배치 " << progress.currentBatchNumber << "/"
                       << progress.totalBatches << ")\n";
            lastAcked = progress.ackedChunks;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    transport.close();

    if (session.state() == OtaSessionState::Completed) {
        std::cout << "[smoke_session_send] 완료 — 전체 " << session.progress().totalChunks
                   << "청크 배치 ACK까지 전부 확인됨\n";
        return 0;
    }

    std::cerr << "[smoke_session_send] 실패: " << session.errorMessage() << "\n";
    return 1;
}
