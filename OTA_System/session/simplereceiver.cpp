#include "simplereceiver.h"

#include "itransport.h"

#include <cstring>

extern "C" {
#include "ota_protocol.h"
}

ReceivedPacket tryReceiveOnce(ITransport &transport)
{
    ReceivedPacket result;

    const auto raw = transport.recv();
    if (raw.empty())
        return result; // 아직 도착한 거 없음 (에러 아님)

    result.raw = raw;

    ota_packet_type_t type;
    if (!ota_protocol_peek_type(raw.data(), raw.size(), &type))
        return result; // 타입 byte도 못 읽음 — kind는 Unknown, raw만 채워서 반환

    switch (type) {
    case OTA_PKT_START: {
        ota_start_fields_t fields;
        if (!ota_protocol_decode_start(raw.data(), raw.size(), &fields))
            return result;
        result.kind = ReceivedPacketKind::Start;
        result.sessionId = fields.session_id;
        result.targetDeviceId = fields.target_device_id;
        result.imageSize = fields.image_size;
        result.totalChunks = fields.total_chunks;
        static_assert(sizeof(result.imageSha256) == sizeof(fields.image_sha256),
                      "ReceivedPacket.imageSha256 크기가 ota_start_fields_t와 다릅니다");
        std::memcpy(result.imageSha256, fields.image_sha256, sizeof(result.imageSha256));
        break;
    }
    case OTA_PKT_DATA: {
        ota_data_header_fields_t header;
        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        if (!ota_protocol_decode_data(raw.data(), raw.size(), &header, &payload, &payloadLen))
            return result;
        result.kind = ReceivedPacketKind::Data;
        result.sessionId = header.session_id;
        result.sequence = header.sequence;
        result.payloadLength = header.payload_length;
        break;
    }
    case OTA_PKT_END: {
        ota_end_fields_t fields;
        if (!ota_protocol_decode_end(raw.data(), raw.size(), &fields))
            return result;
        result.kind = ReceivedPacketKind::End;
        result.sessionId = fields.session_id;
        result.imageSize = fields.image_size;
        result.totalChunks = fields.total_chunks;
        break;
    }
    case OTA_PKT_ACK:
    case OTA_PKT_NACK: {
        ota_packet_type_t ackType;
        ota_ack_fields_t fields;
        if (!ota_protocol_decode_ack(raw.data(), raw.size(), &ackType, &fields))
            return result;
        result.kind = (type == OTA_PKT_ACK) ? ReceivedPacketKind::Ack : ReceivedPacketKind::Nack;
        result.sessionId = fields.session_id;
        result.sequence = fields.sequence;
        result.resultCode = fields.result_code;
        break;
    }
    case OTA_PKT_DISCOVER:
        if (!ota_protocol_decode_discover(raw.data(), raw.size()))
            return result;
        result.kind = ReceivedPacketKind::Discover;
        break;
    case OTA_PKT_DISCOVER_ACK: {
        ota_discover_ack_fields_t fields;
        if (!ota_protocol_decode_discover_ack(raw.data(), raw.size(), &fields))
            return result;
        result.kind = ReceivedPacketKind::DiscoverAck;
        result.deviceId = fields.device_id;
        break;
    }
    default:
        break; // 알 수 없는 type byte — Unknown 유지, raw는 이미 채워짐
    }

    return result;
}

bool sendAckFor(ITransport &transport, const ReceivedPacket &packet, uint8_t resultCode)
{
    ota_packet_type_t ackedType;
    uint32_t sequence;

    // START/END는 특정 청크가 아니라 제어 패킷 자체에 대한 응답이라
    // sequence 자리에 OTA_CONTROL_SEQUENCE(전부 1)를 씀 — ota_protocol.h의
    // 규칙 그대로.
    switch (packet.kind) {
    case ReceivedPacketKind::Start:
        ackedType = OTA_PKT_START;
        sequence = OTA_CONTROL_SEQUENCE;
        break;
    case ReceivedPacketKind::Data:
        ackedType = OTA_PKT_DATA;
        sequence = packet.sequence;
        break;
    case ReceivedPacketKind::End:
        ackedType = OTA_PKT_END;
        sequence = OTA_CONTROL_SEQUENCE;
        break;
    default:
        return false; // ACK/NACK/DISCOVER류는 응답 대상이 아님
    }

    ota_ack_fields_t fields{};
    fields.session_id = packet.sessionId;
    fields.acknowledged_type = static_cast<uint8_t>(ackedType);
    fields.sequence = sequence;
    fields.result_code = resultCode;

    // [버그 수정 2026-08-19] resultCode가 OTA_RESULT_OK가 아니면 실제로
    // OTA_NACK 타입으로 보낸다. 수신측(OtaSession::pollAndApplyAckOrNack())은
    // tryReceiveOnce()가 매긴 kind(Ack/Nack — 와이어의 type byte로만 결정됨,
    // ota_protocol.h 참고)를 보고 재전송 여부를 판단하는데, 지금까지 이
    // 함수는 resultCode 값과 상관없이 무조건 OTA_PKT_ACK로만 인코딩하고
    // 있었다. 우연히 sendStartPacket 쪽 ok 판정이 "kind==Ack && resultCode==
    // OK" 둘 다 확인해서 동작 자체는 어쩌다 맞았지만(resultCode!=OK면
    // ok=false로 떨어져 재전송은 됨), 와이어에는 진짜 NACK 타입 패킷이
    // 한 번도 안 나간 상태였다 — "NACK 경로는 시뮬레이션으로만 검증됨"
    // (docs/roadmap.md)의 근본 원인이 바로 이거였다.
    const ota_packet_type_t wireType = (resultCode == static_cast<uint8_t>(OTA_RESULT_OK))
        ? OTA_PKT_ACK
        : OTA_PKT_NACK;

    uint8_t buffer[OTA_ACK_PACKET_SIZE];
    const size_t written =
        ota_protocol_encode_ack(buffer, sizeof(buffer), wireType, &fields);
    if (written == 0)
        return false;

    return transport.send(std::vector<uint8_t>(buffer, buffer + written));
}

bool peekDataHeaderForNack(const std::vector<uint8_t> &raw, uint32_t *sessionId,
                            uint32_t *sequence)
{
    // ota_protocol_decode_data()는 CRC 불일치면 통째로 실패(false)라서
    // session_id/sequence조차 꺼낼 수 없다 — 그런데 CRC 검사는 payload에만
    // 걸리고(ota_protocol.h 208행 주석 참고) 헤더 자체는 그 대상이 아니므로,
    // 헤더 바이트는 CRC 결과와 무관하게 그대로 읽어도 안전하다. "이 청크가
    // 깨졌다"는 NACK을 보낼 때 "어떤 청크였는지"를 알려면 이게 필요하다 —
    // 안 그러면 그냥 조용히 버리는 수밖에 없다(송신측은 타임아웃까지
    // 기다려야 재전송함).
    if (raw.size() < OTA_DATA_HEADER_SIZE)
        return false;
    if (raw[0] != static_cast<uint8_t>(OTA_PKT_DATA))
        return false;

    if (sessionId) *sessionId = ota_read_u32_le(&raw[1]);
    if (sequence) *sequence = ota_read_u32_le(&raw[5]);
    return true;
}
