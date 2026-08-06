#include "binsplitter.h"

#include <QFile>

#include <cstring>

QVector<OtaChunk> BinSplitter::split(const QString &filePath, int chunkSize, QString *errorMessage)
{
    QVector<OtaChunk> chunks;

    if (chunkSize <= 0 || chunkSize > static_cast<int>(OTA_MAX_PAYLOAD_SIZE)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("청크 크기는 1~%1byte여야 합니다 (ota-protocol 최대 payload)")
                                .arg(OTA_MAX_PAYLOAD_SIZE);
        }
        return chunks;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("파일을 열 수 없습니다: %1").arg(filePath);
        return chunks;
    }

    const qint64 fileSize = file.size();
    if (fileSize <= 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("파일이 비어 있습니다: %1").arg(filePath);
        return chunks;
    }

    // 올림 나눗셈: 마지막 청크가 chunkSize보다 작아도 청크 1개로 셈
    const int totalChunks = static_cast<int>((fileSize + chunkSize - 1) / chunkSize);
    chunks.reserve(totalChunks);

    for (int seq = 0; seq < totalChunks; ++seq) {
        const QByteArray raw = file.read(chunkSize);
        const int actualLength = raw.size();

        if (actualLength <= 0) {
            if (errorMessage)
                *errorMessage = QStringLiteral("청크 %1 읽기 실패").arg(seq);
            chunks.clear();
            return chunks;
        }

        // ota_protocol_encode_data가 헤더(9byte) + payload(actualLength)를 그대로
        // 직렬화해준다. 마지막 청크가 chunkSize보다 짧아도 payload_length 필드에
        // 실제 길이가 남기 때문에 0x00 패딩이 따로 필요 없다.
        uint8_t packetBuffer[OTA_MAX_PACKET_SIZE];
        const size_t written = ota_protocol_encode_data(
            packetBuffer, sizeof(packetBuffer),
            static_cast<uint16_t>(seq), static_cast<uint16_t>(totalChunks),
            reinterpret_cast<const uint8_t *>(raw.constData()),
            static_cast<size_t>(actualLength));

        if (written == 0) {
            if (errorMessage)
                *errorMessage = QStringLiteral("청크 %1 인코딩 실패 (ota_protocol_encode_data)").arg(seq);
            chunks.clear();
            return chunks;
        }

        OtaChunk chunk;
        std::memcpy(&chunk.header, packetBuffer, OTA_PACKET_HEADER_SIZE);
        chunk.packet = QByteArray(reinterpret_cast<const char *>(packetBuffer), static_cast<int>(written));
        chunks.append(chunk);
    }

    return chunks;
}
