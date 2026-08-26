# `BinSplitter` 테스트가 검증하는 것

> 대상: 팀 공유용 / 미래의 나. `tests/tst_binsplitter.cpp`에 있는 테스트 7개가
> 각각 뭘 확인하는지 정리했습니다. 코드만 보면 "뭘 하는 테스트인지" 바로
> 파악하기 어려워서, 이 문서에서 이름-목적-왜 필요한지를 묶어서 봅니다.

## `BinSplitter`가 하는 일 (한 줄 요약)

`.bin` 파일 하나를 받아서, `ota-protocol`(공유 헤더) 규격에 맞는 `OTA_DATA`
패킷 여러 개로 쪼갭니다(`BinSplitter::split()`). 실제 무선 전송이나 파일
재조립은 이 클래스 책임이 아니고, 순수하게 "파일 → 전송 가능한 패킷 목록"
변환만 담당합니다.

## 실행 방법

CMake/Qt 없이 `g++`만으로 바로 실행됩니다 (`BinSplitter`가 Qt에 전혀
의존하지 않기 때문):

```bash
cd OTA_System
g++ -std=c++17 -Wall -Wextra \
    -I. -Icore -I../../ota-protocol/include \
    tests/tst_binsplitter.cpp core/binsplitter.cpp \
    -o /tmp/tst_binsplitter
/tmp/tst_binsplitter
```

CMake로 하려면 `cmake --build build --target ota_core_tests && ctest --test-dir build -R ota_core_tests`.

## 테스트 7개 — 뭘 확인하는가

| 테스트 이름 | 확인하는 것 | 왜 필요한가 |
|---|---|---|
| `splitsExactMultipleFile` | 파일 크기가 청크 크기의 정확한 배수일 때, 청크 개수와 `sequence`(0, 1, 2...) 순서가 맞는지 | 가장 기본적인 "정상 케이스"가 안 되면 나머지는 의미 없음 |
| `lastChunkIsShorterNotPadded` | 파일 크기가 청크 크기의 배수가 아닐 때, 마지막 청크가 남는 만큼만 담기고 0x00으로 채워지지 않는지 | `payload_length` 필드가 실제 길이를 정확히 전달하는지 확인 — 패딩을 넣으면 무선으로 나가는 바이트만 늘고, 안 넣어도 되는 이유가 이 필드 덕분이라는 걸 검증 |
| `corruptedPacketFailsCrcOnDecode` | 인코딩된 패킷의 payload 1byte를 일부러 깨뜨리면, `ota_protocol_decode_data()`가 CRC 불일치로 디코딩을 거부하는지 | 무선 전송 중 노이즈로 데이터가 깨졌을 때 "깨진 걸 깨졌다고 알아채는지"가 핵심 — 이게 안 되면 손상된 펌웨어를 정상인 줄 알고 기록하는 사고로 이어짐 |
| `reportsErrorForMissingFile` | 존재하지 않는 파일 경로를 주면 빈 목록 + 에러 메시지를 돌려주는지 | 파일 선택 UI에서 잘못된 경로가 들어와도 앱이 죽지 않고 사용자에게 에러를 보여줄 수 있는지 |
| `reportsErrorForChunkSizeOutOfRange` | 청크 크기가 0 이하이거나 `OTA_MAX_PAYLOAD_SIZE`(현재 48byte)를 넘으면 거부하는지 | RF 계층이 감당 못 하는 크기로 패킷을 만들어버리는 실수를 미리 막음(CC1101 FIFO/60byte 제약과 연결) |
| `defaultChunkSizeMatchesProtocolMax` | `chunkSize` 인자를 생략하면 기본값이 `OTA_MAX_PAYLOAD_SIZE`로 자동 설정되는지 | 호출부가 매번 이 상수를 몰라도 안전한 기본값으로 동작하는지 확인 |
| `roundTripSplitAndReassembleMatchesOriginalFile` | 파일을 `split()`으로 쪼갠 뒤, 각 청크를 다시 `ota_protocol_decode_data()`로 풀어서 payload를 순서대로 이어붙이면 **원본 파일과 바이트 단위로 완전히 같은지** | 위 6개는 인코딩(split) 쪽만 봤는데, 이건 "실제로 파일이 손실 없이 복원되는가"를 처음부터 끝까지 왕복시켜서 확인하는 테스트 (2026-08-11 추가) |

## 이 테스트들이 확인 안 해주는 것

- **실제 무선 전송(RF)**: `ITransport`/`Cc1101Transport`로 실제 CC1101을
  통해 패킷을 주고받는 건 이 테스트 범위 밖입니다. 여기는 메모리 안에서만
  인코딩→디코딩을 왕복시킵니다.
- **여러 기기 간 세션 흐름**: `OTA_START`/`OTA_END`, ACK/NACK 기반 재전송,
  `OTA_DISCOVER`/`OTA_DISCOVER_ACK` 조회 등 세션 레이어 전체 흐름은
  `OtaSession`(아직 미구현, `docs/fsm-design.md` 참고) 쪽 책임이라 여기
  포함되지 않습니다.
- **실기기(라즈베리파이 2대) 간 실제 스모크테스트**: 이건 별도로 예정된
  작업입니다 — 송신측/수신측 프로그램을 각각 만들어서 실제 CC1101 하드웨어로
  주고받아보는 테스트로, `BinSplitter` 테스트와는 다른 층위의 검증입니다.

## 참고

- 테스트 코드: [`tests/tst_binsplitter.cpp`](../OTA_System/tests/tst_binsplitter.cpp)
- 대상 클래스: [`core/binsplitter.h`](../OTA_System/core/binsplitter.h) /
  [`core/binsplitter.cpp`](../OTA_System/core/binsplitter.cpp)
- 패킷 포맷 전체 레퍼런스: `ota-protocol` 레포의 `docs/ota-protocol-reference.md`
