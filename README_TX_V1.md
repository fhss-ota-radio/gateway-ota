# gateway-ota TX v1 패치

이번 패치는 현재 올라간 `/dev/cc1101` 커널 드라이버를 Qt 앱에서 실제로 사용하여
`BIN -> OTA chunk -> write(/dev/cc1101)`까지 전송하도록 만든 1차 버전입니다.

## 포함된 실제 동작

1. `Cc1101Transport::open()`
   - `/dev/cc1101`을 `O_RDWR | O_NONBLOCK`으로 실제 open
2. `send()`
   - `write()`로 커널 드라이버에 OTA packet 전달
3. `recv()`
   - `poll()` + `read()` 구현
4. `ioctl()`
   - 채널 변경
   - RX 진입
   - RX/TX FIFO flush
5. Qt 연결 버튼
   - 화면 토글이 아니라 실제 `/dev/cc1101` open
6. OTA 전송 시작
   - BinSplitter로 `.bin` 분할
   - 각 packet을 순서대로 CC1101 transport에 전달
   - progress bar 갱신
   - pause/resume
7. 드라이버 packet 제한 반영
   - kernel driver 최대 packet = 61 byte
   - OTA header = 9 byte
   - UI firmware chunk 최대 = 52 byte

## 아직 일부러 넣지 않은 부분

- ACK / NACK 판단
- ACK timeout
- NACK/timeout 재전송 큐
- ESP32 flash write 완료 검증
- 유니캐스트 node address 실제 ioctl 매핑

현재 첨부된 소스에는 ACK/NACK packet의 정확한 type/encode/decode 정의가 없어서
그 부분을 임의로 만들지 않았습니다. 공유 `ota_protocol.h` 또는 ESP32 receiver 코드가
확정되면 다음 단계로 ACK + timeout + retry 상태 머신을 붙이면 됩니다.

## 파일 배치

프로젝트의 기존 폴더 구조에 맞게 아래 파일을 덮어쓰거나 내용만 반영하세요.

- `transport/cc1101transport.h`
- `transport/cc1101transport.cpp`
- `ui/otamanager.h`
- `ui/otamanager.cpp`

그리고 `cc1101_ioctl.h`가 Qt target의 include path에서 보이도록 두어야 합니다.

## 첫 테스트

Raspberry Pi에서:

```sh
ls -l /dev/cc1101
```

가 존재하는 상태에서 앱을 실행하고:

1. 드라이버: `CC1101 (/dev/cc1101)`
2. 경로: `/dev/cc1101`
3. `연결`
4. 작은 `.bin` 파일 선택
5. chunk size 52 이하
6. `전송 시작`

상대 CC1101이 없어도 `write()` 자체의 TX 완료 IRQ까지는 시험할 수 있습니다.
다만 실제 OTA 성공 여부는 수신 노드와 ACK 구현 후 확인해야 합니다.
