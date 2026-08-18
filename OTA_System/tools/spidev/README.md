# tools/spidev — CC1101 통제 실험 도구

> **⚠️ 제품 코드가 아닙니다.** 정식 전송 경로는
> `transport/cc1101transport.h`(`/dev/cc1101`, 커널 드라이버 기반)입니다.
> 이 폴더는 진단·검증 전용이며 GUI 앱(`OTA_System`)은 이 코드를 링크하지 않습니다.

## 이게 뭔가

커널 모듈(`cc1101.ko`)을 **완전히 건너뛰고**, 리눅스 표준 spidev
(`/dev/spidevX.Y`) 유저공간 인터페이스로 CC1101 레지스터를 직접 읽고 쓰는
`ITransport` 구현체입니다. 인터럽트도 kfifo도 쓰지 않고 **폴링**만 합니다.

| | 정식 경로 (`Cc1101Transport`) | 이 도구 (`SpidevTransport`) |
|---|---|---|
| 디바이스 | `/dev/cc1101` | `/dev/spidev0.0` |
| 커널 모듈 | `cc1101.ko` 필요 | **불필요** |
| 수신 알림 | GDO0/GDO2 인터럽트 | 폴링 (`RXBYTES` 반복 읽기) |
| 버퍼 | 커널 kfifo (512byte) | 없음 — 칩 FIFO(64byte) 직접 읽음 |
| 오버레이 | `dtoverlay=cc1101` | `dtparam=spi=on` |

## 왜 남겨뒀나 — 통제 실험(control experiment)

원래는 커널 드라이버가 안 올라가던 동안의 임시 우회로였습니다. 커널 경로가
2026-08-16에 검증을 통과했으니 지워도 되지만, **의도적으로 남겼습니다.**

이 도구는 **커널 계층 전체를 우회**하기 때문에, 문제가 생겼을 때 이것 하나로
원인 계층을 절반으로 잘라낼 수 있습니다:

| spidev로 하면 | 커널 경로로 하면 | 결론 |
|---|---|---|
| ✅ 된다 | ❌ 안 된다 | **커널 드라이버 쪽 문제** — 하드웨어·배선·안테나·칩 설정은 정상 |
| ❌ 안 된다 | ❌ 안 된다 | **하드웨어/RF 쪽 문제** — 안테나, 전원, 배선, 싱크워드 충돌 |

과학 실험의 대조군(control group)과 같은 역할입니다. "무엇을 바꿨을 때
결과가 달라지는가"를 봐야 원인을 특정할 수 있는데, 이 도구가 그 비교 기준이
되어줍니다.

**2026-08-16 실제 사례**: 커널 경로에서 GDO2 인터럽트가 안 울려서 반나절을
드라이버 코드 문제로 의심했는데, spidev로 돌려봤더니 **똑같이 안 됐습니다.**
그 순간 "커널 문제가 아니다"가 확정돼서 하드웨어로 시선을 돌렸고, 결국
①수신측 안테나 불량 ②팀원들과의 싱크워드 충돌이 원인이었습니다.
이 비교가 없었으면 계속 커널 코드만 파고 있었을 겁니다.

## 언제 쓰나

**커널 경로(`/dev/cc1101`)가 안 될 때, 원인 계층을 가르는 첫 번째 수단으로.**

먼저 `kernel-cc1101-spi/tools/cc1101_diag`로 칩 설정을 확인하고, 그래도
원인이 안 잡히면 이걸로 대조 실험을 하세요. 상세 절차는
[`docs/troubleshooting-cc1101.md`](../../../../kernel-cc1101-spi/docs/troubleshooting-cc1101.md)
0장 참고.

## 쓰는 법

**커널 모듈과 동시에 못 씁니다.** `dtoverlay=cc1101`이 SPI 칩셀렉트를
점유하므로 `/dev/spidev0.0`이 아예 안 생깁니다. 모드를 바꿔야 합니다:

```sh
# spidev 모드로 전환 (오버레이 비활성화 후 재부팅)
sudo sed -i 's/^dtoverlay=cc1101/#dtoverlay=cc1101/' /boot/firmware/config.txt
sudo reboot
ls -l /dev/spidev0.0        # 생겼는지 확인

# 빌드
cmake --build build --target ota_smoke_spidev_send ota_smoke_spidev_recv

# 수신측 Pi
sudo ./ota_smoke_spidev_recv /dev/spidev0.0

# 송신측 Pi
sudo ./ota_smoke_spidev_send /dev/spidev0.0 test.bin ffffff 40
```

전체 절차·인자 설명은
[`docs/testing-spidev-transport.md`](../../../docs/testing-spidev-transport.md) 참고.

## 정식 경로와 반드시 맞춰야 하는 것

`spidevtransport.cpp`의 레지스터 배열은 `kernel-cc1101-spi/cc1101_core.c`의
`cc1101_default_regs[]`와 **같은 값이어야 합니다.** 한쪽만 바꾸면 두 경로가
서로 통신하지 못하고, 그 사실이 겉으로는 "RF가 안 된다"처럼 보여서 디버깅을
크게 헤매게 됩니다.

특히 주의할 것:

| 레지스터 | 현재 값 | 이유 |
|---|---|---|
| `SYNC1`/`SYNC0` | `0x2D` / `0xD4` | OTA 전용값. 팀 공용 기본값 `0xD3/0x91`은 다른 팀과 충돌 |
| `PKTCTRL1` | `0x0C` | 하드웨어 주소필터(`ADR_CHK`) 끔 — 우리 패킷 첫 바이트는 주소가 아니라 패킷타입 |
| `CHANNR` | `0x00` | 채널 4가 국내 ISM 밴드 상한. 큰 값은 PLL 락 실패 |

## 언제 지워도 되나

다음 중 하나가 성립하면 이 폴더를 통째로 삭제하고, `OTA_System/CMakeLists.txt`의
`tools/spidev` 블록만 지우면 됩니다 (그 외에는 아무것도 안 건드립니다 —
`ota_core`에 안 들어가 있는 이유가 이것입니다):

- 커널 드라이버가 충분히 안정화돼서 대조 실험이 더 필요 없어졌을 때
- 더 나은 진단 수단이 생겼을 때 (예: 로직 애널라이저, 커널 tracepoint)
- 두 경로의 레지스터 설정을 계속 동기화하는 유지비가 얻는 값보다 커졌을 때

**마지막 항목이 현실적으로 가장 유력한 삭제 사유입니다.** 설정을 두 곳에서
관리한다는 건 언제든 어긋날 수 있다는 뜻이고, 어긋났을 때의 증상이 하필
"RF가 안 되는 것처럼 보인다"라서 특히 위험합니다.

## 파일

| 파일 | 역할 |
|---|---|
| `spidevtransport.h/.cpp` | `ITransport` 구현체 (spidev 직접 제어) |
| `smoke_spidev_send_main.cpp` | 송신측 CLI — 핸드셰이크 → DATA → END |
| `smoke_spidev_recv_main.cpp` | 수신측 CLI — 패킷 디코딩 + ACK 응답 |

`tests/smoke_send_main.cpp`·`smoke_recv_main.cpp`(정식 경로용)와 로직은
동일하고 `ITransport` 구현체만 다릅니다. `session/simplesender`·
`simplereceiver`는 양쪽이 공유합니다 — `ITransport` 추상화를 둔 이유가
바로 이것입니다.
