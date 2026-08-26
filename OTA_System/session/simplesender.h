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

// simpleSendFile()의 "OTA_START 이후" 부분만 떼어낸 함수 — DATA 전부 보내고
// OTA_END까지 보냅니다. OTA_START는 이미 다른 곳(아래 performHandshake())에서
// 보내고 확인까지 받은 뒤, 같은 세션으로 이어서 DATA/END만 보내고 싶을 때
// 씁니다. simpleSendFile()도 내부적으로 이 함수를 그대로 재사용합니다
// (중복 방지) — simpleSendFile()의 동작은 이 리팩터링 전후로 완전히
// 동일합니다.
//
// imageSize/totalChunks: 호출부가 이미 알고 있는 값을 그대로 넘깁니다
// (OTA_START를 만들 때 이미 계산했을 것이므로 다시 계산하지 않음).
SimpleSendResult sendDataAndEnd(
    ITransport &transport,
    const std::string &filePath,
    uint32_t sessionId,
    uint32_t imageSize,
    uint32_t totalChunks,
    int chunkSize = -1,
    int chunkDelayMs = 10,
    const std::function<void(const SimpleSendProgress &)> &onProgress = nullptr);

// sessionId==0("자동 생성" 신호)일 때 실제로 쓸 랜덤 session_id를 만듦.
// simpleSendFile()과 performHandshake()가 공통으로 쓰는 작은 유틸이라 여기
// 하나로 모아둠(중복 방지).
uint32_t generateSessionId();

// 파일 크기만 필요할 때 쓰는 헬퍼. 실패 시 false 반환 + errorMessage 채움.
bool readFileSize(const std::string &filePath, uint32_t *sizeOut, std::string *errorMessage);

// ============================================================================
// 핸드셰이크 — docs/fsm-design.md의 HANDSHAKING 상태 구현
//
// 위 simpleSendFile()/sendDataAndEnd()와 파일을 같이 쓰는 이유: 별도
// session/handshake.h로 분리했다가 "파일이 계속 늘어나는 게 싫다"는
// 피드백을 받고 다시 합쳤습니다. simpleSendFile()(ACK 대기 없음)과
// performHandshake()(ACK 대기함)는 서로 하는 일이 반대라 헷갈릴 수 있어서,
// 아래처럼 이 파일 안에서도 구역을 분명히 나눠뒀습니다 — 파일 하나에
// 있다고 두 함수의 "계약"(단순 전송 vs 핸드셰이크)이 섞이는 건 아닙니다.
// ============================================================================

struct HandshakeResult
{
    bool success = false;
    std::string errorMessage;
    uint32_t sessionId = 0;     // 실제로 사용된 session_id (0을 넘겼다면 내부 생성값)
    uint32_t imageSize = 0;     // 이어서 sendDataAndEnd()에 그대로 넘기면 됨
    uint32_t totalChunks = 0;   // 위와 동일
};

// OTA_START를 보내고, 그 응답(OTA_ACK)이 올 때까지 기다립니다(블로킹).
// timeoutMs 안에 응답이 없으면 START를 다시 보내고, 이를 maxRetry회까지
// 반복합니다. 전부 실패하면 success=false로 반환합니다.
//
// simpleSendFile()과 차이: simpleSendFile()은 START를 보내자마자 응답을
// 기다리지 않고 바로 DATA를 쏘기 시작합니다("단순 전송" — 회선이 살아있는지만
// 확인). 이 함수는 실제 프로토콜 설계대로 "상대가 세션을 인지했다"는 걸
// 확인한 뒤에야 다음 단계(DATA 전송)로 넘어가게 해줍니다. 이후 DATA/END
// 전송은 위 sendDataAndEnd()를 이어서 호출하면 됩니다.
//
// transport: 이미 open()되어 있어야 하고, ACK를 들을 수 있도록 RX 상태여야
//            합니다(호출 전에 startRx() 필요 — 이 함수는 직접 안 함).
// filePath: OTA_START에 실을 image_size/total_chunks 계산용.
// targetDeviceId: OTA_START.target_device_id.
// sessionId: 0을 넘기면 내부에서 랜덤 생성.
// timeoutMs / maxRetry: 기본값 300ms / 5회 — docs/fsm-design.md의
//                        HANDSHAKING 타임아웃/재시도 확정값과 동일하게
//                        맞춤(실기기 실측 전까지는 추정값이라는 점도 동일).
HandshakeResult performHandshake(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId = 0,
    int timeoutMs = 300,
    int maxRetry = 5);

#endif // SIMPLESENDER_H
