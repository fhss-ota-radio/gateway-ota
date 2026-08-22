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
// [RF 프로필 값 — 2026-08-22 실기기 1차 실패로 정정됨] 처음엔
// sync_word=0x2DD4(gateway-ota README의 "OTA 전용" 값)를 그대로 썼다가,
// 실기기에서 프로토콜 핸드셰이크(CONFIG/ACTIVATE ACK)는 성공하고도 ESP32가
// 5초 SYNC 타임아웃으로 MENU_OTA에 되돌아가는 걸 확인했다. ESP32
// firmware-esp32의 rf_transport.c(s_433mhz_settings[])를 직접 읽어보니
// **FHSS 호핑용 싱크워드는 0x2DD4가 아니라 팀 공용 값 0xD391**이었다 —
// 0x2DD4는 "OTA가 채널 0에 고정돼 있을 때"만 쓰는 값이고, FHSS로 호핑하는
// 채널들(1번 이상)은 팀 공용 싱크워드를 그대로 쓴다. 두 값을 헷갈리면
// CONFIG/ACTIVATE 같은 상위 프로토콜은 다 성공해도(그건 아직 채널 0에서
// 오가는 대화라 안 걸림) 실제 호핑 채널에서는 두 기기가 서로 다른
// 싱크워드를 듣고 있어서 절대 무선으로 만날 수 없다 — 그래서 SYNC
// 타임아웃만 계속 났던 것. base_freq_hz/channel_spacing_hz/mdmcfg/pktctrl은
// firmware-esp32 값과 비교해 이미 일치(또는 오차 무시 가능한 수준)임을
// 확인했다. 상세: design-notes-gateway-ota-es.md 42절.
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

// [2026-08-22, 실기기에서 발견] base=0(strtoul 자동 판별)으로 했더니
// "0x" 접두어 없이 그냥 "A29E60"만 넘기면 첫 글자 'A'가 10진수로 무효라
// 조용히 0으로 파싱되던 버그가 있었다(ConfigFailed로만 나와서 원인
// 파악이 어려웠음). smoke_session_send_main.cpp의 parseHexDeviceId()와
// 같은 관례로 맞춤 — base=16 명시(접두어 "0x"가 있어도 없어도 둘 다
// 정상 동작, ota_smoke_discover 등 다른 CLI에서 쓰던 것과 같은 입력
// 형식을 그대로 받을 수 있게).
std::vector<uint32_t> parseTargetList(const std::string &csv)
{
    std::vector<uint32_t> ids;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty())
            ids.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
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
    // session_id_hex/seed_hex는 이름대로 항상 16진수 — base=16 명시(위
    // parseTargetList 주석과 같은 이유. "0x" 접두어 있어도/없어도 둘 다 됨).
    const uint32_t sessionId = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 16));
    const std::vector<uint32_t> targets = parseTargetList(argv[3]);
    const uint32_t generation = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0));
    const uint8_t channelCount = static_cast<uint8_t>(argc >= 6 ? std::strtoul(argv[5], nullptr, 0) : 8);
    const uint8_t firstChannel = static_cast<uint8_t>(argc >= 7 ? std::strtoul(argv[6], nullptr, 0) : 1);
    const uint32_t seed = static_cast<uint32_t>(argc >= 8 ? std::strtoul(argv[7], nullptr, 16) : 0);
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

    // [2026-08-22 추가, 실기기에서 발견] FHSS 호핑은 이 프로세스가 끝나도
    // 커널 드라이버 레벨에서 계속 돈다 — startFhss()를 부른 이전 실행이
    // stopFhss()를 안 부르고 끝나면(이 도구는 의도적으로 자동으로 안 끔,
    // 아래 종료 메시지 참고) 다음 실행이 시작될 때도 칩이 여전히
    // 1~8번 채널을 계속 호핑 중이다. CONFIG/ACTIVATE는 채널 0으로 보내야
    // 하는데 칩이 딴 채널에 가 있으면 ESP32가 물리적으로 못 듣는다.
    //
    // [2026-08-22 정정] kernel-cc1101-spi의 cc1101_fhss_stop()(cc1101_fhss.c
    // 443~479행)을 직접 읽어보니, STOP 자체가 이미 내부적으로
    // cc1101_switch_channel(reserved_channel)을 호출해 채널을 되돌린다 —
    // 즉 아래 setChannel(0)은 이론상 중복 호출이다(reserved_channel=0으로
    // 맞춰 배포했으므로). 그런데도 세 번째 실기기 시도가 여전히
    // ConfigFailed로 실패해서, "이전 실행이 호핑 상태를 남겨뒀다" 가설이
    // 진짜 원인이 맞는지 자체가 불확실해졌다. 그래서 추측 대신 매 실행마다
    // stopFhss() 호출 결과와 getFhssStatus()를 CONFIG 전송 직전에 그대로
    // 찍어서, 다음 실기기 로그에서 "정말 아직 호핑 중이었는지"를 눈으로
    // 바로 확인할 수 있게 했다(design-notes-gateway-ota-es.md 42절 3차
    // 시도 항목 참고 — ESP32 쪽 시리얼 로그도 이 판단에 필요함).
    {
        const auto beforeStop = transport.getFhssStatus();
        std::cout << "[fhss_activate][diag] stopFhss 전: enabled=" << beforeStop.enabled
                   << " synchronized=" << beforeStop.synchronized
                   << " channel=" << static_cast<int>(beforeStop.currentChannel)
                   << " role=" << static_cast<int>(beforeStop.role) << "\n";
    }
    const Cc1101Status stopResult = transport.stopFhss();
    std::cout << "[fhss_activate][diag] stopFhss() 결과 코드=" << static_cast<int>(stopResult)
               << " (0이 아니면 ioctl 자체가 실패한 것 — errno 기반 상태코드는 "
               << "cc1101_status.h 참고)\n";
    if (transport.setChannel(0) != Cc1101Status::Ok)
        std::cerr << "setChannel(0) 실패 — 그래도 계속 진행\n";
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "startRx 실패 — 그래도 계속 진행\n";
    {
        const auto afterStop = transport.getFhssStatus();
        std::cout << "[fhss_activate][diag] stopFhss+setChannel(0) 후: enabled="
                   << afterStop.enabled << " synchronized=" << afterStop.synchronized
                   << " channel=" << static_cast<int>(afterStop.currentChannel)
                   << " role=" << static_cast<int>(afterStop.role) << "\n";
    }

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
    // [2026-08-22 추가, 실기기 3차 ConfigFailed 디버깅용] 매 재시도마다
    // transport.send() 성공 여부와, 대기 중 들어온 패킷(매칭 안 되는 것
    // 포함)을 그대로 찍는다 — fhssrollout.h/.cpp의 FhssRolloutLogFn 참고.
    const auto onRolloutLog = [](const std::string &msg) {
        std::cout << "[fhss_activate][log] " << msg << "\n";
    };
    const auto outcomes = rolloutFhssConfig(transport, sessionId, targets, policy,
                                             timeoutMs, maxRetry, onRolloutLog);

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
    kernelConfig.rfBaseFreqHz = 433919830u;      // firmware-esp32 FREQ2/1/0=0x10/0xB0/0x71 실측 역산값
    kernelConfig.rfChannelSpacingHz = 199951u;   // firmware-esp32 CHANSPC_E/M 실측 역산값
    kernelConfig.rfSyncWord = 0xD391u;           // [2026-08-22 정정] FHSS 호핑 채널은 OTA 전용
                                                   // 0x2DD4가 아니라 팀 공용 싱크워드 사용 — 위 파일
                                                   // 상단 주석 참고
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
