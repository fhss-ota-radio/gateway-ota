// Real-radio OTA flow: DISCOVER -> select one discovered ESP32 -> targeted
// START/DATA/END through OtaSession. This CLI never sends a broadcast START.

#include "cc1101transport.h"
#include "discovery.h"
#include "otasession.h"

extern "C" {
#include "ota_protocol.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool parsePositiveInt(const char *text, int *out)
{
    if (text == nullptr || *text == '\0')
        return false;
    try {
        size_t consumed = 0;
        const long value = std::stol(text, &consumed, 10);
        if (consumed != std::string(text).size() || value <= 0 || value > 3600000)
            return false;
        *out = static_cast<int>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool parseDeviceId(const std::string &text, uint32_t *out)
{
    if (text.empty())
        return false;
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 16);
        if (consumed != text.size() || value > OTA_DEVICE_ID_MAX)
            return false;
        *out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

std::string displayDeviceId(uint32_t deviceId)
{
    char output[16];
    std::snprintf(output, sizeof(output), "%02X-%02X-%02X",
                  static_cast<unsigned>((deviceId >> 16) & 0xFFU),
                  static_cast<unsigned>((deviceId >> 8) & 0xFFU),
                  static_cast<unsigned>(deviceId & 0xFFU));
    return output;
}

void printDevices(const std::vector<DiscoveredDevice> &devices)
{
    for (const auto &device : devices) {
        std::cout << "  - device_id=" << displayDeviceId(device.deviceId)
                  << " fw=" << static_cast<unsigned>(device.fwMajor) << "."
                  << static_cast<unsigned>(device.fwMinor) << "."
                  << static_cast<unsigned>(device.fwPatch) << "\n";
    }
}

void printUsage(const char *program)
{
    std::cerr
        << "Usage: " << program
        << " <device_path> <bin_file> [device_id_hex] [discover_wait_ms]"
           " [batch_size] [chunk_delay_ms] [max_session_ms]\n"
        << "  device_id_hex omitted: exactly one discovered device is required\n"
        << "  broadcast ffffffff is intentionally rejected\n"
        << "  defaults: discover_wait_ms=1000 batch_size=5 chunk_delay_ms=300"
           " max_session_ms=120000\n";
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 8) {
        printUsage(argv[0]);
        return 64;
    }

    const std::string devicePath = argv[1];
    const std::string binFile = argv[2];
    const bool hasRequestedDevice = argc >= 4;
    uint32_t requestedDeviceId = 0;
    if (hasRequestedDevice && !parseDeviceId(argv[3], &requestedDeviceId)) {
        std::cerr << "Invalid 24-bit device_id_hex: " << argv[3] << "\n";
        return 64;
    }

    int discoverWaitMs = 1000;
    int batchSize = 5;
    // ESP32 uses one half-duplex radio owner and completes each ACK TX before
    // re-arming RX. Leave one full ACK/TX/RX budget between DATA packets.
    int chunkDelayMs = 300;
    int maxSessionMs = 120000;
    if ((argc >= 5 && !parsePositiveInt(argv[4], &discoverWaitMs)) ||
        (argc >= 6 && !parsePositiveInt(argv[5], &batchSize)) ||
        (argc >= 7 && !parsePositiveInt(argv[6], &chunkDelayMs)) ||
        (argc >= 8 && !parsePositiveInt(argv[7], &maxSessionMs))) {
        std::cerr << "Invalid positive numeric option\n";
        return 64;
    }
    if (discoverWaitMs > 5000 || batchSize > 32 || chunkDelayMs > 1000) {
        std::cerr << "Option outside safe bounds\n";
        return 64;
    }

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[discover_session] transport open failed: " << devicePath << "\n";
        return 2;
    }
    if (transport.startRx() != Cc1101Status::Ok) {
        std::cerr << "[discover_session] startRx failed\n";
        transport.close();
        return 2;
    }

    std::cout << "[discover_session] sending one DISCOVER; wait="
              << discoverWaitMs << "ms\n";
    const auto devices = discoverDevices(transport, discoverWaitMs);
    if (devices.empty()) {
        std::cerr << "[discover_session] no device discovered; START not sent\n";
        transport.close();
        return 1;
    }
    std::cout << "[discover_session] discovered " << devices.size() << " device(s):\n";
    printDevices(devices);

    const DiscoveredDevice *selected = nullptr;
    if (hasRequestedDevice) {
        const auto match = std::find_if(
            devices.begin(), devices.end(), [&](const DiscoveredDevice &device) {
                return device.deviceId == requestedDeviceId;
            });
        if (match == devices.end()) {
            std::cerr << "[discover_session] requested device was not discovered; START not sent\n";
            transport.close();
            return 1;
        }
        selected = &*match;
    } else if (devices.size() == 1U) {
        selected = &devices.front();
    } else {
        std::cerr << "[discover_session] multiple devices found; specify device_id_hex;"
                     " START not sent\n";
        transport.close();
        return 1;
    }

    std::cout << "[discover_session] selected="
              << displayDeviceId(selected->deviceId) << " fw="
              << static_cast<unsigned>(selected->fwMajor) << "."
              << static_cast<unsigned>(selected->fwMinor) << "."
              << static_cast<unsigned>(selected->fwPatch) << "\n";

    // Keep the same open transport and RX state: the selected ESP32 re-arms RX
    // after DISCOVER_ACK and is ready for this zero-extended targeted START.
    OtaSession session(transport, batchSize, 300, 5, chunkDelayMs);
    session.setOnStateChanged([](OtaSessionState state) {
        std::cout << "[discover_session] state=" << otaSessionStateName(state) << "\n";
    });
    if (!session.start(binFile, selected->deviceId)) {
        std::cerr << "[discover_session] session start failed: "
                  << session.errorMessage() << "\n";
        transport.close();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(maxSessionMs);
    uint32_t lastAcked = 0;
    while (session.state() != OtaSessionState::Completed &&
           session.state() != OtaSessionState::Failed) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[discover_session] session exceeded " << maxSessionMs
                      << "ms; stopping\n";
            transport.close();
            return 1;
        }
        session.tick(otaSessionNowMs());
        const auto progress = session.progress();
        if (progress.ackedChunks != lastAcked) {
            std::cout << "[discover_session] progress=" << progress.ackedChunks
                      << "/" << progress.totalChunks << "\n";
            lastAcked = progress.ackedChunks;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    transport.close();
    if (session.state() == OtaSessionState::Completed) {
        std::cout << "[discover_session] completed\n";
        return 0;
    }
    std::cerr << "[discover_session] failed: " << session.errorMessage() << "\n";
    return 1;
}
