#include "binsplitter.h"

#include <cstring>
#include <fstream>

std::vector<OtaChunk> BinSplitter::split(const std::string &filePath, int chunkSize, std::string *errorMessage)
{
    std::vector<OtaChunk> chunks;

    if (chunkSize <= 0 || chunkSize > static_cast<int>(OTA_MAX_PAYLOAD_SIZE)) {
        if (errorMessage) {
            *errorMessage = "청크 크기는 1~" + std::to_string(OTA_MAX_PAYLOAD_SIZE)
                             + "byte여야 합니다 (ota-protocol 최대 payload)";
        }
        return chunks;
    }

    // std::ios::ate로 열어서 파일 크기를 바로 알아냄 (QFileInfo::size() 대신)
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        if (errorMessage)
            *errorMessage = "파일을 열 수 없습니다: " + filePath;
        return chunks;
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        if (errorMessage)
            *errorMessage = "파일이 비어 있습니다: " + filePath;
        return chunks;
    }
    file.seekg(0, std::ios::beg);

    // 올림 나눗셈: 마지막 청크가 chunkSize보다 작아도 청크 1개로 셈
    const int totalChunks = static_cast<int>((fileSize + chunkSize - 1) / chunkSize);
    chunks.reserve(static_cast<size_t>(totalChunks));

    std::vector<char> raw(static_cast<size_t>(chunkSize));

    for (int seq = 0; seq < totalChunks; ++seq) {
        file.read(raw.data(), chunkSize);
        const std::streamsize actualLength = file.gcount();

        if (actualLength <= 0) {
            if (errorMessage)
                *errorMessage = "청크 " + std::to_string(seq) + " 읽기 실패";
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
            reinterpret_cast<const uint8_t *>(raw.data()),
            static_cast<size_t>(actualLength));

        if (written == 0) {
            if (errorMessage)
                *errorMessage = "청크 " + std::to_string(seq) + " 인코딩 실패 (ota_protocol_encode_data)";
            chunks.clear();
            return chunks;
        }

        OtaChunk chunk;
        std::memcpy(&chunk.header, packetBuffer, OTA_PACKET_HEADER_SIZE);
        chunk.packet.assign(packetBuffer, packetBuffer + written);
        chunks.push_back(std::move(chunk));
    }

    return chunks;
}
