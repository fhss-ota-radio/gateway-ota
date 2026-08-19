/*
 * session/simplereceiver.h(tryReceiveOnce/sendAckFor/peekDataHeaderForNack)
 * 유닛테스트. tst_otasession.cpp와 같은 assert 기반 스타일, Qt 의존성 없음.
 *
 * 이 테스트가 존재하는 이유(2026-08-19): sendAckFor()가 resultCode와
 * 무관하게 항상 OTA_PKT_ACK로만 인코딩하던 버그를 발견해서 고쳤는데,
 * 그동안 이 함수 자체를 검증하는 유닛테스트가 하나도 없었다 —
 * tst_otasession.cpp는 OtaSession(송신측)이 "이미 만들어진" NACK 패킷을
 * 어떻게 처리하는지만 FakeTransport로 검증했지, 수신측이 실제로 NACK
 * 패킷을 만들어 보내는지는 아무도 확인한 적이 없었다. 이 파일이 그 공백을
 * 메운다.
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I.. -I../core -I../transport -I../session -I../../../ota-protocol/include \
 *       tst_simplereceiver.cpp ../session/simplereceiver.cpp -o tst_simplereceiver
 *   ./tst_simplereceiver
 */
#include "simplereceiver.h"

#include "itransport.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

extern "C" {
#include "ota_protocol.h"
}

namespace {

// tst_otasession.cpp의 FakeTransport와 같은 역할 — send()만 기록하면 되므로
// recv()는 안 씀.
class RecordingTransport : public ITransport
{
public:
    bool open() override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool send(const std::vector<uint8_t> &data) override
    {
        lastSent = data;
        return true;
    }
    std::vector<uint8_t> recv() override { return {}; }

    std::vector<uint8_t> lastSent;
};

// resultCode=OTA_RESULT_OK로 sendAckFor()를 부르면 와이어에 실제로
// OTA_PKT_ACK 타입이 나가는지 확인.
void sendAckForEncodesAckTypeWhenResultOk()
{
    RecordingTransport transport;
    ReceivedPacket packet;
    packet.kind = ReceivedPacketKind::Data;
    packet.sessionId = 0xAABBCCDDu;
    packet.sequence = 7;

    const bool sent = sendAckFor(transport, packet, OTA_RESULT_OK);
    assert(sent);
    assert(!transport.lastSent.empty());
    assert(transport.lastSent[0] == static_cast<uint8_t>(OTA_PKT_ACK));

    ota_packet_type_t type;
    ota_ack_fields_t fields;
    assert(ota_protocol_decode_ack(transport.lastSent.data(), transport.lastSent.size(),
                                    &type, &fields));
    assert(type == OTA_PKT_ACK);
    assert(fields.session_id == 0xAABBCCDDu);
    assert(fields.sequence == 7u);
    assert(fields.result_code == static_cast<uint8_t>(OTA_RESULT_OK));
    assert(fields.acknowledged_type == static_cast<uint8_t>(OTA_PKT_DATA));

    std::cout << "[OK] sendAckForEncodesAckTypeWhenResultOk\n";
}

// [2026-08-19 발견한 버그의 회귀 테스트] resultCode가 OK가 아니면 와이어에
// 실제로 OTA_PKT_NACK 타입이 나가야 함 — 예전 코드는 이 경우에도
// OTA_PKT_ACK만 보내서, "resultCode만 다른 ACK"였지 진짜 NACK이 아니었다.
void sendAckForEncodesNackTypeWhenResultNotOk()
{
    RecordingTransport transport;
    ReceivedPacket packet;
    packet.kind = ReceivedPacketKind::Data;
    packet.sessionId = 0x11223344u;
    packet.sequence = 3;

    const bool sent = sendAckFor(transport, packet, OTA_RESULT_INVALID_CRC);
    assert(sent);
    assert(!transport.lastSent.empty());
    assert(transport.lastSent[0] == static_cast<uint8_t>(OTA_PKT_NACK)); // 핵심 검증

    ota_packet_type_t type;
    ota_ack_fields_t fields;
    assert(ota_protocol_decode_ack(transport.lastSent.data(), transport.lastSent.size(),
                                    &type, &fields));
    assert(type == OTA_PKT_NACK);
    assert(fields.session_id == 0x11223344u);
    assert(fields.sequence == 3u);
    assert(fields.result_code == static_cast<uint8_t>(OTA_RESULT_INVALID_CRC));

    std::cout << "[OK] sendAckForEncodesNackTypeWhenResultNotOk\n";
}

// START/END에 대한 NACK도 같은 규칙(OTA_CONTROL_SEQUENCE)을 지키는지.
void sendAckForNackForStartUsesControlSequence()
{
    RecordingTransport transport;
    ReceivedPacket packet;
    packet.kind = ReceivedPacketKind::Start;
    packet.sessionId = 0x55u;

    assert(sendAckFor(transport, packet, OTA_RESULT_INVALID_SESSION));
    assert(transport.lastSent[0] == static_cast<uint8_t>(OTA_PKT_NACK));

    ota_packet_type_t type;
    ota_ack_fields_t fields;
    assert(ota_protocol_decode_ack(transport.lastSent.data(), transport.lastSent.size(),
                                    &type, &fields));
    assert(fields.sequence == OTA_CONTROL_SEQUENCE);
    assert(fields.acknowledged_type == static_cast<uint8_t>(OTA_PKT_START));

    std::cout << "[OK] sendAckForNackForStartUsesControlSequence\n";
}

// peekDataHeaderForNack(): CRC가 일부러 깨진 DATA 패킷이어도 session_id/
// sequence는 정확히 읽어내는지 확인 — 이게 안 되면 CRC 오류 NACK을 보낼 때
// "무슨 청크였는지"를 알 방법이 없다.
void peekDataHeaderForNackRecoversHeaderDespiteBadCrc()
{
    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t packet[OTA_DATA_HEADER_SIZE + 4];
    const size_t written =
        ota_protocol_encode_data(packet, sizeof(packet), 0x99887766u, 42, payload, 4);
    assert(written == OTA_DATA_HEADER_SIZE + 4);

    // CRC16 필드(offset 10~11)를 일부러 깨뜨림 — 정상 decode는 이제 실패해야 함.
    packet[10] ^= 0xFFu;
    packet[11] ^= 0xFFu;

    ota_data_header_fields_t header;
    const uint8_t *payloadOut = nullptr;
    size_t payloadLenOut = 0;
    assert(!ota_protocol_decode_data(packet, sizeof(packet), &header, &payloadOut,
                                      &payloadLenOut)); // 정상 경로는 실패해야 정상

    std::vector<uint8_t> raw(packet, packet + sizeof(packet));
    uint32_t sessionId = 0, sequence = 0;
    assert(peekDataHeaderForNack(raw, &sessionId, &sequence)); // 그래도 헤더는 읽힘
    assert(sessionId == 0x99887766u);
    assert(sequence == 42u);

    std::cout << "[OK] peekDataHeaderForNackRecoversHeaderDespiteBadCrc\n";
}

// DATA가 아닌 패킷이나 너무 짧은 버퍼는 false.
void peekDataHeaderForNackRejectsNonDataOrTooShort()
{
    std::vector<uint8_t> tooShort = {static_cast<uint8_t>(OTA_PKT_DATA), 1, 2, 3};
    uint32_t sessionId = 0, sequence = 0;
    assert(!peekDataHeaderForNack(tooShort, &sessionId, &sequence));

    uint8_t startPacket[OTA_START_PACKET_SIZE];
    ota_start_fields_t startFields{};
    startFields.session_id = 1;
    ota_protocol_encode_start(startPacket, sizeof(startPacket), &startFields);
    std::vector<uint8_t> notData(startPacket, startPacket + sizeof(startPacket));
    assert(!peekDataHeaderForNack(notData, &sessionId, &sequence));

    std::cout << "[OK] peekDataHeaderForNackRejectsNonDataOrTooShort\n";
}

} // namespace

int main()
{
    sendAckForEncodesAckTypeWhenResultOk();
    sendAckForEncodesNackTypeWhenResultNotOk();
    sendAckForNackForStartUsesControlSequence();
    peekDataHeaderForNackRecoversHeaderDespiteBadCrc();
    peekDataHeaderForNackRejectsNonDataOrTooShort();
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
