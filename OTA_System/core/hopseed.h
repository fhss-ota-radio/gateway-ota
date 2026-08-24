#ifndef HOPSEED_H
#define HOPSEED_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// firmware-esp32(components/fhss_audio_adapter/fhss_audio_adapter.c의
// derive_session_hop_seed())와 완전히 동일한 HMAC-SHA256 파생 로직.
// 게이트웨이도 같은 secret_seed를 쥐고 있어야 ESP32와 동일한 hop_seed를
// 계산해 같은 홉 패턴으로 따라갈 수 있다(otamanager.cpp가 이 값을 ESP32용
// FhssHopPolicy.seed와 라즈베리파이 커널 드라이버용 Cc1101FhssConfig.seed
// 양쪽에 그대로 넘김 — 게이트웨이도 실제로 호핑하는 당사자이기 때문).
using SecretSeed = std::array<uint8_t, 4>;

// HMAC(K,m) = SHA256((K' xor opad) || SHA256((K' xor ipad) || m)),
// K' = secret_seed를 64byte로 제로패딩한 것(RFC 2104), m = public_seed의
// big-endian 4byte. 결과 32byte 중 앞 4byte를 hop_seed로 쓴다 — firmware
// 쪽과 정확히 같은 축약 방식이어야 두 기기가 같은 값을 계산한다.
uint32_t deriveSessionHopSeed(const SecretSeed &secretSeed, uint32_t publicSeed);

struct SecretSeedEntry {
    std::string versionLabel; // 파일 이름(확장자 제외) — 드롭다운 표시용
    SecretSeed secretSeed;
};

// dirPath 안의 *.txt 파일들을 스캔한다. 파일 하나 = 첫 줄에 8자리 16진수
// (예: "4B434349", "0x" 접두사 허용, 공백 무시)만 담겨 있으면 그 4byte가
// 그대로 secret_seed가 된다. 형식이 안 맞거나 못 읽는 파일은 건너뛰고
// skippedFiles(NULL 아니면)에 파일명을 남긴다 — 호출부가 로그로 경고할 수
// 있도록. 반환값은 versionLabel 기준 오름차순 정렬.
std::vector<SecretSeedEntry> scanSecretSeedFolder(
    const std::string &dirPath,
    std::vector<std::string> *skippedFiles = nullptr
);

#endif // HOPSEED_H
