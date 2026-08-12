#ifndef BINSPLITTER_H
#define BINSPLITTER_H

#include <cstdint>
#include <string>
#include <vector>

// ota-protocol(firmware-esp32와 공유하는 순수 C 헤더, 현재는 gateway-ota와
// 같은 상위 폴더에 형제 폴더로 clone해서 씀 — 자세한 경로 규칙은 CMakeLists.txt
// 참고, 나중에 git submodule로 바뀌어도 형제 폴더 위치는 그대로 유지될 예정)의
// 패킷 struct/인코딩 함수. C 헤더라 extern "C"로 감싸서 name mangling 문제 없이 사용.
extern "C" {
#include "ota_protocol.h"
}

// [의도적으로 Qt 의존성 없음] 파일 분할은 화면과 무관한 순수 로직이라 표준
// C++(std::string/std::vector)만 씁니다. Qt::Core조차 링크하지 않기 때문에
// g++/clang++만으로 컴파일·테스트할 수 있습니다 (ota-protocol과 같은 원칙).
// 화면(Qt) 쪽에서 이 결과를 쓰려면 QString/QByteArray로 변환하는 얇은 변환
// 코드가 otamanager.cpp(호출부)에 필요합니다 — 그 변환 코드는 "화면과 붙는
// 경계"에서만 필요한 것이라 여기 core/ 안에는 들어오지 않습니다.

// 분할된 청크 하나.
// header - 진행률 표시 등에 쓰는 메타데이터(session_id/sequence/payload_length/crc16).
//          2026-08-11 v0.2 갱신: total_chunks는 더 이상 DATA 헤더에 없습니다
//          (OTA_START에만 있음 — split() 호출부가 chunks.size()로 총 개수를
//          알고 있으니 그걸로 OTA_START를 만들면 됩니다).
// packet - ota_protocol_encode_data()가 만든, 그대로 전송하면 되는 최종 바이트열
//          (12byte 헤더 + payload, CRC16 포함). ITransport::send()에 그대로 넘기면 됨.
struct OtaChunk
{
    ota_data_header_fields_t header{};
    std::vector<uint8_t> packet;
};

// BIN 파일을 ota-protocol 규격에 맞춰 청크로 나누는 순수 로직 클래스.
// Qt에 전혀 의존하지 않아서 화면 없이(assert 기반 테스트 등으로) 단독 테스트 가능.
class BinSplitter
{
public:
    // filePath의 파일을 chunkSize(byte, 기본값 OTA_MAX_PAYLOAD_SIZE=48) 단위로 잘라
    // OtaChunk 목록을 반환합니다. chunkSize가 OTA_MAX_PAYLOAD_SIZE를 넘으면 실패
    // (RF 상위 계층이 보장하는 패킷 바디 최대 60byte 제약 — ota-protocol 레포 README 참고).
    // 마지막 청크는 남는 만큼만 담기고 별도 패딩을 하지 않습니다 — payload_length
    // 필드가 실제 길이를 그대로 전달하기 때문에 패딩이 필요 없어졌습니다.
    //
    // sessionId: 2026-08-11 v0.2에서 새로 필요해진 값(세션마다 랜덤하게 생성해서
    // OTA_START/DATA/END/ACK/NACK에 공통으로 싣는 식별자). BinSplitter가 세션을
    // 새로 만드는 건 아니라서 세션 레이어(OtaSession)가 생성한 값을 그대로
    // 넘겨받습니다 — 그래서 기본값 없이 항상 명시적으로 넘기게 했습니다.
    static std::vector<OtaChunk> split(
        const std::string &filePath,
        uint32_t sessionId,
        int chunkSize = static_cast<int>(OTA_MAX_PAYLOAD_SIZE),
        std::string *errorMessage = nullptr);
};

#endif // BINSPLITTER_H
