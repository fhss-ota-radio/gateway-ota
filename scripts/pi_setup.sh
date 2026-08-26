#!/bin/bash
# 라즈베리파이(욕토/Yocto 이미지)에서 gateway-ota Qt GUI를 셋업+빌드하는
# 스크립트. root로 로그인된 파이(sudo 자체가 없는 최소 이미지)와, sudo가
# 있는 일반 파이 둘 다에서 그대로 쓸 수 있게 만듦.
#
# 사용법:
#   ./scripts/pi_setup.sh [브랜치명]
#   (브랜치명 생략하면 feature/qt-fhss-transfer)
#
# 이 스크립트가 해결하는 문제들 (docs/running-qt-gui-on-raspberry-pi.md 참고):
#   1) RTC(배터리 백업 시계) 없는 파이는 부팅 시 시각이 과거로 리셋돼서,
#      HTTPS 인증서 "아직 유효하지 않음" 오류로 git/apt가 막힘 -> NTP로
#      맞추고, 안 되면 수동으로라도 대략 맞춤.
#   2) root로 로그인된 파이엔 sudo 자체가 없어서 "sudo: command not found"로
#      실패할 수 있음 -> root면 sudo 없이, 아니면 sudo를 붙여서 실행.
#   3) cmake/qt6이 apt로 설치 가능한 파이라고 가정 — apt 저장소 자체가 없는
#      파이(예: 149)라면 이 스크립트 대신 OTA_System/Makefile.pi로 CLI
#      도구만 빌드해야 함(Qt GUI는 그 경우 이 방법으로 못 만듦).
set -e

BRANCH="${1:-feature/qt-fhss-transfer}"

# root면 sudo 자체가 없거나 필요 없으므로 SUDO를 비워둠
if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
else
    SUDO="sudo"
fi

echo "=== 1) 현재 시각 ==="
date

echo "=== 2) NTP로 시각 동기화 시도 ==="
$SUDO timedatectl set-ntp true 2>/dev/null || true
sleep 5
date

CURRENT_YEAR=$(date +%Y)
if [ "$CURRENT_YEAR" -lt 2025 ]; then
    echo "=== NTP로 안 맞춰짐(여전히 ${CURRENT_YEAR}년) — 수동으로 날짜 설정 ==="
    $SUDO date -s "2026-08-25 12:00:00"
    date
else
    echo "=== NTP로 정상 동기화됨(${CURRENT_YEAR}년) ==="
fi

echo "=== 3) 빌드 도구 설치 (cmake, build-essential, qt6) ==="
$SUDO apt update
$SUDO apt install -y cmake build-essential qt6-base-dev

echo "=== 4) gateway-ota 레포 갱신 (브랜치: ${BRANCH}) ==="
cd "$(dirname "$0")/.."
git fetch origin
git checkout "$BRANCH"
git pull

echo "=== 5) 빌드 ==="
cmake -S . -B build
cmake --build build

echo "=== 완료 — OTA_System 실행 파일 확인 ==="
find build -maxdepth 2 \( -name "OTA_System" -o -name "OTA_System.app" \) 2>/dev/null
