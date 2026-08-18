#include "simplereceiver.h"

#include "itransport.h"

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

    uint8_t buffer[OTA_ACK_PACKET_SIZE];
    const size_t written =
        ota_protocol_encode_ack(buffer, sizeof(buffer), OTA_PKT_ACK, &fields);
    if (written == 0)
        return false;

    return transport.send(std::vector<uint8_t>(buffer, buffer + written));
}
