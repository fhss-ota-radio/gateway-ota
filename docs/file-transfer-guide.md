# 파일 송신 파이프라인 (`OTA_System`)

| 항목 | 내용 |
|---|---|
| 대상 레포 | `gateway-ota` (`ota-protocol` v0.2 참조) |
| 범위 | `.bin` 파일 분할(송신측) ~ 단순 전송 ~ 패킷 재조립(수신측 기준 로직) |
| 최종 수정 | 2026-08-14 (핸드셰이크 추가) |
| 관련 브랜치 | `feature/ota-core-split`(`develop` 병합 완료), `feature/simple-ota-send-test`(진행 중) |

## 1. 개요

`OTA_System`은 BIN 파일 하나를 무선(RF)으로 나눠 보내기 위해 세 계층으로
구성됩니다.

```
ui/         화면 (OtaManager)
core/       파일 분할 로직 (BinSplitter) — 순수 데이터 변환, I/O 없음
transport/  전송 계층 (ITransport, Cc1101Transport) — I/O 담당
session/    core/·transport/를 엮어 전송 흐름을 만드는 조율 계층 (simplesender)
tests/      core/·transport/·session/ 단위 테스트 및 CLI 진입점
```

`core/`·`transport/`·`session/`는 Qt(GUI 프레임워크)에 의존하지 않는 순수
C++로 작성되어 있습니다. `std::string`/`std::vector` 등 표준 라이브러리만
사용하므로 화면 없이 `g++`/`clang++` 단독으로 컴파일·테스트할 수 있습니다.

## 2. 컴포넌트

### 2.1 화면 (`ui/OtaManager`)
연결(Transport) · 전송 대상(유니캐스트/브로드캐스트) · BIN 파일 · 진행률 ·
로그, 5개 영역으로 구성. 마지막 설정값(포트, 드라이버, 청크 크기, 전송
모드)은 `QSettings`에 저장되어 재실행 시 복원됩니다.

### 2.2 전송 계층 (`transport/`)

| 구성요소 | 역할 |
|---|---|
| `ITransport` | `open`/`close`/`isOpen`/`send`/`recv`를 정의하는 추상 인터페이스 |
| `Cc1101Transport` | POSIX 시스템콜(open/write/poll+read/ioctl)로 `/dev/cc1101` 디바이스 파일과 통신하는 구현체 |
| `cc1101_ioctl.h` | `kernel-cc1101-spi` 커널 모듈과 공유하는 UAPI(User API) 계약 헤더 — ioctl 번호·구조체를 커널/유저 공간 양쪽이 동일하게 참조 |

**플랫폼 가드**: `#if defined(__linux__)`로 감싸 라즈베리파이(Linux)에서만
실구현이 빌드됩니다. macOS 등 커널 헤더가 없는 환경에서는 항상 실패를
반환하는 폴백 스텁이 대신 빌드되어, 로컬 개발 빌드가 깨지지 않습니다.

### 2.3 BIN 분할 (`core/BinSplitter`)
`.bin` 파일을 `ota-protocol` v0.2 규격의 `OTA_DATA` 패킷으로 분할합니다.
상세 구조는 4절 참고.

### 2.4 세션 조율 (`session/simplesender`)
`core/`(BinSplitter)와 `transport/`(Cc1101Transport)를 엮어서 "OTA_START →
DATA 전부 → OTA_END" 순서로 실행하는 조율(orchestration) 계층입니다.
`core/`·`transport/`가 각자 한 가지 일만 하는 것과 달리, 이 계층은 둘을
호출하는 흐름 자체를 담당합니다 — 그래서 `core/`가 아니라 별도 `session/`
폴더에 둡니다(순수 변환 로직 자리인 `core/`의 정의를 지키기 위함). 지금은
`simplesender`(ACK 대기·재전송 없는 단순 전송)와 `handshake`(START만 응답
확인하는 핸드셰이크, `fsm-design.md`의 `HANDSHAKING` 상태 구현)가 있고,
다음 단계로 예정된 `OtaSession`(FSM, 배치 ACK/재전송 담당)도 이 폴더에
들어갈 예정입니다. 상세는 6절 참고.

### 2.5 테스트 (`tests/`)
`tst_binsplitter.cpp` — Qt 없이 `g++`/CMake 어느 쪽으로든 실행 가능한 7개
테스트. 목록은 [`binsplitter-tests.md`](binsplitter-tests.md) 참고.
`smoke_send_main.cpp` — `simplesender`를 호출하는 CLI 진입점(실기기
필요, `ctest` 자동 실행 대상 아님). 상세는 6절 참고.

## 3. 데이터 흐름

```
[송신측 분할]                                   [수신측 재조립]
.bin 파일                                        수신 패킷들
   │                                                 │
   ▼                                                 ▼
BinSplitter::split()                    ota_protocol_decode_data()  (패킷마다 1회)
   │ 파일 읽기 + ota_protocol_encode_data 호출          │ 헤더 파싱 + CRC 검증
   ▼                                                 ▼
std::vector<OtaChunk>                    header + payload 포인터
   │                                                 │
   ▼                                                 ▼
OtaChunk.packet을                        payload를 sequence 순서로
ITransport::send()로 전송                     이어붙임 (reassembled 버퍼)
                                                      │
                                                      ▼
                                                 원본과 동일한 파일 바이트열
```

분할은 `gateway-ota`(`core/binsplitter.{h,cpp}`)가 담당합니다. 재조립은
전용 클래스 없이, `ota-protocol` 공유 헤더의 `ota_protocol_decode_data()`를
호출부가 반복 호출하며 payload를 이어붙이는 방식으로 처리합니다(현재는
테스트 코드가 호출부 역할, 추후 세션 로직으로 이전 예정).

## 4. 분할 — `core/binsplitter.h` / `core/binsplitter.cpp`

### 4.1 타입

```cpp
struct OtaChunk {
    ota_data_header_fields_t header{};  // session_id/sequence/payload_length/crc16
    std::vector<uint8_t> packet;        // 전송할 최종 바이트열 (헤더+payload)
};
```

`ota_data_header_fields_t`는 `ota_protocol.h`(공유 헤더)에 정의된 구조체를
그대로 재사용합니다 — `BinSplitter`가 별도로 정의한 타입이 아닙니다.

### 4.2 함수 시그니처

```cpp
class BinSplitter {
public:
    static std::vector<OtaChunk> split(
        const std::string &filePath,
        uint32_t sessionId,
        int chunkSize = static_cast<int>(OTA_MAX_PAYLOAD_SIZE),  // 기본 48byte
        std::string *errorMessage = nullptr);
};
```

| 파라미터 | 설명 |
|---|---|
| `filePath` | 분할할 `.bin` 파일 경로 |
| `sessionId` | 세션마다 랜덤 생성되는 식별자. `BinSplitter`가 직접 만들지 않고 세션 레이어가 생성해 전달 — 기본값 없이 항상 명시적으로 받음 |
| `chunkSize` | 청크 크기(byte). `OTA_MAX_PAYLOAD_SIZE`(48) 초과 시 실패 |
| `errorMessage` | 실패 사유를 담을 출력 파라미터 |

### 4.3 내부 동작

1. `chunkSize` 범위 검증 (1 ~ `OTA_MAX_PAYLOAD_SIZE`)
2. 파일을 열고 크기를 확인해 총 청크 개수를 올림 나눗셈으로 계산
3. `sequence = 0`부터 순서대로:
   - 파일에서 `chunkSize`만큼 읽음(마지막 청크는 남은 만큼만)
   - `ota_protocol_encode_data(buffer, ..., sessionId, sequence, payload, length)` 호출
   - `OtaChunk` 생성 — `header`는 이미 아는 값(session_id/sequence/
     payload_length)과 `ota_protocol_crc16()` 계산값으로 직접 채움
   - 결과 벡터에 추가
4. `std::vector<OtaChunk>` 반환

마지막 청크는 패딩 없이 실제 길이만 담습니다 — `payload_length` 필드가
정확한 길이를 전달하므로 패딩이 불필요합니다.

### 4.4 사용 예시

```cpp
std::string error;
auto chunks = BinSplitter::split("firmware.bin", sessionId, 48, &error);
for (const auto &chunk : chunks)
    transport.send(chunk.packet);
```

## 5. 재조립 — `ota_protocol_decode_data()` (`ota-protocol` 레포)

전용 재조립 클래스는 존재하지 않습니다. 필요한 로직은 패킷마다 디코딩해
payload를 순서대로 이어붙이는 것뿐이며, 이는 공유 헤더의
`ota_protocol_decode_data()`로 충분합니다. 참조 사용 패턴은
`tst_binsplitter.cpp`의 `roundTripSplitAndReassembleMatchesOriginalFile`
테스트에 구현되어 있습니다.

```cpp
std::vector<uint8_t> reassembled;
for (const auto &chunk : chunks) {          // sequence 순서 보장 전제
    ota_data_header_fields_t header;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;

    bool ok = ota_protocol_decode_data(
        chunk.packet.data(), chunk.packet.size(),
        &header, &payload, &payloadLen);    // CRC 불일치 시 false

    reassembled.insert(reassembled.end(), payload, payload + payloadLen);
}
// reassembled == 원본 파일 바이트열
```

### 5.1 `ota_protocol_decode_data()` 동작 단계

1. 패킷 길이·type byte 검증
2. 헤더 파싱 → `session_id`/`sequence`/`payload_length`/`crc16` 추출
3. `payload_length`만큼 실제 payload가 있는지 길이 검증
4. `ota_protocol_crc16(payload, length)` 재계산 후 헤더의 `crc16`과 비교 —
   불일치 시 `false` 반환(손상 패킷, 상위 로직이 NACK 처리해야 함)
5. 전 단계 통과 시에만 `header_out`/`payload_out`/`payload_length_out`에 값 반영

실패 시(`false` 반환) out 파라미터는 전혀 변경되지 않습니다 — 반환값을
확인하지 않고 바로 읽어도 오염된 값이 섞이지 않도록 설계되어 있습니다.

## 6. 송신 로직 — 핸드셰이크 + 단순 전송 (`session/`)

두 가지 경로가 있습니다.

- **`simpleSendFile()`** — `OTA_START → DATA 전부 → OTA_END`를 ACK 대기 없이
  한 번에 쏘는 가장 단순한 루틴. 회선·인코딩이 죽지 않고 도는지만 확인하는
  용도(6.1~6.2절).
- **`performHandshake()` + `sendDataAndEnd()`** — START를 보내고 **응답을
  기다린 뒤에야** DATA를 보내는, `fsm-design.md`의 `HANDSHAKING` 상태를
  실제로 구현한 경로(6.3절). `tests/smoke_send_main.cpp`는 이제 이 경로를
  씁니다.

내부적으로 `simpleSendFile()`도 `sendDataAndEnd()`를 그대로 호출합니다 —
"START 보내고 곧장 DATA로" vs "START 보내고 응답 기다린 뒤 DATA로"의 차이만
있을 뿐, DATA/END를 만들고 보내는 로직 자체는 하나로 공유됩니다.

### 6.1 `simpleSendFile()` — 단순 전송

```cpp
SimpleSendResult simpleSendFile(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId = 0,      // 0이면 내부에서 랜덤 생성
    int chunkSize = -1,          // -1이면 OTA_MAX_PAYLOAD_SIZE(48byte)
    int chunkDelayMs = 10,       // DATA 패킷 사이 대기 시간(ms)
    const std::function<void(const SimpleSendProgress &)> &onProgress = nullptr);
```

| 파라미터 | 설명 |
|---|---|
| `transport` | 이미 `open()`된 상태여야 함(연결 수명 관리는 호출자 책임) |
| `targetDeviceId` | 특정 기기면 `device_id`, 전체 대상이면 `OTA_BROADCAST_DEVICE_ID` |
| `sessionId` | 재현 가능한 테스트가 필요하면 직접 지정, 아니면 0으로 자동 생성 |
| `chunkDelayMs` | CC1101 드라이버가 이전 `write()`를 처리할 시간을 벌어주기 위한 값. 연속으로 너무 빨리 쏘면 유실 가능성이 있어 기본 10ms 대기(실측 기반 값은 아니고 추정치 — 실기기 테스트하며 조정 예정) |

내부 동작: ①`transport.isOpen()` 확인 ②파일 크기 확인 →
`ota_protocol_total_chunks()`로 총 청크 개수 계산 ③`OTA_START` 인코딩+전송
(`image_sha256`은 아직 0 — 6.4절 참고) ④곧장 `sendDataAndEnd()` 호출(응답
안 기다림).

### 6.2 `sendDataAndEnd()` — DATA 전부 + END (핸드셰이크 이후 공용)

```cpp
SimpleSendResult sendDataAndEnd(
    ITransport &transport,
    const std::string &filePath,
    uint32_t sessionId,
    uint32_t imageSize,
    uint32_t totalChunks,
    int chunkSize = -1,
    int chunkDelayMs = 10,
    const std::function<void(const SimpleSendProgress &)> &onProgress = nullptr);
```

`simpleSendFile()`의 "OTA_START 이후" 부분만 떼어낸 함수입니다. `sessionId`/
`imageSize`/`totalChunks`를 호출부(START를 이미 보낸 쪽)가 넘겨줍니다.

1. `BinSplitter::split()`으로 청크 생성 → 넘겨받은 `totalChunks`와 개수가
   일치하는지 확인(불일치 시 실패 처리)
2. 청크를 순서대로 `transport.send()` — 매 청크마다 `onProgress` 콜백 호출,
   `chunkDelayMs`만큼 대기
3. `OTA_END` 인코딩 + 전송

### 6.3 `performHandshake()` — `session/simplesender.h` / `.cpp`

(`simpleSendFile`/`sendDataAndEnd`와 같은 파일입니다 — 처음엔
`session/handshake.h`로 따로 뺐다가, 파일 개수가 계속 늘어나는 게
싫다는 피드백을 받고 다시 합쳤습니다. 헷갈리지 않도록 파일 안에서 구역만
분명히 나눠뒀습니다.)

`fsm-design.md`의 `HANDSHAKING` 상태 구현입니다. OTA_START를 보내고
**응답(ACK)이 올 때까지 기다립니다**(블로킹). 타임아웃 시 재전송 —
`fsm-design.md`가 정해둔 값과 동일하게 기본 300ms 타임아웃 × 최대 5회
재시도.

```cpp
HandshakeResult performHandshake(
    ITransport &transport,
    const std::string &filePath,
    uint32_t targetDeviceId,
    uint32_t sessionId = 0,
    int timeoutMs = 300,
    int maxRetry = 5);
```

내부 동작: `OTA_START` 인코딩 → `maxRetry`회까지 반복(START 전송 →
`timeoutMs` 동안 `tryReceiveOnce()`로 폴링) → `session_id`와 `sequence`
(START 응답은 `OTA_CONTROL_SEQUENCE`)가 둘 다 일치하는 `ACK`를 받으면 성공.
일치하는 `NACK`를 받으면 사유 판단 없이 타임아웃과 동일하게 취급하고 다음
시도로 넘어감(사유별 처리는 `OtaSession` 몫). 성공 시 반환되는
`imageSize`/`totalChunks`를 그대로 `sendDataAndEnd()`에 넘기면 됩니다.

> **[검증 완료, 2026-08-16] `Cc1101Transport`(커널 드라이버 경로)와
> `SpidevTransport`(우회 경로) 양쪽 모두 실기기로 검증됐습니다** —
> 51200byte / 1067청크 전량 수신(1067/1067, 디코딩 실패 0건).
>
> 단, **송신 시 청크 간 대기 40ms가 필요합니다**(`chunkDelayMs` 인자).
> 기본값 10ms에서는 수신이 송신 속도를 못 따라가 패킷 경계가 밀립니다
> (깨진 패킷 꼬리에 다음 패킷의 프리앰블+싱크워드가 딸려 들어오고,
> 디코딩 성공률이 4개 중 1개꼴로 떨어짐). 수신 처리량 개선은 남은 과제입니다.
>
> 자세한 경위는 `docs/note/design-notes-gateway-ota-es.md` 18~21절,
> `kernel-cc1101-spi/docs/driver-changes-handoff-2026-08-16.md` 참고.

### 6.4 CLI 진입점 — `tests/smoke_send_main.cpp`

```
ota_smoke_send <device_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]
```

`performHandshake()` → 성공 시 `sendDataAndEnd()` → 다 보낸 뒤 남은 ACK를
`ackListenMs` 동안 더 확인, 순서로 호출하는 얇은 CLI입니다. argv 파싱,
`Cc1101Transport` 생성, 진행 상황/결과를 콘솔에 출력하는 역할만 하고 로직
자체는 갖고 있지 않습니다. `Cc1101Transport::startRx()` 호출 시점이
`OTA_START`를 보내기 **전**으로 되어 있음에 주의 — 핸드셰이크 응답을
들으려면 START 전송 전부터 RX 상태여야 하기 때문입니다. [확인 필요] RX
상태에서 `send()`가 문제없이 동작하는지는 드라이버 구현에 달려 있어
실기기에서 확인해야 합니다.

실기기 없이 자동 실행되는 `ota_core_tests`(ctest 등록)와 달리
`/dev/cc1101`이 있어야 동작하는 수동 실행 도구라 `add_test()`에는
등록하지 않습니다.

### 6.5 수신측 스텁 — `session/simplereceiver.h` / `tests/smoke_recv_main.cpp`

송신측과 대칭되는 구조로, `tryReceiveOnce()`(핵심 로직, `session/`)가
`transport.recv()`로 패킷 하나를 논블로킹 확인하고 type byte로 맞는
`ota_protocol_decode_*()`를 호출해 필드를 채웁니다. `tests/smoke_recv_main.cpp`
(CLI 진입점)는 이걸 반복 호출하며 콘솔에 종류/필드를 출력만 합니다.

"스텁"(아직 완성되지 않은 임시 뼈대)인 이유: 재전송 판단이나 대기(타임아웃)
없이, 받은 패킷 하나마다 반사적으로 `OTA_ACK`만 즉시 돌려보냅니다
(`session/simplereceiver.h`의 `sendAckFor()`, kind가 Start/Data/End일 때만
동작). 실제 프로토콜 대화(청크마다 응답 기다리기, 실패 시 재전송 요청)는
`OtaSession`(FSM) 몫으로 남겨두고, 지금은 "송신측이 보낸 게 여기서
보이는지 + 받았다는 신호가 다시 송신측까지 가는지"만 확인하는 관찰용입니다.

```
ota_smoke_recv <device_path> [저장할_파일]
```

**[추가 2026-08-16] 파일 재조립 — 무결성 확인용**

두 번째 인자로 파일 경로를 주면 받은 DATA를 재조립해서 그 파일에 씁니다.
그동안은 로그만 찍어서 *"1067개 전부 받았다"*는 건 알아도 **내용이 원본과
같은지는 확인할 수 없었습니다.** CRC는 패킷 단위 검사일 뿐이라, 순서 뒤바뀜·
중복·특정 청크 유실은 CRC를 다 통과하고도 파일을 깨뜨릴 수 있습니다.

동작 방식: `sequence`를 그대로 파일 오프셋으로 씁니다
(`seq × OTA_MAX_PAYLOAD_SIZE`). 그래서 패킷이 뒤바뀌어 도착해도 제자리에
기록되고, 유실된 구간은 0으로 남아 어디가 빠졌는지 드러납니다.
`OTA_START`에서 `imageSize`만큼 파일을 미리 늘려두는 것도 같은 이유입니다.

`OTA_END`를 받으면 채워진/중복/누락 청크 수를 요약하고, 누락이 있으면 빠진
`seq`를 앞쪽 20개까지 보여줍니다. 최종 확인은 해시 비교로 합니다:

```sh
# 수신측
sudo ./ota_smoke_recv /dev/cc1101 recv.bin
# 전송 끝난 뒤 양쪽에서
sha256sum 원본.bin      # 송신측
sha256sum recv.bin      # 수신측
```

> `ota_smoke_spidev_recv`(우회 경로 버전)에는 아직 이 기능이 없습니다 —
> 필요하면 같은 방식으로 옮기면 됩니다.

송신측(`ota_smoke_send`)도 모든 청크를 다 보낸 뒤, `ackListenMs`(기본
2000ms, CLI 5번째 인자로 조정 가능) 동안 `tryReceiveOnce()`로 들어오는
ACK를 화면에 보여줍니다 — 여기서도 재전송 판단은 하지 않고, 그냥 양쪽
화면에서 확인 가능하게만 합니다.

**검증(실기기 없이)**: `ITransport`가 추상 인터페이스라는 점을 이용해
실제 CC1101 대신 큐로 send/recv를 흉내 내는 가짜 transport로 확인:
① `simpleSendFile()`이 넣은 패킷을 `tryReceiveOnce()`가 그대로 다시 읽어
필드까지 정확히 일치하는지(START 1개+DATA 3개+END 1개), ② 양방향 가짜
링크로 receiver가 보낸 ACK 5개를 sender 쪽이 다시 읽어 session_id/
result_code까지 일치하는지, ③ `performHandshake()`가 실제로 응답을
"기다리는" 동안(블로킹) receiver를 별도 스레드로 동시에 돌려 실시간
응답 상황을 재현 — START 핸드셰이크 성공 → 이어서 DATA×2+END까지 정상
처리(총 4개 패킷 송수신·ACK 확인). 전부 통과. 실기기에서 남은 건 이제
순수하게 무선 구간(칩·안테나·드라이버)뿐입니다.

## 7. 재조립 — `ota_protocol_decode_data()` (`ota-protocol` 레포)

전용 재조립 클래스는 존재하지 않습니다. 필요한 로직은 패킷마다 디코딩해
payload를 순서대로 이어붙이는 것뿐이며, 이는 공유 헤더의
`ota_protocol_decode_data()`로 충분합니다. 참조 사용 패턴은
`tst_binsplitter.cpp`의 `roundTripSplitAndReassembleMatchesOriginalFile`
테스트에 구현되어 있습니다.

```cpp
std::vector<uint8_t> reassembled;
for (const auto &chunk : chunks) {          // sequence 순서 보장 전제
    ota_data_header_fields_t header;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;

    bool ok = ota_protocol_decode_data(
        chunk.packet.data(), chunk.packet.size(),
        &header, &payload, &payloadLen);    // CRC 불일치 시 false

    reassembled.insert(reassembled.end(), payload, payload + payloadLen);
}
// reassembled == 원본 파일 바이트열
```

### 7.1 `ota_protocol_decode_data()` 동작 단계

1. 패킷 길이·type byte 검증
2. 헤더 파싱 → `session_id`/`sequence`/`payload_length`/`crc16` 추출
3. `payload_length`만큼 실제 payload가 있는지 길이 검증
4. `ota_protocol_crc16(payload, length)` 재계산 후 헤더의 `crc16`과 비교 —
   불일치 시 `false` 반환(손상 패킷, 상위 로직이 NACK 처리해야 함)
5. 전 단계 통과 시에만 `header_out`/`payload_out`/`payload_length_out`에 값 반영

실패 시(`false` 반환) out 파라미터는 전혀 변경되지 않습니다 — 반환값을
확인하지 않고 바로 읽어도 오염된 값이 섞이지 않도록 설계되어 있습니다.

## 8. 알려진 제약 — 실제 무선 수신 시나리오

4·7절의 분할/재조립 검증은 **순서대로, 누락 없이, 중복 없이** 도착하는
가장 단순한 조건만 다룹니다. `sendDataAndEnd()`(DATA 구간)도 청크마다
응답을 기다리지 않고 이 조건을 그대로 전제합니다 — `performHandshake()`가
확인하는 건 START 하나뿐이고, DATA 구간의 신뢰성은 여전히 없습니다. 실제
무선(RF) 수신에서는 다음이 추가로 필요합니다.

| 상황 | 필요 처리 | 현재 구현 여부 |
|---|---|---|
| 패킷 순서 뒤바뀜 | `sequence` 기준 정렬 후 저장 | 미구현 |
| 일부 패킷 유실 | NACK/재전송으로 채움 | 미구현 |
| 재전송으로 인한 중복 도착 | 중복 sequence 제거 | 미구현 |

이 로직은 `OtaSession`(FSM, [`fsm-design.md`](fsm-design.md) 참고) 또는
별도 수신측 프로그램에서 구현될 예정이며, 현재 브랜치 범위 밖입니다.

## 9. 남은 작업

- 실기기(라즈베리파이)에서 `ota_smoke_send`/`ota_smoke_recv` 실제 송수신 검증
  (CC1101 셋팅 완료 후) — 특히 RX 상태에서 `send()`가 정상 동작하는지
  (6.4절 [확인 필요] 항목)
- `simplesender`/`BinSplitter` 결과를 화면(`otamanager.cpp`)에서 실제로
  호출해 전송하는 연동
- `OtaSession`(FSM) — 배치 전송, 청크 단위 ACK/NACK 처리, 종료를 담당하는
  컨트롤러 (`session/`에 추가 예정). 핸드셰이크(START)는 완료됐고, 나머지
  단계(`SENDING_BATCH`/`WAITING_BATCH_ACK`/`RETRANSMITTING`/`WAITING_END_ACK`)가
  남음
- 8절의 실제 무선 수신 시나리오 대응 재조립 로직 (`simplereceiver`는 관찰만,
  정렬/재전송/중복제거는 아직 없음)

## 10. 관련 파일

| 역할 | 위치 |
|---|---|
| 분할 클래스 | `OTA_System/core/binsplitter.h`, `.cpp` |
| 단순 전송 + DATA/END 공용 로직 | `OTA_System/session/simplesender.h`, `.cpp` |
| 핸드셰이크(START, 응답 대기) | `OTA_System/session/simplesender.h`, `.cpp` (`performHandshake()`) |
| 송신 스모크테스트 CLI | `OTA_System/tests/smoke_send_main.cpp` |
| 단순 수신(스모크테스트용 스텁, ACK 반사 응답 포함) | `OTA_System/session/simplereceiver.h`, `.cpp` |
| 수신 스모크테스트 CLI | `OTA_System/tests/smoke_recv_main.cpp` |
| 분할+재조립 사용 예시(테스트) | `OTA_System/tests/tst_binsplitter.cpp` |
| 인코딩/디코딩 함수 (`encode_data`/`decode_data`/`encode_start`/`encode_end`/`encode_ack`) | `ota-protocol` 레포 `include/ota_protocol.h` |
| 테스트 항목별 설명 | [`binsplitter-tests.md`](binsplitter-tests.md) |
| 세션 FSM 설계 (`HANDSHAKING` 등 전체 상태) | [`fsm-design.md`](fsm-design.md) |
| 패킷 분할/재전송 전략(프로토콜 레벨) | `ota-protocol` 레포 `docs/note/notion-data-transfer.md` |
