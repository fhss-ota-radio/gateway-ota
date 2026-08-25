# 라즈베리파이에서 큐티(Qt) GUI 돌리는 법

`OTA_System`(Qt GUI 앱)을 디스플레이 없는 라즈베리파이(욕토 이미지)에서
빌드·실행하면서 겪은 문제와 해결법 정리. 2026-08-25 기준.

## 0. 왜 이렇게 복잡한가

이 프로젝트가 쓰는 라즈베리파이 이미지는 욕토(Yocto — 임베디드 리눅스를
커스텀으로 찍어내는 빌드 시스템)로 만든 **최소 구성** 이미지입니다.
데스크톱 환경(X11/Wayland)도, 배터리 백업 시계(RTC)도, 일반적인 apt
저장소 설정도 기본으로는 없을 수 있습니다. 아래는 그로 인해 실제로
겪은 문제들과 각각의 해결법입니다.

## 1. 시계가 틀려서 git/apt가 다 막힘

**증상**: `git fetch`/`git pull`/`apt update`가
`SSL certificate problem: certificate is not yet valid`로 실패.

**원인**: RTC(배터리로 유지되는 하드웨어 시계)가 없는 파이는 전원이
꺼졌다 켜지면 시계가 리셋돼서, 인터넷 연결 전엔 훨씬 과거 날짜(예:
2018년)로 잡혀 있습니다. HTTPS 인증서는 "이 날짜 이후부터 유효"라는
시작 시각이 있는데, 파이 시계가 그보다 과거면 인증서 자체가 아직
유효하지 않다고 거부당합니다. git/apt 둘 다 HTTPS를 쓰므로 둘 다
막힙니다.

**해결**:
```bash
# root로 로그인돼 있으면 sudo 자체가 없을 수 있음 — 명령어만 그대로 실행
timedatectl set-ntp true 2>/dev/null
sleep 5
date

# 그래도 안 맞으면(사내망이라 NTP 서버에 못 닿는 경우 등) 수동으로
date -s "2026-08-25 12:00:00"
```

## 2. cmake가 없음 — apt로 되는지, 아니면 Makefile.pi

**증상**: `cmake: command not found`.

**원인**: 최소 이미지라 Qt6/cmake가 기본으로 안 들어있습니다.

**해결 두 가지**:

**A) apt로 설치가 되는 파이라면** (이미지에 apt 저장소가 정상 설정된
경우 — 시계 문제만 해결하면 됨):
```bash
apt update
apt install -y cmake build-essential qt6-base-dev
cmake -S . -B build && cmake --build build
```

**B) apt 저장소 자체가 비어 있는 파이라면** (예: 149 — `sources.list`
내용이 아예 없어서 `apt install`이 전부 실패하는 경우): cmake 없이
빌드하는 대체 경로가 있습니다 — `OTA_System/Makefile.pi`.

```bash
cd gateway-ota/OTA_System
make -f Makefile.pi ota_smoke_discover        # CLI 도구 하나만
make -f Makefile.pi smoke                     # 실기기 스모크테스트 전부
```

단, **Makefile.pi는 CLI 테스트 도구만 빌드합니다** (`ota_core`가 Qt
의존성이 없게 설계돼 있어서 g++/make만으로 됨). **Qt GUI 앱
(`OTA_System` 자체)은 이 방법으로 못 만듭니다** — Qt6이 진짜로 없으면
GUI는 cmake+Qt6 둘 다 있는 환경(A) 또는 맥에서 크로스컴파일해서
바이너리만 옮기는 방법밖에 없습니다.

> Makefile.pi는 `OTA_System/.gitignore`의 `Makefile*` 규칙에 걸려서
> 한동안 git에 커밋이 안 되고 있었습니다(2026-08-25에 `!Makefile.pi`
> 예외 처리해서 수정 — 지금은 `git pull`로 정상적으로 받아집니다).

## 3. GUI 화면을 어떻게 보는가 — VNC만 됩니다

이 파이엔 디스플레이가 물리적으로 없거나(또는 있어도 아래 이유로 못
씀), X11/Wayland 데스크톱도 없습니다. Qt6에 설치된 화면 백엔드(QPA,
Qt Platform Abstraction plugin) 목록을 직접 확인해보면:

```bash
find / -path "*/plugins/platforms/*" -iname "*.so" 2>/dev/null
```

이 이미지엔 다음이 있었습니다: `libqminimal.so`, `libqvnc.so`,
`libqoffscreen.so`, `libqxcb.so`, `libqvkkhrdisplay.so`.

| 플러그인 | 됨? | 이유 |
|---|---|---|
| `vnc` | ✅ **이것만 씀** | VNC(원격 화면 프로토콜) 서버로 직접 떠서, 데스크톱 환경 없이도 원격에서 화면을 볼 수 있음 |
| `xcb` | ❌ | X11 서버가 실제로 떠 있어야 하는데, 이 이미지엔 X 서버 자체가 없음 |
| `vkkhrdisplay` | ❌ | Vulkan `VK_KHR_display` 확장으로 화면에 직접 그리는 방식인데, **`QWindow`가 Vulkan 서피스일 때만 지원**함. `OTA_System`은 일반 `QWidget`/`QMainWindow`(`QtWidgets`) 기반이라 구조적으로 안 맞음 — 앱을 Vulkan 전용으로 다시 짜지 않는 한 불가능. 실행하면 "vkkhrdisplay platform plugin only supports QWindow with surfaceType == VulkanSurface" 에러 후 멈춤 |
| `linuxfb`, `eglfs` | ❌ (플러그인 자체가 없음) | 라즈베리파이 GUI 앱에서 보통 가장 흔히 쓰는 방식(프레임버퍼 직결/GPU 가속 직결)인데, 이 Qt6 빌드엔 아예 포함이 안 돼 있음. LCD에 직접(VNC 없이) 띄우려면 이 플러그인들이 포함된 Qt6를 다시 빌드/설치해야 하는데, 지금 시도해본 적은 없음(향후 과제로 남김) |

**결론: 지금은 VNC로만 화면을 볼 수 있습니다.**

## 4. 실행/종료 방법

**추천: tmux 사용** (SSH 끊겨도 안 죽고, 실시간 로그 그대로 보임):
```bash
which tmux || apt install -y tmux

tmux new -s qt
# --- tmux 세션 안 ---
cd /home/root
QT_QPA_PLATFORM=vnc ./OTA_System
```
- 세션에서 나가기(프로그램은 계속 돎): `Ctrl+B` 뗀 다음 `D`
- 다시 붙기: `tmux attach -t qt`
- 종료: 세션 안에서 `Ctrl+C`

**tmux 없으면 (대안)**:
```bash
cd /home/root
QT_QPA_PLATFORM=vnc ./OTA_System > /tmp/ota.log 2>&1 &
tail -f /tmp/ota.log     # 로그 보기
killall OTA_System 2>/dev/null   # 종료
```

**맥에서 화면 보기**: Finder에서 `Cmd+K` → `vnc://<파이IP>:5900`,
또는 RealVNC Viewer / TigerVNC Viewer 앱.

## 5. 확인된 것들

- 한글(CJK) 폰트 번들링(`main.cpp`의 `loadKoreanFont()`,
  2026-08-25 커밋)이 정상 동작 — 실행 로그에
  `Loaded font: "Noto Sans CJK JP"` 확인됨.
- `Detected locale "C" ... Qt depends on a UTF-8 locale, and has
  switched to "C.UTF-8" instead.` 경고는 무해함 — Qt가 알아서
  UTF-8 로케일로 전환하고 계속 실행됨.

## 6. 앞으로 볼 것 (미해결)

- LCD에 VNC 없이 직접 띄우기(`linuxfb`/`eglfs` 플러그인 포함된 Qt6
  구하거나 직접 빌드) — 지금은 시도 안 함
- 맥에서 Docker+QEMU로 크로스컴파일하는 방법 — 149 환경 점검 때
  검토했지만 아직 실행 안 함 (`docs/note/design-notes-gateway-ota-es.md`
  69절)
