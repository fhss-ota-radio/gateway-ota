#include "hopseed.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

bool parseHexSecretSeed(const std::string &line, SecretSeed *out)
{
    std::string trimmed;
    trimmed.reserve(line.size());
    for (const char c : line) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            trimmed.push_back(c);
    }
    if (trimmed.size() >= 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
        trimmed = trimmed.substr(2);
    if (trimmed.size() != 8)
        return false;
    for (const char c : trimmed) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    for (size_t i = 0; i < 4; ++i)
        (*out)[i] = static_cast<uint8_t>(std::stoul(trimmed.substr(i * 2, 2), nullptr, 16));
    return true;
}

} // namespace

uint32_t deriveSessionHopSeed(const SecretSeed &secretSeed, uint32_t publicSeed)
{
    constexpr size_t kBlockSize = 64;
    constexpr size_t kDigestSize = 32;

    const uint8_t message[4] = {
        static_cast<uint8_t>((publicSeed >> 24) & 0xFFU),
        static_cast<uint8_t>((publicSeed >> 16) & 0xFFU),
        static_cast<uint8_t>((publicSeed >> 8) & 0xFFU),
        static_cast<uint8_t>(publicSeed & 0xFFU),
    };

    uint8_t ipad[kBlockSize];
    uint8_t opad[kBlockSize];
    std::memset(ipad, 0x36, sizeof(ipad));
    std::memset(opad, 0x5C, sizeof(opad));
    for (size_t i = 0; i < secretSeed.size(); ++i) {
        ipad[i] ^= secretSeed[i];
        opad[i] ^= secretSeed[i];
    }

    uint8_t innerDigest[kDigestSize];
    Sha256 inner;
    inner.update(ipad, sizeof(ipad));
    inner.update(message, sizeof(message));
    inner.finish(innerDigest);

    uint8_t outerDigest[kDigestSize];
    Sha256 outer;
    outer.update(opad, sizeof(opad));
    outer.update(innerDigest, sizeof(innerDigest));
    outer.finish(outerDigest);

    return (static_cast<uint32_t>(outerDigest[0]) << 24) |
           (static_cast<uint32_t>(outerDigest[1]) << 16) |
           (static_cast<uint32_t>(outerDigest[2]) << 8) |
           static_cast<uint32_t>(outerDigest[3]);
}

std::vector<SecretSeedEntry> scanSecretSeedFolder(
    const std::string &dirPath,
    std::vector<std::string> *skippedFiles)
{
    std::vector<SecretSeedEntry> result;
    std::error_code ec;
    if (!fs::is_directory(dirPath, ec))
        return result;

    for (const auto &entry : fs::directory_iterator(dirPath, ec)) {
        if (ec || !entry.is_regular_file())
            continue;
        const fs::path &path = entry.path();
        if (path.extension() != ".txt")
            continue;

        std::ifstream file(path);
        std::string firstLine;
        if (!file || !std::getline(file, firstLine)) {
            if (skippedFiles)
                skippedFiles->push_back(path.filename().string());
            continue;
        }

        SecretSeed secretSeed{};
        if (!parseHexSecretSeed(firstLine, &secretSeed)) {
            if (skippedFiles)
                skippedFiles->push_back(path.filename().string());
            continue;
        }

        result.push_back(SecretSeedEntry{path.stem().string(), secretSeed});
    }

    std::sort(result.begin(), result.end(), [](const SecretSeedEntry &a, const SecretSeedEntry &b) {
        return a.versionLabel < b.versionLabel;
    });
    return result;
}
