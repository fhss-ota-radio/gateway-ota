// 실기기(CC1101) 스모크테스트 — session/fhssrollout.h(rolloutFhssConfig())로
// 실제 ESP32에 FHSS 설정을 배포·활성화하고, Gateway 자신도 커널 호핑을
// 켜서(MASTER) 동기화 상태를 지켜보는 CLI.
//
// [왜 필요한가] tests/tst_fhssrollout.cpp는 FakeTransport(인메모리)로 로직만
// 검증한 것이고, 진짜 CC1101 RF 링크로 FHSS_CONFIG/FHSS_ACTIVATE를 보냈을 때
// firmware-esp32의 ota_consumer_handle_fhss_config/activate()가 실제로
// ACK을 응답하는지, 그 다음 커널이 실제로 동기화(synchronized=1)까지
// 가는지는 실기기로 확인해야 한다.
//
// [사전 조건] firmware-esp32 develop(725b27d 이상)에서 확인한 FSM 순서상,
// 대상 ESP32는 반드시 MENU_OTA(STANDBY) 화면에 들어가 있어야 FHSS_CONFIG를
// 받아준다(design-notes-gateway-ota-es.md 41절 참고). 로터리 인코더로
// OTA 메뉴 진입 후 실행할 것.
//
// [RF 프로필 값에 대한 주의] base_freq_hz/channel_spacing_hz/sync_word/
// mdmcfg3·4/pktctrl0·1은 ota_protocol.h의 FHSS_CONFIG 패킷엔 안 실리는
// "로컬 전용" 값이라 와이어로 검증할 방법이 없다 — 아래 기본값은
// kernel-cc1101-spi의 팀 공용 베이스라인(cc1101_default_regs[], 433.92MHz)
// 기준으로 잡은 추정치이고, sync_word만 gateway-ota README에 명시된 OTA
// 전용 값(0x2D/0xD4)을 썼다. channel_spacing_hz(200kHz 추정)를 포함해
// 실제 배포 전 통신 담당(팀원 4·5)과 반드시 재확인할 것 — 이 파일 어디에도
// "검증된 값"이라고 주장하지 않는다.
//
// 사용법:
//   ota_smoke_fhss_activate <device_path> <session_id_hex>
//       <target_id_hex>[,<target_id_hex>...] <generation>
//       [channel_count=8] [first_channel=1] [seed_hex=0]
//       [timeout_ms=300] [max_retry=5]
//
// 예: ota_smoke_fhss_activate /dev/cc1101 0x1 A29E60 1 8 1 0x46485353

#include "cc1101transport.h"
#include "fhssrollout.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<uint32_t> parseTargetList(const std::string &csv)
{
    std::vector<uint32_t> ids;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty())
            ids.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 0)));
    }
    return ids;
}

const char *stageName(FhssRolloutStage stage)
{
    switch (stage) {
    case FhssRolloutStage::ConfigFailed: return "ConfigFailed";
    case FhssRolloutStage::ConfigAcked:  return "ConfigAcked(ActivateFailed)";
    case FhssRolloutStage::Activated:    return "Activated";
    }
    return "?";
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 5) {
        std::cerr << "사용법: " << argv[0]
                   << " <device_path> <session_id_hex> <target_id_hex>[,<target_id_hex>...] "
                   << "<generation> [channel_count=8] [first_channel=1] [seed_hex=0] "
                   << "[timeout_ms=300] [max_retry=5]\n";
        return 1;
    }

    const std::string devicePath = argv[1];
    const uint32_t sessionId = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 0));
    const std::vector<uint32_t> targets = parseTargetList(argv[3]);
    const uint32_t generation = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0));
    const uint8_t channelCount = static_cast<uint8_t>(argc >= 6 ? std::strtoul(argv[5], nullptr, 0) : 8);
    const uint8_t firstChannel = static_cast<uint8_t>(argc >= 7 ? std::strtoul(argv[6], nullptr, 0) : 1);
    const uint32_t seed = static_cast<uint32_t>(argc >= 8 ? std::strtoul(argv[7], nullptr, 0) : 0);
    const int timeoutMs = argc >= 9 ? std::atoi(argv[8]) : 300;
    const int maxRetry = argc >= 10 ? std::atoi(argv[9]) : 5;

    if (targets.empty()) {
        std::cerr << "target_id 목록이 비어있습니다.\n";
        return 1;
    }

    std::cout << "[fhss_activate] device=" << devicePath << " session_id=0x" << std::hex
               << sessionId << " generation=" << std::dec << generation
               << " channel_count=" << static_cast<int>(channelCount)
               << " first_channel=" << static_cast<int>(firstChannel) << "\n";
    std::cout << "[fhss_activate] 대상 " << targets.size() << "대: ";
    for (uint32_t t : targets)
        std::cout << "0x" << std::hex << t << std::dec << " ";
    std::cout << "\n";
    std::cout << "[fhss_activate] (대상 ESP32들이 전부 MENU_OTA/STANDBY 상태여야 합니다)\n";

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "transport open 실패: " << devicePath << "\n";
        return 1;
    }
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "startRx 실패 — 그래도 계속 진행\n";

    FhssHopPolicy policy;
    policy.generation = generation;
    policy.algorithmVersion = 1; // OTA_FHSS_ALGORITHM_VERSION
    policy.channelProfileId = 0;
    policy.firstChannel = firstChannel;
    policy.rendezvousChannel = firstChannel; // 프로토콜 검증 조건: rendezvous == first
    policy.channelCount = channelCount;
    policy.reservedChannel = 0; // OTA 전용 채널 0 — 호핑 범위에서 제외
    policy.seed = seed;
    policy.slotDurationUs = 300000; // hop_policy 주석 기준 현재 값
    policy.channelSwitchGuardUs = 5000;

    std::cout << "[fhss_activate] 1~2단계: FHSS_CONFIG -> FHSS_ACTIVATE 순차 배포 시작...\n";
    const auto outcomes = rolloutFhssConfig(transport, sessionId, targets, policy,
                                             timeoutMs, maxRetry);

    int activatedCount = 0;
    for (const auto &outcome : outcomes) {
        std::cout << "  - device_id=0x" << std::hex << outcome.deviceId << std::dec
                   << " -> " << stageName(outcome.stage) << "\n";
        if (outcome.stage == FhssRolloutStage::Activated)
            ++activatedCount;
    }

    if (activatedCount == 0) {
        std::cout << "[fhss_activate] 활성화된 기기가 하나도 없어 Gateway 쪽 호핑은 시작하지 "
                   << "않습니다.\n";
        transport.close();
        return 1;
    }

    std::cout << "[fhss_activate] " << activatedCount << "/" << targets.size()
               << "대 활성화 성공. Gateway 쪽 커널 호핑(MASTER)을 켭니다...\n";

    // [RF 프로필 — 위 파일 상단 주석 참고] 팀 공용 베이스라인 추정치.
    Cc1101FhssConfig kernelConfig;
    kernelConfig.generation = generation;
    kernelConfig.algorithmId = 1; // CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION
    kernelConfig.rfBaseFreqHz = 433920000u;      // 팀 공용 베이스라인(433.92MHz)
    kernelConfig.rfChannelSpacingHz = 200000u;   // 추정치(~200kHz) — 재확인 필요
    kernelConfig.rfSyncWord = 0x2DD4u;           // gateway-ota README의 OTA 전용 싱크워드
    kernelConfig.rfMdmcfg4 = 0xCA;
    kernelConfig.rfMdmcfg3 = 0x83;
    kernelConfig.rfPktctrl1 = 0x04;
    kernelConfig.rfPktctrl0 = 0x05;
    kernelConfig.seed = policy.seed;
    kernelConfig.slotDurationUs = policy.slotDurationUs;
    kernelConfig.channelSwitchGuardUs = policy.channelSwitchGuardUs;
    kernelConfig.channelCount = policy.channelCount;
    kernelConfig.firstChannel = policy.firstChannel;
    kernelConfig.rendezvousChannel = policy.rendezvousChannel;
    kernelConfig.reservedChannel = policy.reservedChannel;
    kernelConfig.algorithmVersion = policy.algorithmVersion;
    kernelConfig.channelProfileId = policy.channelProfileId;

    if (transport.configureFhss(kernelConfig) != Cc1101Status::Ok) {
        std::cerr << "[fhss_activate] configureFhss 실패\n";
        transport.close();
        return 1;
    }
    if (transport.startFhss(Cc1101FhssRole::Master) != Cc1101Status::Ok) {
        std::cerr << "[fhss_activate] startFhss(MASTER) 실패\n";
        transport.close();
        return 1;
    }

    std::cout << "[fhss_activate] 3단계: 동기화 상태 5초간 관찰 (0.5초 간격)...\n";
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto status = transport.getFhssStatus();
        std::cout << "  t=" << (i + 1) * 500 << "ms enabled=" << status.enabled
                   << " synchronized=" << status.synchronized
                   << " channel=" << static_cast<int>(status.currentChannel)
                   << " sync_packets=" << status.syncPackets
                   << " sync_misses=" << status.syncMisses
                   << " last_error=" << status.lastError << "\n";
    }

    std::cout << "[fhss_activate] 종료 — 필요하면 stopFhss()는 별도 CLI/Ctrl+C 후 재실행으로 "
               << "확인하세요 (이 도구는 자동으로 끄지 않습니다).\n";
    transport.close();
    return 0;
}
