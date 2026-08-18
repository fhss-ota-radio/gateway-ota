# 처음부터 따라하기 — 라즈베리파이 2대로 CC1101 파일 전송 테스트

> **대상**: 이 프로젝트를 처음 받아서 CC1101 무선 전송을 직접 돌려보려는 팀원
> **결과**: 51,200byte 파일을 한 파이에서 다른 파이로 무선 전송.
> 실측 수신률은 **99~100%** (재전송 미구현이라 매번 다릅니다 — 6절 참고)
> **소요**: 처음이면 1~2시간 (재부팅 2회 포함), 두 번째부터는 15분

이 문서는 **클론부터 실행까지 한 번에** 따라갈 수 있게 쓴 것입니다.
막히면 각 절 끝의 "안 될 때" 박스를 보세요.

---

## 준비물 체크리스트

| 항목 | 개수 | 확인 |
|---|---|---|
| 라즈베리파이 | 2대 | 서로 SSH 접속 가능 |
| CC1101 모듈 (433MHz) | 2개 | **안테나 반드시 장착** |
| 안테나 | 2개 | ⚠️ 불량품이 흔합니다. 여분 확보 권장 |
| 브레드보드 + 점퍼선 | — | |
| 빌드 서버 | 1대 | 커스텀 커널이면 필요 (0절에서 판단) |

> **⚠️ 시작 전 경고 3가지**
>
> 1. **CC1101은 최대 3.6V입니다. 5V를 넣으면 칩이 탑니다.**
> 2. **안테나 없이 송신하지 마세요.** 반사파로 칩이 손상될 수 있습니다.
> 3. **싱크워드를 확인하세요.** 이 레포는 OTA 전용값 `0x2D/0xD4`를 씁니다.
>    다른 팀 장비와 통신하려면 값을 맞춰야 합니다 (7절).

---

## 0. 먼저 판단 — 내 파이의 커널이 표준인가 커스텀인가

**이 판단을 건너뛰면 나중에 `insmod`가 거부당하고 원인을 못 찾습니다.**

```sh
uname -r
cat /proc/version
```

| `/proc/version`에 보이는 것 | 종류 | 따라갈 절 |
|---|---|---|
| `(builder@...)`, 라즈베리파이 재단 공식 | **표준 커널** | 2-A (파이에서 직접 빌드) |
| `(ubuntu@ubuntu24)` 같은 낯선 계정 | **커스텀 커널** | 2-B (빌드 서버에서 크로스컴파일) |

**왜 이게 중요한가**: 커널 모듈(`.ko`)은 실행 중인 커널과 **정확히 같은
버전·설정**으로 빌드돼야만 로드됩니다. 비슷한 버전의 헤더로 빌드해도
**vermagic**(모듈에 박히는 커널 버전 식별 문자열)이나
**MODVERSIONS**(심볼별 체크섬)가 안 맞으면 `insmod`가
`Invalid module format`으로 거부합니다. `insmod -f`(force, 강제)도
이 커널은 `CONFIG_MODULE_FORCE_LOAD`가 꺼져 있어 안 통합니다.

> **우리 프로젝트의 파이 2대(pi24, pi06)는 커스텀 커널입니다.** → 2-B로.

---

## 1. 클론 — 레포 3개를 형제 폴더로

**⚠️ 폴더 배치가 중요합니다.** `gateway-ota`의 빌드 스크립트가
`../ota-protocol/include`를 상대경로로 찾습니다. 나란히 두지 않으면
CMake가 에러를 냅니다.

```sh
mkdir -p ~/FHSS && cd ~/FHSS

git clone git@github.com:fhss-ota-radio/kernel-cc1101-spi.git
git clone git@github.com:fhss-ota-radio/gateway-ota.git
git clone git@github.com:fhss-ota-radio/ota-protocol.git
```

결과가 이 모양이어야 합니다:

```
~/FHSS/
├─ kernel-cc1101-spi/    ← 커널 드라이버
├─ gateway-ota/          ← OTA 송수신 앱
└─ ota-protocol/         ← 두 레포가 공유하는 패킷 규격 (header-only)
```

**브랜치를 맞추세요.** 검증된 코드는 `main`이 아니라 작업 브랜치에 있습니다:

```sh
cd ~/FHSS/kernel-cc1101-spi && git checkout feature/cc1101_V1
cd ~/FHSS/gateway-ota       && git checkout feature/simple-ota-send-test
```

> **안 될 때**
> - `Permission denied (publickey)` → GitHub SSH 키가 등록 안 됨.
>   `ssh-keygen -t ed25519` 후 `~/.ssh/id_ed25519.pub` 내용을 GitHub
>   Settings → SSH keys에 등록. 또는 HTTPS로 클론:
>   `git clone https://github.com/fhss-ota-radio/....git`

---

## 2. 커널 모듈(`cc1101.ko`) 빌드

### 2-A. 표준 커널 — 파이에서 직접 빌드

```sh
sudo apt update
sudo apt install -y raspberrypi-kernel-headers build-essential device-tree-compiler

cd ~/FHSS/kernel-cc1101-spi
make
ls -l cc1101.ko          # 생겼으면 성공
```

### 2-B. 커스텀 커널 — 빌드 서버에서 크로스컴파일

**크로스컴파일(cross-compile)** = "만드는 기계와 돌릴 기계가 다른 빌드".
빌드 서버는 x86_64인데 결과물은 ARM에서 돌아야 하므로, ARM용 컴파일러를
써야 합니다.

**① 정확히 일치하는 커널 소스 찾기**

커스텀 커널을 빌드한 사람에게 물어보세요. 우리 프로젝트는:

| 항목 | 값 |
|---|---|
| 빌드 서버 | `ubuntu@10.10.16.54` |
| 커널 소스 | `/home/ubuntu/pi_bsp/kernel/linux` |
| 작업 폴더 | `~/pi_bsp/drivers/cc1101` |

확인:
```sh
cat /home/ubuntu/pi_bsp/kernel/linux/include/config/kernel.release
```
이 값이 파이의 `uname -r`과 **완전히 동일**해야 합니다.

**② 소스를 빌드 서버로 올리고 빌드**

```sh
# 내 PC에서 — 소스를 빌드 서버로
scp -r ~/FHSS/kernel-cc1101-spi/*.c \
       ~/FHSS/kernel-cc1101-spi/*.h \
       ~/FHSS/kernel-cc1101-spi/Makefile \
       ubuntu@10.10.16.54:~/pi_bsp/drivers/cc1101/

# 빌드 서버에서
ssh ubuntu@10.10.16.54
cd ~/pi_bsp/drivers/cc1101
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
     KDIR=/home/ubuntu/pi_bsp/kernel/linux

modinfo cc1101.ko | grep vermagic   # 파이의 uname -r과 같은지 확인
```

- `ARCH=arm` — 32비트 ARM으로 빌드 (파이가 64비트 OS면 `arm64`)
- `CROSS_COMPILE=arm-linux-gnueabihf-` — 이 접두사가 붙은 툴체인
  (`arm-linux-gnueabihf-gcc` 등)을 쓰라는 뜻.
  `gnueabihf` = GNU EABI **H**ard **F**loat(하드웨어 부동소수점 연산)
- `KDIR` = **K**ernel **DIR**ectory, ①에서 찾은 소스 경로

**③ 파이 2대로 전송**

```sh
# 빌드 서버에서
scp cc1101.ko pi24@10.10.16.84:~/
scp cc1101.ko pi06@10.10.16.66:~/
```

`scp` = **s**ecure **cp**(copy). `cp`에 SSH 암호화를 씌운 것입니다.
원격은 `사용자@호스트:경로` 형태로 쓰는데, **콜론(`:`)이 "여기부터 원격
경로"라는 구분자**입니다 — 빠뜨리면 그냥 로컬 파일로 복사됩니다.

> **안 될 때**
> - `make`가 아무 것도 안 하고 끝남(`CC [M]` 줄이 안 보임) → 소스가 실제로
>   전송 안 됐을 수 있습니다. `grep -c "" cc1101_main.c`로 파일이 있는지 먼저 확인
> - `Invalid module format` → vermagic 불일치. ①로 돌아가 커널 소스가
>   정말 일치하는지 확인
> - 매번 비밀번호 치기 번거로우면: `ssh-copy-id pi24@10.10.16.84`

---

## 3. 배선

**GDO0/GDO2는 CC1101이 "패킷 다 보냈다 / 받았다"를 커널에 알리는 인터럽트
핀입니다.** 이게 틀리면 `TX 타임아웃`이 납니다.

| CC1101 핀 | 라즈베리파이 | 물리 핀 번호 |
|---|---|---|
| VCC | **3.3V** ⚠️ (5V 금지) | 1 또는 17 |
| GND | GND | 6, 9, 14, 20, 25... |
| SCK | GPIO11 (SCLK) | 23 |
| MOSI | GPIO10 (MOSI) | 19 |
| MISO | GPIO9 (MISO) | 21 |
| CSN | GPIO8 (CE0) | 24 |
| **GDO0** | **GPIO24** | 18 |
| **GDO2** | **GPIO25** | 22 |

> **⚠️ 팀에 공유된 배선표가 틀렸습니다.** 전달받은 표는 GDO0=GPIO25,
> GDO2=GPIO24였는데 **실기기 검증 결과 반대**였습니다. 위 표(레포 기본값)가
> 맞습니다. 표대로 스왑하면 `cc1101 spi0.0: TX 타임아웃`이 납니다.
>
> **새 보드는 직접 검증하세요** — 6절의 `/proc/interrupts` 방법.

**브레드보드 전원모듈(MB102 / HW-131)을 쓴다면:**

- 전압 점퍼가 **3.3V**에 꽂혀 있는지 확인 (5V면 칩 손상)
- **3.3V 출력 지점이 여러 곳인데 서로 연결하면 발열합니다.** 한 곳만 쓰세요
  (가운데 핀과 레일 점퍼를 이어놨다가 모듈이 뜨거워진 사례가 있었습니다)
- **파이와 GND를 반드시 공유하세요.** 전원만 따로 주고 GND를 안 묶으면
  SPI/GDO 신호의 기준 전압이 안 맞아 증상이 더 이상해집니다
- 연결 전에 멀티미터로 3.3V가 나오는지 먼저 재보세요

---

## 4. 디바이스트리 오버레이(DTO) 설정

**디바이스트리(Device Tree)** = 리눅스 커널에게 "이 보드에 어떤 하드웨어가
어디 붙어 있는지" 알려주는 하드웨어 명세서입니다. x86 PC는 BIOS가 알려주지만,
ARM 임베디드는 그런 게 없어서 별도 파일로 기술합니다.

**오버레이(overlay, 덧씌우기)** = 기본 디바이스트리를 통째로 바꾸지 않고
"여기에 이것만 추가"하는 조각 파일입니다. 확장자 `.dts`(소스) →
컴파일 → `.dtbo`(**D**evice **T**ree **B**lob **O**verlay).

### ① 오버레이 컴파일

```sh
cd ~/FHSS/kernel-cc1101-spi
which dtc || sudo apt install -y device-tree-compiler
dtc -@ -I dts -O dtb -o cc1101.dtbo dts/cc1101-overlay.dts
```

- `dtc` = **D**evice **T**ree **C**ompiler
- `-@` — 심볼 정보를 남겨서 나중에 파라미터로 값을 바꿀 수 있게 함
- `-I dts -O dtb` — **I**nput 형식은 dts, **O**utput 형식은 dtb

### ② 파이에 설치

**부트 파티션 경로가 OS 버전마다 다릅니다. 먼저 확인하세요:**

```sh
ls /boot/firmware/overlays/ 2>/dev/null && echo "→ /boot/firmware/ 사용" \
  || echo "→ /boot/ 사용"
```

최신 Raspberry Pi OS(bookworm 이후)는 `/boot/firmware/`입니다:

```sh
sudo cp cc1101.dtbo /boot/firmware/overlays/
echo "dtparam=spi=on"  | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=cc1101" | sudo tee -a /boot/firmware/config.txt
sudo reboot
```

- `dtparam=spi=on` — 파이의 SPI 컨트롤러를 켬
- `dtoverlay=cc1101` — 방금 넣은 오버레이를 적용

### ③ 재부팅 후 확인

```sh
sudo insmod ~/cc1101.ko
dmesg | grep -i cc1101       # "CC1101 감지됨", "/dev/cc1101 등록 완료"
ls -l /dev/cc1101            # 파일이 생겼는지
grep cc1101 /proc/interrupts # gdo0, gdo2 둘 다 등록됐는지
```

**두 파이 모두에서** 이게 나와야 다음으로 갑니다.

> **핀 번호를 바꿔서 테스트하고 싶다면** 재컴파일 없이 파라미터로 됩니다:
> ```sh
> sudo sed -i 's/^dtoverlay=cc1101$/dtoverlay=cc1101,gdo0_pin=25,gdo2_pin=24/' \
>   /boot/firmware/config.txt
> sudo reboot
> ```
> 쓸 수 있는 파라미터: `gdo0_pin`, `gdo2_pin`, `speed`(SPI 클럭 Hz)

> **안 될 때**
> - `/dev/cc1101`이 안 생김 → `dmesg | grep -i cc1101`에 에러가 있는지 확인.
>   아무 것도 없으면 오버레이가 적용 안 된 것(`config.txt` 경로 확인)
> - `insmod: ERROR: could not insert module: File exists` → 이미 올라가 있음.
>   `sudo rmmod cc1101` 후 다시
> - `rmmod: ERROR: Module cc1101 is in use` → `/dev/cc1101`을 잡고 있는
>   프로그램이 있음. **드라이버는 동시에 하나만 open을 허용합니다**

---

## 5. OTA 앱 빌드

**두 파이 모두**에서 합니다. Qt는 필요 없습니다 — 전송 로직은 Qt 의존성이
전혀 없어서 `g++`만으로 빌드됩니다.

### 방법 A. CMake (권장)

```sh
cd ~/FHSS/gateway-ota/OTA_System
cmake -B build
cmake --build build --target ota_smoke_send ota_smoke_recv
```

### 방법 B. g++ 단독 (Qt/CMake 없이)

```sh
cd ~/FHSS/gateway-ota/OTA_System

# 송신
g++ -std=c++17 -I. -Icore -Itransport -Isession -I../../ota-protocol/include \
  core/binsplitter.cpp session/simplesender.cpp session/simplereceiver.cpp \
  transport/cc1101transport.cpp tests/smoke_send_main.cpp \
  -o ota_smoke_send

# 수신
g++ -std=c++17 -I. -Icore -Itransport -Isession -I../../ota-protocol/include \
  session/simplereceiver.cpp transport/cc1101transport.cpp \
  tests/smoke_recv_main.cpp \
  -o ota_smoke_recv
```

### 로직만 먼저 검증하고 싶다면 (하드웨어 불필요)

```sh
cmake --build build --target ota_core_tests
ctest --test-dir build -R ota_core_tests    # 7개 통과해야 정상
```

> **안 될 때**
> - `ota-protocol을 못 찾았습니다` → 1절의 폴더 배치를 확인하세요.
>   다른 위치에 뒀다면:
>   `cmake -B build -DOTA_PROTOCOL_INCLUDE_DIR=/경로/ota-protocol/include`

---

## 6. 실행 — 파일 전송 테스트

### ① 테스트 파일 만들기 (송신측 파이에서)

```sh
head -c 51200 /dev/urandom > test.bin
sha256sum test.bin        # 이 값을 적어두세요
```

`/dev/urandom`은 무작위 바이트를 무한히 뱉는 리눅스 특수 파일입니다.
`head -c 51200`로 51,200byte만 잘라냅니다. **무작위 데이터를 쓰는 이유**는
0으로 채워진 파일이면 오프셋이 밀려도 결과가 같아 보여서 검증이 안 되기
때문입니다.

### ② 수신측 파이 먼저 띄우기

**순서가 중요합니다. 수신측이 먼저 대기하고 있어야 핸드셰이크가 성립합니다.**

```sh
sudo ./ota_smoke_recv /dev/cc1101 recv.bin
```

두 번째 인자(`recv.bin`)를 주면 받은 DATA를 sequence 순서대로 재조립해서
그 파일에 씁니다. 생략하면 로그만 출력합니다.

### ③ 송신측 파이에서 전송

```sh
sudo ./ota_smoke_send /dev/cc1101 test.bin ffffffff 40
```

| 인자 | 값 | 의미 |
|---|---|---|
| 1 | `/dev/cc1101` | 디바이스 경로 |
| 2 | `test.bin` | 보낼 파일 |
| 3 | `ffffffff` | 대상 device_id. `ffffffff` = **브로드캐스트**(전체) |
| 4 | `40` | 청크 간 대기 ms. **⚠️ 40 미만은 실패합니다** |

> **⚠️ 4번째 인자를 반드시 40으로 주세요.** 기본값 10ms에서는 수신이
> 송신 속도를 못 따라가 패킷 경계가 밀립니다 — 깨진 패킷 꼬리에 다음
> 패킷의 프리앰블(`AA AA AA`)과 싱크워드(`2D D4`)가 딸려 들어오고,
> 디코딩 성공률이 4개 중 1개꼴로 떨어집니다.
> 40ms면 1067청크에 **약 43초** 걸립니다. (수신 처리량 개선은 남은 과제)

### ④ 성공했을 때 보이는 것

**수신측:**
```
[smoke_recv] Start  session_id=0x5107beee imageSize=51200 totalChunks=1067
[smoke_recv] Data   seq=0 len=48
...
[smoke_recv] End
  기대 청크: 1067 / 채워진 청크: 1067 / 중복: 0 / 누락: 0
```

**누락이 0이면 해시로 확인:**

```sh
sha256sum recv.bin          # 수신측
# 송신측에서 적어둔 값과 비교
```

### ⚠️ 누락이 몇 개 나오는 게 정상입니다

**재전송이 아직 구현 안 돼 있어서(마일스톤 4), 무선 손실이 그대로 구멍으로
남습니다.** 실측 수신률은 99~100% 사이에서 매번 달라집니다.

| 회차 | 결과 |
|---|---|
| 2026-08-16 | 1067/1067 |
| 2026-08-17 1차 | 811/1067 (순간 간섭) |
| 2026-08-17 2차 | 1059/1067 |

**그래서 해시 일치는 신뢰할 만한 판정 기준이 아닙니다** — RF 운에 좌우되니까요.
대신 **"받은 청크가 전부 올바른 위치에 쓰였는가"**를 봅니다:

```sh
# 수신측 recv.bin을 송신측으로 옮긴 뒤, 송신측에서
cmp -l test.bin recv.bin | awk '{print int(($1-1)/48)}' | sort -n | uniq -c
```

`cmp -l`은 다른 바이트의 위치를 전부 나열하고(**l**ist), `awk`가 그 위치를
48로 나눠 청크 번호로 바꿉니다.

**출력에 나오는 청크 번호가 "누락 청크" 목록과 정확히 일치하면 성공입니다.**
받은 청크는 전부 정확한 오프셋에 정확하게 기록됐다는 뜻이에요. 다른 번호가
섞여 나오면 재조립 오프셋 버그입니다.

> 청크당 48이 아니라 47이 나오는 경우가 있는데 정상입니다. 누락 구간은
> `0`으로 남는데, 원본의 그 자리 바이트가 우연히 `0x00`이면 일치해버리거든요.
> 무작위 데이터라 256분의 1 확률로 일어납니다.

---

## 7. ⚠️ 다른 팀 장비와 통신하려면 — 싱크워드

**이 레포는 OTA 전용 싱크워드 `0x2D/0xD4`를 씁니다.** 팀 공용 기본값
`0xD3/0x91`에서 바꾼 것입니다.

**왜 바꿨나**: 팀원 전원이 TI 레퍼런스 기본 설정을 그대로 써서
**433.92MHz + 싱크워드가 전부 겹쳤습니다.** 남의 패킷이 CRC까지 통과해
정상 패킷으로 올라와 커널 RX 큐(512byte)를 채우고, 그러면 우리 패킷은
도착해도 폐기됐습니다. GDO2 인터럽트 1182회 중 대부분이 남의 패킷이었고,
**원인 찾는 데 하루가 걸렸습니다.**

CC1101은 싱크워드가 다르면 **하드웨어 단에서 무시**합니다 — 인터럽트도
안 울리고 FIFO에도 안 들어옵니다. 그래서 격리가 완벽하고 CPU 부담이 0입니다.

**바꿔야 한다면 두 곳을 동시에 고치세요:**

| 파일 | 위치 |
|---|---|
| `kernel-cc1101-spi/cc1101_core.c` | `cc1101_default_regs[]`의 `CC1101_SYNC1`/`SYNC0` |
| `gateway-ota/OTA_System/tools/spidev/spidevtransport.cpp` | 레지스터 배열 인덱스 4, 5 |

**한쪽만 바꾸면 두 경로가 통신하지 못합니다.** 그리고 그 증상이 겉으로는
"RF가 안 된다"로 보여서 디버깅 비용이 큽니다.

> **싱크워드 값 고르는 법**: 1과 0이 균형 잡히고(8:8) 반복 패턴이 없어야
> 오검출(false sync)이 적습니다. `0x2DD4`가 이 조건을 만족합니다.

> **채널(`CHANNR`)로 분리하려는 경우 주의**:
> 주파수 = 433.92MHz + CHANNR × 약 200kHz입니다.
> 국내 433MHz ISM 밴드는 433.05~434.79MHz라 **채널 4(434.72MHz)가 상한**,
> 채널 5부터는 전파법 위반입니다. 큰 값(예: 200)은 473.92MHz로
> **CC1101 지원 밴드(387~464MHz)조차 벗어나 PLL이 락을 못 걸어 송수신이
> 통째로 죽습니다.**

---

## 8. 안 될 때 — 원인 계층부터 가르세요

CC1101 문제는 원인 후보가 **안테나 → 전원 → 배선 → SPI → 칩 설정 → 커널
인터럽트 → 큐 → 유저공간**으로 넓게 퍼져 있습니다. 아무 데나 손대면 하루가
날아갑니다. **이 두 줄이 시간을 가장 많이 아껴줍니다:**

```sh
grep cc1101 /proc/interrupts        # 인터럽트가 울리는가?
sudo ./cc1101_diag --set-rx         # 칩 설정/상태가 정상인가?
```

| 관찰 | 의미 | 볼 곳 |
|---|---|---|
| 카운트가 **전혀 안 올라감** | 전파가 칩까지 안 옴 | **물리 계층** — 안테나·전원·배선 |
| 카운트는 올라가는데 **데이터가 깨짐** | 신호는 오는데 해석이 틀림 | **소프트웨어** — 설정·읽기 타이밍 |
| 카운트가 **비정상 폭증**(수천만) | 인터럽트 폭주 | 칩 상태 복귀 처리 |

30초 간격으로 두 번 찍어 비교하세요:
```sh
grep cc1101 /proc/interrupts; sleep 30; grep cc1101 /proc/interrupts
```

### 진단 도구 — 추측하지 말고 재세요

```sh
cd ~/FHSS/kernel-cc1101-spi
gcc -O2 -o cc1101_diag tools/cc1101_diag.c -I.
sudo ./cc1101_diag --set-rx
```

칩 레지스터 17개를 기대값과 비교하고 `MARCSTATE`(칩 내부 상태머신)를
사람이 읽을 수 있는 이름으로 출력합니다.

- `MARCSTATE = 0x0D (RX)` → 칩은 정상. 다른 데를 보세요
- `PARTNUM`이 `0x00`이 아님 → **SPI 통신이 깨지고 있습니다.**
  CC1101의 `PARTNUM`은 항상 `0x00`이어야 합니다

> ⚠️ 드라이버는 **동시에 하나만 open**을 허용하므로, 진단 도구와 수신
> 프로그램을 같이 못 돌립니다(`-EBUSY`).

### 가장 흔한 원인 3가지

**1. 안테나 (하루에 4번 겪었습니다)**

> **⚠️ 결정적 진단 힌트: "송신은 되는데 수신만 안 된다"면 안테나입니다.**
> 송신은 칩이 신호를 밀어내는 거라 안테나가 부실해도 근거리면 상대가
> 받습니다. 반면 수신은 공기 중의 미약한 신호를 긁어모아야 해서 안테나
> 성능에 훨씬 민감합니다. 이 비대칭 패턴을 보면 **다른 걸 보기 전에
> 안테나부터** 확인하세요.

순서: ① SMA 커넥터를 손으로 꽉 조이기 → ② **다른 안테나로 교체**
(헐거운 게 아니라 안테나 자체가 불량인 경우가 실제로 있었습니다) →
③ 두 보드를 30cm 이내로 붙여서 재시도

**2. 싱크워드 충돌** → 7절

**3. 배선 (GDO0/GDO2 스왑)** → 3절

### 전체 트러블슈팅

**`kernel-cc1101-spi/docs/troubleshooting-cc1101.md`** — 증상별 8장 가이드.
CC1101 쓰시는 분은 한 번 읽어두시면 좋습니다.

---

## 9. 두 번째부터는 이것만

```sh
# 파이 양쪽
sudo rmmod cc1101; sudo insmod ~/cc1101.ko && ls /dev/cc1101

# 수신측
sudo ./ota_smoke_recv /dev/cc1101 recv.bin

# 송신측
sudo ./ota_smoke_send /dev/cc1101 test.bin ffffffff 40

# 검증
sha256sum recv.bin   # 송신측 test.bin 해시와 비교
```

---

## 참고 문서

| 문서 | 용도 |
|---|---|
| `kernel-cc1101-spi/docs/troubleshooting-cc1101.md` | **안 될 때 증상별 가이드** |
| `kernel-cc1101-spi/docs/pi-bringup-guide.md` | 커널 모듈 빌드/적재 상세 |
| `gateway-ota/docs/file-transfer-guide.md` | 전송이 코드 안에서 어떻게 도는지 |
| `gateway-ota/docs/roadmap.md` | 진행 상황·다음 할 일 |
| `gateway-ota/OTA_System/tools/spidev/README.md` | 커널 우회 진단 경로 |
| `ota-protocol/README.md` | 패킷 규격 |
