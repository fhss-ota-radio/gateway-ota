/*
 * BinSplitter + 공유 ota_protocol.h를 검증하는 테스트.
 * Qt에 전혀 의존하지 않는 순수 C++ 테스트입니다 (ota-protocol/test_ota_protocol.c와
 * 같은 assert 기반 스타일). g++/clang++만으로 직접 빌드/실행할 수 있습니다:
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I.. -I../core -I../../../ota-protocol/include \
 *       tst_binsplitter.cpp ../core/binsplitter.cpp -o tst_binsplitter
 *   ./tst_binsplitter
 *
 * CMake로 빌드하면(ota_core_tests 타깃) 위 include 경로는 CMakeLists.txt가 대신 잡아줍니다.
 */
#include "binsplitter.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

// 테스트 전용 임의의 session_id. 값 자체엔 의미가 없고, split()이 넘겨받은
// 값을 그대로 헤더/패킷에 싣는지만 확인하면 됨 (실제 세션 생성은 OtaSession 몫).
constexpr uint32_t kTestSessionId = 0x11223344u;

// QTemporaryFile 대신: /tmp에 테스트용 임시 파일을 만들고 경로를 돌려준다.
// 테스트 함수가 끝날 때 std::remove()로 직접 지운다 (Qt처럼 자동 삭제는 안 됨).
std::string writeTempFile(const std::string &name, const std::vector<uint8_t> &content)
{
    const std::string path = "/tmp/" + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    assert(f != nullptr);
    std::fwrite(content.data(), 1, content.size(), f);
    std::fclose(f);
    return path;
}

std::vector<uint8_t> repeat(uint8_t value, size_t count)
{
    return std::vector<uint8_t>(count, value);
}

void splitsExactMultipleFile()
{
    const int chunkSize = 20;
    const std::string path = writeTempFile("bs_exact.bin", repeat('A', static_cast<size_t>(chunkSize) * 2)); // 정확히 2개 청크

    std::string error;
    const auto chunks = BinSplitter::split(path, kTestSessionId, chunkSize, &error);

    assert(error.empty());
    assert(chunks.size() == 2);
    assert(chunks[0].header.session_id == kTestSessionId);
    assert(chunks[0].header.sequence == 0);
    assert(chunks[1].header.sequence == 1);
    // v0.2: total_chunks는 DATA 헤더가 아니라 OTA_START에만 실림 —
    // 여기선 chunks.size()가 곧 총 개수라 별도로 확인할 필요 없음.
    assert(int(chunks[0].header.payload_length) == chunkSize);
    assert(chunks[0].packet.size() == OTA_DATA_HEADER_SIZE + static_cast<size_t>(chunkSize));

    std::remove(path.c_str());
    std::cout << "[OK] splitsExactMultipleFile\n";
}

void lastChunkIsShorterNotPadded()
{
    const int chunkSize = 20;
    const std::string path = writeTempFile("bs_short.bin", repeat('B', static_cast<size_t>(chunkSize) + 7)); // 20 + 7 -> 마지막 청크는 7byte

    std::string error;
    const auto chunks = BinSplitter::split(path, kTestSessionId, chunkSize, &error);

    assert(error.empty());
    assert(chunks.size() == 2);
    // ota_protocol의 payload_length 필드가 실제 길이를 담기 때문에 0x00 패딩이 필요 없어짐
    assert(int(chunks.back().header.payload_length) == 7);
    assert(chunks.back().packet.size() == OTA_DATA_HEADER_SIZE + 7);

    std::remove(path.c_str());
    std::cout << "[OK] lastChunkIsShorterNotPadded\n";
}

void corruptedPacketFailsCrcOnDecode()
{
    const std::string path = writeTempFile("bs_corrupt.bin", repeat('C', 10));

    std::string error;
    auto chunks = BinSplitter::split(path, kTestSessionId, 10, &error);
    assert(error.empty());
    assert(chunks.size() == 1);

    // payload 한 byte를 일부러 깨뜨림
    auto corrupted = chunks[0].packet;
    corrupted[OTA_DATA_HEADER_SIZE] = static_cast<uint8_t>(corrupted[OTA_DATA_HEADER_SIZE] ^ 0xFF);

    ota_data_header_fields_t header;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;
    const bool ok = ota_protocol_decode_data(corrupted.data(), corrupted.size(), &header, &payload, &payloadLen);

    assert(!ok); // CRC16 불일치라 디코딩이 실패해야 정상 (-> 상위 로직이 NACK 보내야 함)

    std::remove(path.c_str());
    std::cout << "[OK] corruptedPacketFailsCrcOnDecode\n";
}

void reportsErrorForMissingFile()
{
    std::string error;
    const auto chunks = BinSplitter::split("/no/such/file.bin", kTestSessionId, 20, &error);
    assert(chunks.empty());
    assert(!error.empty());
    std::cout << "[OK] reportsErrorForMissingFile\n";
}

void reportsErrorForChunkSizeOutOfRange()
{
    const std::string path = writeTempFile("bs_range.bin", repeat('D', 10));

    std::string error;
    assert(BinSplitter::split(path, kTestSessionId, 0, &error).empty());
    assert(!error.empty());

    error.clear();
    // ota-protocol의 OTA_MAX_PAYLOAD_SIZE(48byte, RF 상위 계층 60byte 제약 기준)를 넘으면 거부
    assert(BinSplitter::split(path, kTestSessionId, int(OTA_MAX_PAYLOAD_SIZE) + 1, &error).empty());
    assert(!error.empty());

    std::remove(path.c_str());
    std::cout << "[OK] reportsErrorForChunkSizeOutOfRange\n";
}

void defaultChunkSizeMatchesProtocolMax()
{
    const std::string path = writeTempFile("bs_default.bin", repeat('E', size_t(OTA_MAX_PAYLOAD_SIZE) + 10));

    // chunkSize를 생략하면 binsplitter.h의 기본값(OTA_MAX_PAYLOAD_SIZE)이 쓰여야 함
    const auto chunks = BinSplitter::split(path, kTestSessionId);

    assert(!chunks.empty());
    assert(int(chunks[0].header.payload_length) == int(OTA_MAX_PAYLOAD_SIZE));

    std::remove(path.c_str());
    std::cout << "[OK] defaultChunkSizeMatchesProtocolMax\n";
}

// 쪼갠 뒤 다시 합치면 원본과 완전히 같은 바이트인지 확인하는 왕복(round-trip)
// 테스트. 위 테스트들은 split() 한쪽만 검증했는데, 이건 decode까지 왕복시켜서
// "실제로 파일이 손실 없이 복원되는지"를 끝까지 확인함. 마지막 청크가 짧게
// 남도록 일부러 청크 크기의 배수가 아닌 파일 크기를 씀.
void roundTripSplitAndReassembleMatchesOriginalFile()
{
    const int chunkSize = 16;
    std::vector<uint8_t> original(size_t(chunkSize) * 3 + 5); // 마지막 청크가 5byte만 남게
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = static_cast<uint8_t>(i % 251); // 같은 값 반복이 아니라 순서 섞임까지 잡아내는 패턴

    const std::string path = writeTempFile("bs_roundtrip.bin", original);

    std::string error;
    const auto chunks = BinSplitter::split(path, kTestSessionId, chunkSize, &error);
    assert(error.empty());
    assert(chunks.size() == 4); // 16*3 + 5 -> 청크 4개(16,16,16,5)

    // 청크를 sequence 순서대로 디코딩해서 payload를 그대로 이어붙임
    std::vector<uint8_t> reassembled;
    reassembled.reserve(original.size());
    for (const auto &chunk : chunks) {
        ota_data_header_fields_t header;
        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        const bool ok = ota_protocol_decode_data(chunk.packet.data(), chunk.packet.size(),
                                                   &header, &payload, &payloadLen);
        assert(ok); // 방금 만든 정상 패킷이라 CRC도 맞아야 함
        assert(header.session_id == kTestSessionId);
        reassembled.insert(reassembled.end(), payload, payload + payloadLen);
    }

    assert(reassembled == original); // 원본과 바이트 단위로 완전히 같아야 함

    std::remove(path.c_str());
    std::cout << "[OK] roundTripSplitAndReassembleMatchesOriginalFile (" << original.size() << " byte)\n";
}

} // namespace

int main()
{
    splitsExactMultipleFile();
    lastChunkIsShorterNotPadded();
    corruptedPacketFailsCrcOnDecode();
    reportsErrorForMissingFile();
    reportsErrorForChunkSizeOutOfRange();
    defaultChunkSizeMatchesProtocolMax();
    roundTripSplitAndReassembleMatchesOriginalFile();
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
