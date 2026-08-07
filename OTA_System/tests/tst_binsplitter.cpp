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
    const auto chunks = BinSplitter::split(path, chunkSize, &error);

    assert(error.empty());
    assert(chunks.size() == 2);
    assert(chunks[0].header.seq == 0);
    assert(chunks[1].header.seq == 1);
    assert(chunks[0].header.total_chunks == 2);
    assert(int(chunks[0].header.payload_length) == chunkSize);
    assert(chunks[0].packet.size() == OTA_PACKET_HEADER_SIZE + static_cast<size_t>(chunkSize));

    std::remove(path.c_str());
    std::cout << "[OK] splitsExactMultipleFile\n";
}

void lastChunkIsShorterNotPadded()
{
    const int chunkSize = 20;
    const std::string path = writeTempFile("bs_short.bin", repeat('B', static_cast<size_t>(chunkSize) + 7)); // 20 + 7 -> 마지막 청크는 7byte

    std::string error;
    const auto chunks = BinSplitter::split(path, chunkSize, &error);

    assert(error.empty());
    assert(chunks.size() == 2);
    // ota_protocol의 payload_length 필드가 실제 길이를 담기 때문에 0x00 패딩이 필요 없어짐
    assert(int(chunks.back().header.payload_length) == 7);
    assert(chunks.back().packet.size() == OTA_PACKET_HEADER_SIZE + 7);

    std::remove(path.c_str());
    std::cout << "[OK] lastChunkIsShorterNotPadded\n";
}

void corruptedPacketFailsCrcOnDecode()
{
    const std::string path = writeTempFile("bs_corrupt.bin", repeat('C', 10));

    std::string error;
    auto chunks = BinSplitter::split(path, 10, &error);
    assert(error.empty());
    assert(chunks.size() == 1);

    // payload 한 byte를 일부러 깨뜨림
    auto corrupted = chunks[0].packet;
    corrupted[OTA_PACKET_HEADER_SIZE] = static_cast<uint8_t>(corrupted[OTA_PACKET_HEADER_SIZE] ^ 0xFF);

    ota_packet_header_t header;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;
    const bool ok = ota_protocol_decode(corrupted.data(), corrupted.size(), &header, &payload, &payloadLen);

    assert(!ok); // CRC16 불일치라 디코딩이 실패해야 정상 (-> 상위 로직이 NACK 보내야 함)

    std::remove(path.c_str());
    std::cout << "[OK] corruptedPacketFailsCrcOnDecode\n";
}

void reportsErrorForMissingFile()
{
    std::string error;
    const auto chunks = BinSplitter::split("/no/such/file.bin", 20, &error);
    assert(chunks.empty());
    assert(!error.empty());
    std::cout << "[OK] reportsErrorForMissingFile\n";
}

void reportsErrorForChunkSizeOutOfRange()
{
    const std::string path = writeTempFile("bs_range.bin", repeat('D', 10));

    std::string error;
    assert(BinSplitter::split(path, 0, &error).empty());
    assert(!error.empty());

    error.clear();
    // ota-protocol의 OTA_MAX_PAYLOAD_SIZE(55byte, CC1101 FIFO 64byte 제약)를 넘으면 거부
    assert(BinSplitter::split(path, int(OTA_MAX_PAYLOAD_SIZE) + 1, &error).empty());
    assert(!error.empty());

    std::remove(path.c_str());
    std::cout << "[OK] reportsErrorForChunkSizeOutOfRange\n";
}

void defaultChunkSizeMatchesProtocolMax()
{
    const std::string path = writeTempFile("bs_default.bin", repeat('E', size_t(OTA_MAX_PAYLOAD_SIZE) + 10));

    // chunkSize를 생략하면 binsplitter.h의 기본값(OTA_MAX_PAYLOAD_SIZE)이 쓰여야 함
    const auto chunks = BinSplitter::split(path);

    assert(!chunks.empty());
    assert(int(chunks[0].header.payload_length) == int(OTA_MAX_PAYLOAD_SIZE));

    std::remove(path.c_str());
    std::cout << "[OK] defaultChunkSizeMatchesProtocolMax\n";
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
    std::cout << "\n모든 테스트 통과\n";
    return 0;
}
