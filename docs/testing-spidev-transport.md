# `SpidevTransport` 스모크테스트 실행법

| 항목 | 내용 |
|---|---|
| 대상 파일 | `transport/spidevtransport.h/.cpp`, `tests/smoke_spidev_send_main.cpp`, `tests/smoke_spidev_recv_main.cpp` |
| 목적 | `/dev/cc1101` 커널 드라이버 없이, `/dev/spidevX.Y`로 직접 CC1101을 제어해서 실기기로 전체 OTA 흐름(핸드셰이크→DATA→END) 검증 |
| 실행 환경 | 라즈베리파이 2대 (실기기 전용, 로컬/CI에서는 안 돌아감) |
| 배경/설계 이유 | `docs/file-transfer-guide.md` 2.2·6절, `docs/note/design-notes-gateway-ota-es.md` 18절 |

이 문서는 **"어떻게 실행하는지"**만 다룹니다. 코드 내부 동작 원리(핸드셰이크
로직, `ITransport` 구조 등)는 `docs/file-transfer-guide.md`를 참고하세요.

> **[2026-08-16 기준 현황] 지금 이 문서(`SpidevTransport`)가 핸드셰이크→
> DATA→END 전체 흐름이 실기기로 검증된 **유일한** 경로입니다.** 정식
> 경로인 `Cc1101Transport`(`/dev/cc1101`, 커널 드라이버)는 GDO2 인터럽트가
> 실기기에서 한 번도 안 울리는 버그 때문에 핸드셰이크 자체가 원천적으로
> 성공할 수 없는 상태입니다 — `Cc1101Transport::recv()`가 그 인터럽트로만
> 채워지는 큐를 읽는 구조라서요. 자세한 원인은
> `docs/note/design-notes-gateway-ota-es.md` 18절,
> `kernel-cc1101-spi/docs/pi-bringup-guide.md` 8절 참고.

## 1. 사전 준비

### 1.1 라즈베리파이 쪽 — `cc1101.ko`를 내리고 `/dev/spidevX.Y`로 전환

`cc1101.ko`(커널 드라이버)와 `SpidevTransport`(`/dev/spidevX.Y`)는 **같은
SPI 칩셀렉트(`spi0.0`)를 두고 서로 경합**합니다. `dtoverlay=cc1101`이
활성화돼 있으면 그 칩셀렉트는 `cc1101` 드라이버가 가져가서 `/dev/cc1101`로
노출되고, 라즈베리파이 기본 `spidev` 드라이버는 그 자리에 바인딩되지
않습니다(그래서 `/dev/spidev0.0`이 안 보임). 커널 드라이버 테스트를 하던
파이라면 아래 순서로 먼저 "spidev 모드"로 되돌려야 합니다.

**① 커널 모듈이 로드돼 있으면 내리기**
```sh
lsmod | grep cc1101        # 나오면 아래로, 안 나오면 ②로
sudo rmmod cc1101
```

**② 부팅 설정에서 `cc1101` 오버레이를 끄고 기본 `spidev`를 켜기**
```sh
grep -n 'dtoverlay=cc1101\|dtparam=spi' /boot/firmware/config.txt
sudo sed -i 's/^dtoverlay=cc1101/#dtoverlay=cc1101/' /boot/firmware/config.txt
# dtparam=spi=on 줄이 없거나 주석 처리돼 있으면 추가/해제:
grep -q '^dtparam=spi=on' /boot/firmware/config.txt || \
  echo 'dtparam=spi=on' | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

`dtoverlay=cc1101`만 주석 처리하고 `dtparam=spi=on`은 그대로 두면, 라즈베리파이
기본 이미지에 이미 들어있는 `spidev` 커널 모듈이 `spi0.0`/`spi0.1`에 자동으로
바인딩되어 `/dev/spidev0.0`/`/dev/spidev0.1`이 생깁니다 — 이건 리눅스에 이미
빌드돼 있는 표준 드라이버라 보통 따로 `insmod`할 필요가 없습니다.

**③ 재부팅 후 확인**
```sh
ls /dev/spidev*                # /dev/spidev0.0 등이 보여야 함
lsmod | grep spidev            # 안 보이면 수동 로드
sudo modprobe spidev           # 위에서 안 보였을 때만 실행
```

**④ 나중에 커널 드라이버(`cc1101.ko`) 테스트로 다시 돌아갈 때**
```sh
sudo sed -i 's/^#dtoverlay=cc1101/dtoverlay=cc1101/' /boot/firmware/config.txt
sudo reboot
sudo insmod cc1101.ko          # kernel-cc1101-spi/docs/pi-bringup-guide.md 4절과 동일
```

- CC1101 배선(SPI 4선 + GDO0/GDO2 + 안테나)은 평소와 동일합니다. **안테나가
  헐겁지 않은지 꼭 확인하세요** — 2026-08-16 실기기 테스트에서 소프트웨어는
  전부 정상인데 안테나 접촉 불량 하나로 반나절 동안 수신 자체가 안 되는
  일이 있었습니다 (design-notes 18절 참고).

### 1.2 빌드 방법 (둘 중 하나)

**A. CMake (Qt 설치돼 있을 때, 권장)**

```sh
cd OTA_System
cmake -B build          # 필요 시 -DOTA_PROTOCOL_INCLUDE_DIR=경로/include 추가
cmake --build build --target ota_smoke_spidev_send ota_smoke_spidev_recv
```

결과물: `build/ota_smoke_spidev_send`, `build/ota_smoke_spidev_recv`

> 이 두 타겟은 `ota_core_tests`처럼 자동 테스트(`ctest`)에는 등록돼 있지
> 않습니다. `/dev/spidevX.Y`가 실제로 있어야 동작하는 수동 실행 도구라서
> 그렇습니다 — `ota_smoke_send`/`ota_smoke_recv`(커널드라이버 버전)와 동일한
> 방침입니다.

**B. g++ 단독 (Qt 없이, 라즈베리파이에 Qt 안 깔았을 때)**

```sh
cd OTA_System
g++ -std=c++17 -I. -Icore -Itransport -Isession -I../../ota-protocol/include \
  core/binsplitter.cpp session/simplesender.cpp session/simplereceiver.cpp \
  transport/spidevtransport.cpp tests/smoke_spidev_send_main.cpp \
  -o ota_smoke_spidev_send

g++ -std=c++17 -I. -Icore -Itransport -Isession -I../../ota-protocol/include \
  core/binsplitter.cpp session/simplesender.cpp session/simplereceiver.cpp \
  transport/spidevtransport.cpp tests/smoke_spidev_recv_main.cpp \
  -o ota_smoke_spidev_recv
```

(`ota-protocol` 레포가 `gateway-ota`와 같은 상위 폴더에 clone돼 있어야
합니다 — README 마일스톤 3 안내 참고.)

## 2. 실행 순서

두 대의 라즈베리파이가 필요합니다. **수신 쪽을 먼저 켜두고**, 송신 쪽을
나중에 실행하세요 (수신 프로그램은 무한 루프로 대기하므로 순서가 바뀌어도
크게 상관없지만, 먼저 켜두면 초반 패킷을 놓칠 걱정이 없습니다).

### 2.1 수신 쪽 (받는 Pi)

```sh
sudo ./ota_smoke_spidev_recv /dev/spidev0.0
```

- `sudo` 필요: `/dev/spidevX.Y` 접근 권한 때문입니다 (권한 그룹 설정을
  안 했다면).
- `Ctrl+C`로 종료할 때까지 계속 대기하며 받는 패킷을 전부 콘솔에
  출력합니다 (`OTA_START`/`OTA_DATA`/`OTA_END` 종류·필드, 받을 때마다 ACK도
  자동으로 돌려보냄).

### 2.2 송신 쪽 (보내는 Pi)

```sh
sudo ./ota_smoke_spidev_send /dev/spidev0.0 firmware.bin
```

인자 전체 형식:

```
ota_smoke_spidev_send <spidev_path> <bin_file> [target_device_id_hex] [chunk_delay_ms] [ack_listen_ms]
```

| 인자 | 기본값 | 설명 |
|---|---|---|
| `spidev_path` | (필수) | 예: `/dev/spidev0.0` |
| `bin_file` | (필수) | 전송할 `.bin` 파일 경로 |
| `target_device_id_hex` | 브로드캐스트 | 특정 단말만 지정하려면 16진수 device_id |
| `chunk_delay_ms` | 10 | DATA 패킷 사이 대기시간(ms). CC1101이 이전 전송을 처리할 시간을 벌어주는 값 |
| `ack_listen_ms` | 2000 | 전송 완료 후 ACK/NACK을 얼마나 기다리며 볼지(ms) |

## 3. 정상 동작 시 로그로 확인할 것

**송신 쪽**: `핸드셰이크 성공` → `전송 중: N/전체` 카운트가 끝까지 올라감 →
`완료` → 이후 `ACK ...` 줄이 몇 개 찍힘.

**수신 쪽**: `OTA_START`(imageSize/totalChunks 표시) → `OTA_DATA`가
`seq=`와 함께 반복(중간에 `-> ACK 전송함`도 같이 찍힘) → `OTA_END`에
`(실제 받은 DATA 개수=N)`이 표시됨. **이 N이 총 청크 수보다 적게 나올 수
있습니다** — 3절 참고.

## 4. 기대 결과 — 정상이면 100% 수신됩니다

**2026-08-16 검증 결과: 51200byte / 1067청크 전부 수신 성공(1067/1067,
디코딩 실패 0건).** 가까운 거리에서 테스트하면 이 정도가 정상입니다.

> **[중요] 예전 문서의 "일부 유실은 정상" 설명은 틀렸습니다.**
> 한때 838/1067(78.5%)만 수신되던 시기가 있었고 이를 "무선 환경 손실이라
> 어쩔 수 없다"고 적어뒀었는데, 실제 원인은 `SpidevTransport::recv()`가
> 패킷이 다 도착하기 전에 FIFO를 읽어서 스스로 깨뜨리던 **소프트웨어
> 버그**였습니다. 지금은 수정됨 —
> `docs/note/design-notes-gateway-ota-es.md` 19절 참고.
> **그러니 수신율이 100%에 한참 못 미친다면 "원래 그런가보다" 하지 말고
> 원인을 찾으세요.**

다만 이 스모크테스트에 **DATA 구간 청크 단위 재전송이 없는 것은 사실**
입니다. 핸드셰이크(`OTA_START`)만 응답을 기다리고 재시도하며, 그 뒤 DATA는
응답 확인 없이 순서대로 쏘기만 합니다(`session/simplesender.cpp`의
`sendDataAndEnd()`). `docs/file-transfer-guide.md` 8절 참고:

| 상황 | 필요 처리 | 현재 구현 여부 |
|---|---|---|
| 일부 패킷 유실 | NACK/재전송으로 채움 | **미구현** |

따라서 거리가 멀거나 간섭이 심한 실제 환경에서는 여전히 진짜 유실이 생길
수 있고, 그때 채워줄 재전송 큐는 README 마일스톤 4 / `OtaSession`(FSM)
몫으로 아직 계획 단계입니다.

### 문제가 생겼을 때 — 로그로 원인 계층 가르기

| 수신측 화면 | 의미 | 볼 곳 |
|---|---|---|
| `RXBYTES=` 줄이 **한 줄도 안 나옴** | 무선 신호 자체가 안 들어옴 | **물리 계층** — 안테나 접촉(가장 흔함), 전원, SPI 배선 |
| `RXBYTES=`는 나오는데 값이 깨짐/디코딩 실패 | 신호는 오는데 읽기가 잘못됨 | **소프트웨어** — `recv()` 타이밍 등 |

이 구분이 디버깅 시간을 가장 크게 줄여줍니다. 그리고 **송수신 역할을 서로
바꿔서**(pi06↔pi24) 테스트하면 "어느 방향/어느 보드가 죽었는지"를 빠르게
좁힐 수 있습니다.

## 5. 종료 후 정리

`SpidevTransport`는 임시 우회 코드입니다. `Cc1101Transport`(커널 드라이버
경유)의 GDO2 인터럽트 감지 문제가 해결되면 `transport/spidevtransport.h/.cpp`,
이 문서, `tests/smoke_spidev_*_main.cpp`는 전부 삭제 대상입니다 — 자세한
경위는 `docs/note/design-notes-gateway-ota-es.md` 18절,
`kernel-cc1101-spi/docs/pi-bringup-guide.md` 참고.
