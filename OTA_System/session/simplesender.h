#ifndef SIMPLESENDER_H
#define SIMPLESENDER_H

#include <cstdint>
#include <functional>
#include <string>

class ITransport;

// [의도적으로 Qt 의존성 없음] binsplitter.h와 같은 이유 — 화면 없이도
// g++/clang++로 컴파일·테스트할 수 있어야 스모크테스트(tests/smoke_send_main.cpp)에서
// 실기기 없이도 로직만 검증할 수 있음.

// 진행 상황 콜백에 넘기는 값. 화면(otamanager.cpp)에서 재사용할 때는 이 값을
// 그대로 진행률 바(0~100%)로 환산하면 됨: sentChunks * 100 / totalChunks.
struct SimpleSendProgress
{
    uint32_t sentChunks = 0;
    uint32_t totalChunks = 0;
};

struct SimpleSendResult
{
    bool success = false;
    std::string errorMessage;
    uint32_t sessionId = 0;    // 실제로 사용된 session_id (0을 넘겼다면 내부 생성값)
    uint32_t totalChunks = 0;
};

// OTA_START -> OTA_DATA(전부) -> OTA_END를 순서대로 한 번에 쏘는 "단순 전송" 루틴.
//
// 왜 "단순"인가: ACK/NACK 대기, 재전송, 타임아웃 처리를 전혀 하지 않습니다.
// 이 루틴의 목적은 회선(Cc1101Transport)과 프로토콜 인코딩이 실기기에서 실제로
// 동작하는지 확인하는 스모크테스트이지, 신뢰성 있는 전송이 아닙니다. 신뢰성
// 있는 전송(배치 단위 ACK, 실패 시 재전송)은 OtaSession(FSM, docs/fsm-design.md)의
// 몫이며, 그 구현이 이 함수를 대체할 예정입니다. 그때도 "START 만들기 -> DATA
// 채워넣기 -> END 만들기"라는 뼈대 자체는 재사용 가능하도록 여기 core/에
// 분리해뒀습니다(요청/응답 대기 로직만 얹으면 됨).
//
// transport: 이미 open()된 상태여야 함 (open 여부는 이 함수가 확인만 하고
//            직접 열지 않음 — 연결 수명 관리는 호출자 책임).
// filePath: 보낼 .bin 파일 경로.
// targetDeviceId: OTA_START.target_device_id. 특정 기기를 지정하려면
//                 DISCOVER_ACK로 알아낸 device_id를, 모든 기기 대상이면
//                 OTA_BROADCAST_DEVICE_ID를 넘김.
// sessionId: 0을 넘기면 내부에서 랜덤 생성. 재현 가능한 테스트가 필요하면
//            직접 값을 지정할 수 있음.
// chunkSize: -1이면 OTA_MAX_PAYLOAD_SIZE(48byte) 사용.
// chunkDelayMs: DATA 패킷 사이에 넣을 대기 시간(ms). CC1101 드라이버가 이전
//               write()를 처리할 시간을 벌어주기 위함 — 너무 빨리 연속으로
//               쏘면 드라이버/하드웨어 FIFO가 못 따라가 유실될 수 있음. 0이면
//               딜레이 없이 연속 전송.
// onProgress: 청크 하나 보낼 때마다 호출되는 콜백(생략 가능).
//
// 반환값: image_sha256은 아직 채우지 않고 32byte 전부 0으로 보냅니다 — 이
// 함수는 회선 확인용 스모크테스트이고, 실제 무결성 검증(수신측 SHA-256 대조)은
// OtaSession 구현 시점에 붙일 예정이라 지금은 의미 없는 값입니다(수신측도
// 아직 검증 안 함).
SimpleSendResult simpleSendFile(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId = 0,
    int chunkSize = -1,
    int chunkDelayMs = 10,
    const std::function<void(const SimpleSendProgress &)> &onProgress = nullptr);

#endif // SIMPLESENDER_H
