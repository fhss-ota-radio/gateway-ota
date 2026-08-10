# gateway-ota

OTA 매니저 Qt/C++ 앱.

## 핵심 기능
- `.bin` 파일 선택 및 청크 분할 (`ota-protocol` 규격)
- `/dev/cc1101` 경유 전송, 진행률·로그 표시
- ACK/NACK 기반 재전송 큐
- 유니캐스트(특정 단말) / 브로드캐스트(1:N) 전송 모드

## 담당
팀원3, 4

주요 변경 사항
Cc1101Transport 실제 구현
/dev/cc1101을 POSIX open()으로 연결
write()를 이용한 OTA packet 송신
poll() + read() 기반 수신 인터페이스 구현
ioctl()을 이용한 RX 진입, 채널 변경, RX/TX FIFO 초기화
시스템콜 실패 상태를 Cc1101Status로 변환하도록 처리
Qt와 CC1101 드라이버 연결
기존에는 연결 버튼 클릭 시 UI 상태만 변경했으나, 실제 Cc1101Transport::open() 결과에 따라 연결 상태를 표시하도록 변경
/dev/cc1101 연결 실패 시 오류 로그 출력
연결 성공 시 CC1101을 RX 상태로 전환
OTA BIN 순차 전송 구현
선택한 .bin 파일을 BinSplitter로 분할
각 청크를 OTA protocol packet으로 생성
생성된 packet을 순서대로 /dev/cc1101에 전달
전송 진행률 및 현재 송신 청크를 Qt UI에 표시
전송 일시정지 / 재개 기능 추가

기존 BinSplitter는 OTA 헤더와 payload를 포함한 최종 전송 packet까지 생성하도록 구현되어 있어 해당 결과를 그대로 Transport 계층으로 전달하도록 연결했습니다.

Packet 크기 제한 수정

CC1101 커널 드라이버에서 허용하는 packet 크기를 고려하여 OTA payload 크기를 조정했습니다.

CC1101 driver 최대 packet : 61 bytes
OTA protocol header        : 9 bytes
Firmware payload 최대      : 52 bytes

따라서 기존 UI의 최대 chunk size 55 byte를 실제 드라이버와 호환되는 52 byte 이하로 제한하도록 수정했습니다. 기존 UI에서는 최대 55 byte로 설정되어 있었습니다.
