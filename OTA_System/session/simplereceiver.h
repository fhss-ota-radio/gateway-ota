#ifndef SIMPLERECEIVER_H
#define SIMPLERECEIVER_H

#include <cstdint>
#include <vector>

class ITransport;

// [의도적으로 Qt 의존성 없음] simplesender.h와 같은 이유.

// 받은 패킷이 어떤 종류인지 (ota_protocol.h의 ota_packet_type_t와 대응).
// 타입을 못 읽었거나(길이 부족) decode에 실패했으면(CRC 불일치 등) Unknown.
enum class ReceivedPacketKind {
    Unknown = 0,
    Start,
    Data,
    End,
    Ack,
    Nack,
    Discover,
    DiscoverAck,
};

// 패킷 하나를 받았을 때 담기는 정보. decode까지 이미 끝낸 상태라
// 호출부는 kind만 보고 바로 로그를 찍으면 됩니다.
// kind별로 의미 있는 필드만 채워지고 나머지는 0입니다 — 예를 들어
// Data면 sequence/payloadLength만 의미 있고 targetDeviceId 등은 0.
struct ReceivedPacket
{
    ReceivedPacketKind kind = ReceivedPacketKind::Unknown;
    std::vector<uint8_t> raw;   // 원본 바이트 (Unknown이어도 항상 채워짐 — hex dump용)

    uint32_t sessionId = 0;          // Start/Data/End/Ack/Nack
    uint32_t sequence = 0;           // Data/Ack/Nack
    uint32_t targetDeviceId = 0;     // Start
    uint32_t imageSize = 0;          // Start/End
    uint32_t totalChunks = 0;        // Start/End
    uint8_t  payloadLength = 0;      // Data
    uint8_t  resultCode = 0;         // Ack/Nack
    uint32_t deviceId = 0;           // DiscoverAck
    uint8_t  imageSha256[32] = {};   // Start — 송신측이 OtaSession::start()에서
                                      // 계산해 보낸 값 그대로. 수신측이 재조립
                                      // 완료 후 자체적으로 계산한 해시와 비교하면
                                      // (ESP32의 ota_writer_finish()가 하는 것과
                                      // 같은 검증을) sha256sum을 손으로 안 돌려도
                                      // 자동으로 확인할 수 있다.
};

// transport.recv()를 논블로킹(non-blocking, 데이터가 없어도 기다리지 않고
// 바로 리턴)으로 한 번 확인합니다. 아직 도착한 게 없으면
// kind==Unknown && raw가 빈 상태로 반환됩니다(에러 아님 — 호출부가 반복문
// 안에서 계속 불러주는 폴링 방식 전제).
ReceivedPacket tryReceiveOnce(ITransport &transport);

// 받은 패킷 하나에 대해 OTA_ACK를 즉시 만들어서 돌려보냅니다("반사적으로
// 응답하기"만 함 — 재전송 판단, 대기, 타임아웃 같은 건 전혀 없습니다. 그런
// 세션 레벨 로직은 OtaSession(FSM) 몫이고, 여기선 그냥 "이 패킷 받았다"는
// 응답 패킷 하나를 encode+send만 합니다).
//
// packet.kind가 Start/Data/End일 때만 동작합니다 — ACK/NACK/DISCOVER류는
// ota_protocol.h 설계상 애초에 응답 대상이 아니라서, 그 외 kind로 호출하면
// 아무것도 안 하고 false를 반환합니다.
//
// resultCode: ota_result_t 값(uint8_t로 받음 — 이 헤더가 ota_protocol.h
// 타입에 직접 의존하지 않도록 하기 위함). 기본값 0은 OTA_RESULT_OK와 같음.
bool sendAckFor(ITransport &transport, const ReceivedPacket &packet,
                 uint8_t resultCode = 0 /* OTA_RESULT_OK */);

// OTA_DATA 패킷의 헤더(session_id, sequence)만 다시 읽는다. 정상 decode
// 경로(tryReceiveOnce)와 달리 CRC 검증을 하지 않으므로, CRC가 깨진 DATA도
// "어떤 세션의 몇 번 청크였는지"는 알아낼 수 있다 — CRC 오류 NACK을 보낼 때
// 이 정보가 필요하다(안 그러면 그냥 버리는 것과 재전송 유도를 구분할 수 없음).
//
// raw가 OTA_DATA 타입이 아니거나 헤더 길이(OTA_DATA_HEADER_SIZE)보다 짧으면
// false — 그 경우 sessionId/sequence는 건드리지 않는다.
bool peekDataHeaderForNack(const std::vector<uint8_t> &raw, uint32_t *sessionId,
                            uint32_t *sequence);

#endif // SIMPLERECEIVER_H
