#ifndef BINSPLITTER_H
#define BINSPLITTER_H

#include <QByteArray>
#include <QString>
#include <QVector>

// ota-protocol(firmware-esp32와 공유하는 순수 C 헤더, 현재는 gateway-ota와
// 같은 상위 폴더에 형제 폴더로 clone해서 씀 — 자세한 경로 규칙은 CMakeLists.txt
// 참고, 나중에 git submodule로 바뀌어도 형제 폴더 위치는 그대로 유지될 예정)의
// 패킷 struct/인코딩 함수. C 헤더라 extern "C"로 감싸서 name mangling 문제 없이 사용.
extern "C" {
#include "ota_protocol.h"
}

// 분할된 청크 하나.
// header - 진행률 표시 등에 쓰는 메타데이터(seq/totalChunks/payloadLength 등)
// packet - ota_protocol_encode_data()가 만든, 그대로 전송하면 되는 최종 바이트열
//          (9byte 헤더 + payload, CRC16 포함). ITransport::send()에 그대로 넘기면 됨.
struct OtaChunk
{
    ota_packet_header_t header{};
    QByteArray packet;
};

// BIN 파일을 ota-protocol 규격에 맞춰 청크로 나누는 순수 로직 클래스.
// QWidget/QMainWindow에 의존하지 않아서 화면 없이(QTest 등으로) 단독 테스트 가능.
class BinSplitter
{
public:
    // filePath의 파일을 chunkSize(byte, 기본값 OTA_MAX_PAYLOAD_SIZE=55) 단위로 잘라
    // OtaChunk 목록을 반환합니다. chunkSize가 OTA_MAX_PAYLOAD_SIZE를 넘으면 실패
    // (CC1101 FIFO 64byte 제약 — ota-protocol 레포 README 참고).
    // 마지막 청크는 남는 만큼만 담기고 별도 패딩을 하지 않습니다 — payload_length
    // 필드가 실제 길이를 그대로 전달하기 때문에 패딩이 필요 없어졌습니다.
    static QVector<OtaChunk> split(
        const QString &filePath,
        int chunkSize = static_cast<int>(OTA_MAX_PAYLOAD_SIZE),
        QString *errorMessage = nullptr);
};

#endif // BINSPLITTER_H
