/*
 * deriveSessionHopSeed()/scanSecretSeedFolder()(core/hopseed.h) 검증.
 * Qt에 전혀 의존하지 않는 순수 C++ 테스트입니다 (tst_binsplitter.cpp와 같은
 * assert 기반 스타일). g++/clang++만으로 직접 빌드/실행할 수 있습니다:
 *
 *   g++ -std=c++17 -Wall -Wextra \
 *       -I../core \
 *       tst_hopseed.cpp ../core/hopseed.cpp ../core/sha256.cpp -o tst_hopseed
 *   ./tst_hopseed
 *
 * CMake로 빌드하면(ota_hopseed_tests 타깃) 위 include 경로는 CMakeLists.txt가
 * 대신 잡아줍니다.
 *
 * 기대값은 firmware-esp32 쪽과 완전히 같은 계산인지 확인하려고 파이썬
 * hmac/hashlib(표준 라이브러리, 이 알고리즘의 레퍼런스 구현)로 별도 계산해
 * 대조한 값이다:
 *   secret = bytes([0x4B,0x43,0x43,0x49])  # "KCCI", firmware의
 *            s_secret_seed와 동일
 *   msg = public_seed.to_bytes(4, 'big')
 *   hmac.new(secret, msg, hashlib.sha256).digest()[0:4]
 */
#include "hopseed.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

constexpr SecretSeed kKcciSecret = {0x4B, 0x43, 0x43, 0x49};

void testKnownVectors()
{
    // 세 값 다 파이썬 hmac/hashlib로 미리 계산해서 대조함 — 소스는 파일 상단
    // 주석 참고. secret_seed가 firmware의 s_secret_seed와 같으므로, 이 값들이
    // 맞으면 실기기(ESP32)와 게이트웨이가 항상 같은 hop_seed를 계산한다는
    // 뜻이다.
    assert(deriveSessionHopSeed(kKcciSecret, 0x00000000u) == 0x36ef4bebu);
    assert(deriveSessionHopSeed(kKcciSecret, 0x12345678u) == 0xa22e6a9eu);
    assert(deriveSessionHopSeed(kKcciSecret, 0xFFFFFFFFu) == 0x5c5568ddu);
    std::printf("testKnownVectors OK\n");
}

void testDifferentSecretsDiverge()
{
    // secret_seed가 다르면 같은 public_seed에서도 다른 hop_seed가 나와야
    // 한다 — 아니면 HMAC 키 조합이 사실상 무시되고 있다는 뜻이라 회귀 검증.
    const SecretSeed otherSecret = {0x01, 0x02, 0x03, 0x04};
    assert(deriveSessionHopSeed(kKcciSecret, 0x12345678u) !=
           deriveSessionHopSeed(otherSecret, 0x12345678u));
    std::printf("testDifferentSecretsDiverge OK\n");
}

// /tmp에 테스트용 임시 파일을 쓰고 경로를 돌려준다 (tst_binsplitter.cpp의
// writeTempFile()과 같은 패턴).
std::string writeTempFile(const std::string &name, const std::string &content)
{
    const std::string path = "/tmp/" + name;
    std::ofstream file(path);
    assert(file);
    file << content;
    file.close();
    return path;
}

void testScanSecretSeedFolder()
{
    const std::string dir = "/tmp/tst_hopseed_folder";
    std::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());

    writeTempFile("tst_hopseed_folder/v1.0.0.txt", "4B434349\n");
    writeTempFile("tst_hopseed_folder/v1.1.0.txt", "0xAABBCCDD\n");
    writeTempFile("tst_hopseed_folder/broken.txt", "not-hex\n");
    writeTempFile("tst_hopseed_folder/notes.md", "4B434349\n"); // .txt 아니므로 무시돼야 함

    std::vector<std::string> skipped;
    const std::vector<SecretSeedEntry> entries = scanSecretSeedFolder(dir, &skipped);

    assert(entries.size() == 2); // v1.0.0, v1.1.0만 파싱됨 (broken.txt 제외, notes.md 확장자 제외)
    assert(entries[0].versionLabel == "v1.0.0");
    assert((entries[0].secretSeed == SecretSeed{0x4B, 0x43, 0x43, 0x49}));
    assert(entries[1].versionLabel == "v1.1.0");
    assert((entries[1].secretSeed == SecretSeed{0xAA, 0xBB, 0xCC, 0xDD}));

    assert(skipped.size() == 1);
    assert(skipped[0] == "broken.txt");

    std::system(("rm -rf " + dir).c_str());
    std::printf("testScanSecretSeedFolder OK\n");
}

} // namespace

int main()
{
    testKnownVectors();
    testDifferentSecretsDiverge();
    testScanSecretSeedFolder();
    std::printf("All hopseed tests passed.\n");
    return 0;
}
