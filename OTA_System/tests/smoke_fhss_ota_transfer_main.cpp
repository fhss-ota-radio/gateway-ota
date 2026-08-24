// 실기기(CC1101) 스모크테스트 — FHSS_CONFIG/ACTIVATE로 ESP32를 호핑에
// 참여시키고, Gateway 자신도 커널 호핑(MASTER)을 켠 뒤, 그 위에서
// OtaSession(FSM)으로 실제 펌웨어 파일을 전송하는 CLI.
//
// [왜 필요한가] ota_smoke_fhss_activate는 CONFIG->ACTIVATE->SYNC 관찰까지만
// 하고 끝나는 도구였다(2026-08-23 실기기 검증 완료, design-notes-gateway-ota-es.md
// 42절 5차 시도). OTA_START/DATA/END를 보내는 코드가 아예 없어서, "SYNC는 되는데
// 파일 전송이 안 됨"은 버그가 아니라 그 도구가 원래 거기까지만 하도록 만들어진
// 결과였다. 이 파일이 그 다음 단계 — 호핑 중인 채널 위에서 실제로
// OtaSession::start()를 이어붙여서 파일을 보내는 통합 CLI다.
//
// [설계] 앞부분(CONFIG/ACTIVATE/커널 호핑 시작)은 smoke_fhss_activate_main.cpp와
// 동일한 로직을 그대로 재사용한다(rolloutFhssConfig() + configureFhss() +
// startFhss()). 그 다음 ESP32가 SYNC를 잡을 시간을 잠깐 기다린 뒤(SYNC_ACQUIRED까지
// 보통 1~2초, 아래 kSyncSettleMs 참고), smoke_session_send_main.cpp와 동일한
// 방식으로 OtaSession을 돌려서 파일을 보낸다. transport.send()/recv()는 현재
// 호핑 중인 채널이 무엇이든 상관없이 그대로 동작한다 — 채널 전환은 커널
// hop worker가 알아서 하고, 애플리케이션은 평소처럼 read/write만 하면 되는
// 구조이기 때문이다(kernel-cc1101-spi README 설계 원칙).
//
// [session_id 재사용] FHSS_CONFIG/ACTIVATE에 쓴 session_id를 OtaSession::start()의
// sessionId 인자로도 그대로 넘긴다 — 이 CLI 한 번 실행이 "하나의 세션"이라는
// 개념을 일관되게 유지하기 위함(다르게 할 이유가 없고, 로그 추적도 더 쉬움).
//
// 사용법:
//   ota_smoke_fhss_ota_transfer <device_path> <bin_file> <session_id_hex>
//       <target_id_hex> <generation> [channel_count=8] [first_channel=1]
//       [seed_hex=0] [batchSize=5] [chunkDelayMs=40] [timeoutMs=300] [maxRetry=5]
//
// 예: ota_smoke_fhss_ota_transfer /dev/cc1101 firmware.bin 0x1 A29E60 1 8 1 0x46485353

#include "cc1101transport.h"
#include "fhssrollout.h"
#include "otasession.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// smoke_fhss_activate_main.cpp/smoke_session_send_main.cpp와 동일한 관례 —
// base=16 명시(접두어 "0x" 있어도 없어도 둘 다 됨, 2026-08-22 hex 파싱 버그
// 교훈 참고).
bool parseHexU32(const std::string &text, uint32_t *out)
{
    if (text.empty())
        return false;
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 16);
        if (consumed != text.size())
            return false;
        *out = static_cast<uint32_t>(value);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void printUsage(const char *argv0)
{
    std::cerr << "사용법: " << argv0
              << " <device_path> <bin_file> <session_id_hex> <target_id_hex> "
                 "<generation> [channel_count=8] [first_channel=1] [seed_hex=0] "
                 "[batchSize=5] [chunkDelayMs=40] [timeoutMs=300] [maxRetry=5]\n"
              << "  예: " << argv0
              << " /dev/cc1101 firmware.bin 0x1 A29E60 1 8 1 0x46485353\n";
}

// FHSS_CONFIG/ACTIVATE 완료 후 커널 MASTER 호핑을 켜면, ESP32가 랑데부
// 채널에서 SYNC를 찾아 SYNC_ACQUIRED까지 가는 데 보통 1~2초 걸린다(2026-08-23
// 실기기 로그: 3개 SYNC 패킷 검증 후 SYNC_ACQUIRED, slot_duration_us=300000
// 기준 약 0.9~1.2초). 그 전에 OTA_START를 보내면 ESP32가 아직 OTA_FHSS_READY가
// 아니라서 응답이 없을 수 있으므로, 여유를 둬서 기다린다.
//
// [2026-08-24 정정: 2000 -> 4000] 위 "0.9~1.2초"는 **첫 SYNC를 이미 잡은
// 뒤부터** 재는 시간이었다. 실제로는 그 앞에 "첫 SYNC를 잡기까지"가
// 따로 있고, 이게 훨씬 길다:
//
//   ESP32는 ACTIVATE 직후 랑데부 채널(=first_channel, 보통 1번)에 고정된
//   채 SYNC를 기다린다. 그런데 Gateway는 이미 8채널을 순회 중이라 1번
//   채널로 돌아오는 건 8슬롯마다 = 300ms x 8 = 2.4초에 한 번뿐이다.
//   즉 첫 SYNC까지 최악 2.4초 + 획득까지 3슬롯 0.9초 = 최악 3.3초.
//
// 2000ms는 이 최악값보다 짧아서, ESP32가 아직 SEARCHING인 상태에서
// OTA_START/DATA가 먼저 도착한다. 그러면 ESP32는 그 DATA를 처리하느라
// 바로 다음 SYNC를 놓치는데, 아직 "획득 전"(SYNCHRONIZING) 단계라
// 관용이 없어서 기준점을 통째로 버리고 랑데부 채널로 되돌아간다 —
// 그럼 또 2.4초를 기다려야 하고, 그 사이 DATA는 계속 오므로 같은 일이
// 반복되어 영영 TRACKING에 못 간다. 결국 ESP32의 5초 동기화 타임아웃
// (firmware-esp32 main/fsm.c OTA_FHSS_SYNC_TIMEOUT_MS)이 먼저 터져
// 세션이 폐기된다.
//
// 148 실기기 로그(2026-08-24)에서 이 교착이 정확히 재현됐고, 세션이
// 폐기되어 DATA가 멈추자마자 300ms 만에 깨끗이 SYNC_ACQUIRED까지 간
// 것이 결정적 증거였다. 상세: design-notes-gateway-ota-es.md 54절.
//
// 최악 3.3초 + 여유를 두어 4000ms로 올린다. ESP32 쪽 타임아웃도 5초 ->
// 10초로 함께 올려서, 이 대기가 오히려 타임아웃을 유발하지 않게 했다.
constexpr int kSyncSettleMs = 4000;
constexpr int kSlotPollMs = 2;
constexpr int kPostSyncGuardMs = 25;
constexpr int kSlotGateTimeoutMs = 1200;
// [2026-08-24 추가, perf/fhss-slot-batch-tx] 슬롯 하나당 패킷 하나씩만
// 보내던 게 너무 느려서(슬롯 300ms마다 1개 = 7825청크면 40분 가까이) 같은
// 슬롯 안에서 여러 개를 연달아 보내도록 바꿈. 앞쪽(kPostSyncGuardMs)은
// 그대로 두고, 뒤쪽에도 이만큼(kTailGuardMs) 여유를 남겨서 다음 슬롯
// 경계(=다음 SYNC 송신 시점)와 안 겹치게 함 — 아직 실기기로 잰 값이
// 아니라 "합리적인 추정치"라서, 첫 실기기 테스트 로그(각 전송이 안전
// 창의 몇 ms 지점에서 나갔는지)를 보고 좁혀나갈 것.
constexpr int kTailGuardMs = 40;
// [2026-08-24 추가] 같은 슬롯 안에서 연속으로 보낼 때 패킷 사이에 최소한
// 이만큼은 띄운다.
//
// [왜 필요한가] CC1101은 반이중(half-duplex — 송신과 수신을 동시에 못 하는
// 방식) 트랜시버 하나뿐이라, ESP32가 ACK를 송신하는 동안은 귀가 닫혀 있다.
// 148 실기기 로그(2026-08-24, ESP32 fe7f96c)로 이 왕복 시간을 실측했다:
//
//   23185  RX DATA(seq=0)        DATA 도착
//   23234  DATA accepted seq=0
//   23248  TX ACK seq=0
//   23306  TX_RESULT ch=1        ACK 송신 완료 -> 왕복 약 121ms
//   23337  batch store seq=2, received=0x05   <- seq=1은 아예 못 받음
//   23403  TX_RESULT (seq=2 ACK)
//   23449  batch store seq=4, received=0x15   <- seq=3도 못 받음
//
// 위 실행에서 Gateway는 슬롯 하나에 5개를 16ms 간격으로 쐈는데(안전창
// 재사용 최적화의 부작용), ESP32의 왕복이 121ms라서 ACK 송신 중에 지나간
// 홀수 seq가 통째로 유실됐다 — missing_mask가 0x1A(=seq 1,3,4)로 고정되고
// 재전송 한도를 넘겨 실패. "패킷을 촘촘히 보낼수록 오히려 덜 도착하는"
// 상태였던 것.
//
// 실측 121ms에 여유를 얹어 150ms로 둔다. 안전창(슬롯 300ms -
// kPostSyncGuardMs 25 - kTailGuardMs 40 = 235ms) 안에서는 슬롯당 2개가
// 나가고, 나머지는 다음 슬롯 창으로 자연히 밀린다(batchSize와 무관하게
// 동작 — 창이 닫히면 ensureSafeWindow()가 다음 슬롯을 기다림).
// 상세: docs/note/design-notes-gateway-ota-es.md 55절.
constexpr int kMinPacketGapMs = 150;

class SlotAwareTransport final : public ITransport
{
public:
    // slotDurationUs: FhssHopPolicy::slotDurationUs를 그대로 받음 — 안전
    // 창이 이번 슬롯 안에서 아직 안 닫혔는지 판단하려면 슬롯 길이를 알아야
    // 하는데, 예전엔 이 클래스가 몰라도 됐음(매번 다음 슬롯을 기다렸으니까).
    SlotAwareTransport(Cc1101Transport &transport, uint32_t generation, uint32_t slotDurationUs)
        : m_transport(transport), m_generation(generation),
          m_slotDurationMs(static_cast<int64_t>(slotDurationUs) / 1000)
    {
    }

    bool open() override { return m_transport.isOpen() || m_transport.open(); }
    void close() override { m_transport.close(); }
    bool isOpen() const override { return m_transport.isOpen(); }

    bool send(const std::vector<uint8_t> &packet) override
    {
        if (!ensureSafeWindow())
            return false;

        const uint8_t type = packet.empty() ? 0 : packet.front();
        const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_windowOpenedAt).count();
        std::cout << "[fhss_ota][slot_tx] type=" << static_cast<unsigned>(type)
                  << " slot=" << m_openWindowSlot
                  << " window_elapsed_ms=" << elapsedMs << "\n";

        const bool ok = m_transport.send(packet);
        if (!ok) {
            // 실패했으면 이 창을 더 이상 못 믿음(예: 그 사이 동기화가
            // 깨졌을 수도 있음) — 다음 send() 호출은 처음부터 다시 게이팅.
            m_openWindowSlot = kNoWindow;
        } else {
            m_lastSentAt = std::chrono::steady_clock::now();
        }
        return ok;
    }

    std::vector<uint8_t> recv() override { return m_transport.recv(); }

private:
    static constexpr uint64_t kNoWindow = ~static_cast<uint64_t>(0);

    bool usable(const Cc1101FhssStatus &status) const
    {
        return status.enabled && status.synchronized &&
               status.role == static_cast<uint8_t>(Cc1101FhssRole::Master) &&
               status.generation == m_generation && status.lastError == 0;
    }

    static void logGateFailure(const char *where, const Cc1101FhssStatus &status)
    {
        std::cerr << "[fhss_ota][slot_tx] " << where
                  << " enabled=" << status.enabled
                  << " synchronized=" << status.synchronized
                  << " role=" << static_cast<unsigned>(status.role)
                  << " generation=" << status.generation
                  << " slot=" << status.currentSlot
                  << " error=" << status.lastError << "\n";
    }

    // 지금 바로 send()해도 안전한 상태인지 확인한다.
    //
    // 이미 이번 슬롯에서 안전 창을 열어둔 상태(직전 send()가 같은 슬롯에서
    // 성공)라면, 그 창이 아직 안 닫혔는지(같은 슬롯 + 뒤쪽 여유 충분)만
    // 빠르게 확인하고 대기 없이 바로 통과시킨다 — 이게 "슬롯당 여러 개"의
    // 핵심. 창이 없거나 이미 닫혔으면 예전 방식 그대로 다음 슬롯 경계까지
    // 기다렸다가 새 창을 연다.
    bool ensureSafeWindow()
    {
        // [2026-08-24 추가] 반이중 왕복 보호 — 직전 전송 이후
        // kMinPacketGapMs가 안 지났으면 그만큼 기다린다. ESP32가 직전
        // DATA의 ACK를 송신하는 동안은 수신을 못 하므로, 이걸 안 지키면
        // 그 사이에 보낸 패킷이 통째로 유실된다(상단 kMinPacketGapMs
        // 주석의 실측 로그 참고). 창이 살아있는 경로/새 창을 여는 경로
        // 둘 다에 적용돼야 하므로 함수 맨 앞에 둔다 — 새 창을 여는
        // 경로는 어차피 슬롯 경계까지 기다리느라 이 간격이 자연히
        // 확보되지만, 명시적으로 보장해두는 편이 안전하다.
        if (m_lastSentAt.time_since_epoch().count() != 0) {
            for (;;) {
                const int64_t sinceLastMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_lastSentAt).count();
                if (sinceLastMs >= kMinPacketGapMs)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(kSlotPollMs));
            }
        }

        if (m_openWindowSlot != kNoWindow) {
            const auto status = m_transport.getFhssStatus();
            const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_windowOpenedAt).count();
            // 창이 열린 시각(m_windowOpenedAt)은 대략 "슬롯 경계 + kPostSyncGuardMs"
            // 지점이므로, 다음 슬롯 경계까지 남은 시간 ≈
            // m_slotDurationMs - kPostSyncGuardMs - elapsedMs. 이게
            // kTailGuardMs 이상 남아있어야 새로 보내도 안전하다고 봄.
            const bool stillSameSlot = usable(status) && status.currentSlot == m_openWindowSlot;
            const bool stillHasRoom =
                elapsedMs + kTailGuardMs < m_slotDurationMs - kPostSyncGuardMs;
            if (stillSameSlot && stillHasRoom)
                return true; // 대기 없이 통과 — 슬롯당 여러 개 보내지는 지점

            m_openWindowSlot = kNoWindow; // 창 닫힘(슬롯 바뀜/여유 부족/동기화 깨짐) -> 새로 게이팅
        }

        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(kSlotGateTimeoutMs);
        auto status = m_transport.getFhssStatus();
        if (!usable(status)) {
            logGateFailure("initial status", status);
            return false;
        }

        const uint64_t baselineSlot = status.currentSlot;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSlotPollMs));
            status = m_transport.getFhssStatus();
            if (!usable(status)) {
                logGateFailure("status while waiting", status);
                return false;
            }
            if (status.currentSlot == baselineSlot)
                continue;

            const uint64_t sendSlot = status.currentSlot;
            std::this_thread::sleep_for(std::chrono::milliseconds(kPostSyncGuardMs));
            const auto verified = m_transport.getFhssStatus();
            if (!usable(verified) || verified.currentSlot != sendSlot)
                continue;

            m_openWindowSlot = sendSlot;
            m_windowOpenedAt = std::chrono::steady_clock::now();
            std::cout << "[fhss_ota][slot_tx] new window slot=" << sendSlot
                      << " channel=" << static_cast<unsigned>(verified.currentChannel)
                      << " post_sync_ms=" << kPostSyncGuardMs << "\n";
            return true;
        }

        std::cerr << "[fhss_ota][slot_tx] safe slot timeout after "
                  << kSlotGateTimeoutMs << "ms\n";
        return false;
    }

    Cc1101Transport &m_transport;
    uint32_t m_generation;
    int64_t m_slotDurationMs;
    uint64_t m_openWindowSlot = kNoWindow;
    std::chrono::steady_clock::time_point m_windowOpenedAt{};
    // 기본 생성된 time_point는 epoch(=count() 0)이므로, "아직 한 번도 안
    // 보냄"을 별도 bool 없이 이걸로 구분한다(ensureSafeWindow() 참고).
    std::chrono::steady_clock::time_point m_lastSentAt{};
};

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 6) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string devicePath = argv[1];
    const std::string binFile = argv[2];

    uint32_t sessionId = 0;
    uint32_t targetDeviceId = 0;
    uint32_t generation = 0;
    if (!parseHexU32(argv[3], &sessionId)) {
        std::cerr << "session_id_hex 파싱 실패: " << argv[3] << "\n";
        return 1;
    }
    if (!parseHexU32(argv[4], &targetDeviceId)) {
        std::cerr << "target_id_hex 파싱 실패: " << argv[4] << "\n";
        return 1;
    }
    if (!parseHexU32(argv[5], &generation)) {
        std::cerr << "generation 파싱 실패: " << argv[5] << "\n";
        return 1;
    }

    const uint8_t channelCount = static_cast<uint8_t>(argc >= 7 ? std::strtoul(argv[6], nullptr, 0) : 8);
    const uint8_t firstChannel = static_cast<uint8_t>(argc >= 8 ? std::strtoul(argv[7], nullptr, 0) : 1);
    uint32_t seed = 0;
    if (argc >= 9 && !parseHexU32(argv[8], &seed)) {
        std::cerr << "seed_hex 파싱 실패: " << argv[8] << "\n";
        return 1;
    }
    const int batchSize = (argc >= 10) ? std::atoi(argv[9]) : 5;
    const int chunkDelayMs = (argc >= 11) ? std::atoi(argv[10]) : 40;
    const int timeoutMs = (argc >= 12) ? std::atoi(argv[11]) : 300;
    const int maxRetry = (argc >= 13) ? std::atoi(argv[12]) : 5;

    std::cout << "[fhss_ota] device=" << devicePath << " file=" << binFile
               << " session_id=0x" << std::hex << sessionId
               << " target=0x" << targetDeviceId << std::dec
               << " generation=" << generation
               << " channel_count=" << static_cast<int>(channelCount)
               << " first_channel=" << static_cast<int>(firstChannel) << "\n";
    std::cout << "[fhss_ota] (대상 ESP32가 MENU_OTA/STANDBY 상태여야 합니다)\n";

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[fhss_ota] transport open 실패: " << devicePath << "\n";
        return 1;
    }

    // [1단계] 이전 실행의 호핑 잔존을 정리 — ota_smoke_fhss_activate와 동일한
    // 이유(design-notes 42절 2~4차 시도 참고). 이번 CLI는 곧바로 다시
    // 호핑을 켤 것이므로 실질적 영향은 크지 않지만, 시작 시점을 항상 같은
    // 상태로 맞추는 관례를 그대로 따른다.
    (void)transport.stopFhss();
    if (transport.setChannel(0) != Cc1101Status::Ok)
        std::cerr << "[fhss_ota] setChannel(0) 실패 — 그래도 계속 진행\n";
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[fhss_ota] startRx 실패 — 그래도 계속 진행\n";

    FhssHopPolicy policy;
    policy.generation = generation;
    policy.algorithmVersion = 1; // OTA_FHSS_ALGORITHM_VERSION
    policy.channelProfileId = 0;
    policy.firstChannel = firstChannel;
    policy.rendezvousChannel = firstChannel;
    policy.channelCount = channelCount;
    policy.reservedChannel = 0;
    policy.seed = seed;
    policy.slotDurationUs = 300000;
    policy.channelSwitchGuardUs = 5000;

    std::cout << "[fhss_ota] 1~2단계: FHSS_CONFIG -> FHSS_ACTIVATE 배포...\n";
    const auto onRolloutLog = [](const std::string &msg) {
        std::cout << "[fhss_ota][rollout] " << msg << "\n";
    };
    const auto outcomes = rolloutFhssConfig(transport, sessionId, {targetDeviceId}, policy,
                                             timeoutMs, maxRetry, onRolloutLog);
    if (outcomes.empty() || outcomes.front().stage != FhssRolloutStage::Activated) {
        std::cerr << "[fhss_ota] FHSS 활성화 실패 — 파일 전송을 시작하지 않습니다.\n";
        transport.close();
        return 1;
    }
    std::cout << "[fhss_ota] FHSS 활성화 성공. Gateway 커널 호핑(MASTER) 시작...\n";

    // [RF 프로필 — smoke_fhss_activate_main.cpp와 동일값, 2026-08-22 실기기로
    // sync_word=0xD391/base_freq_hz=433919830/channel_spacing_hz=199951 확정됨]
    Cc1101FhssConfig kernelConfig;
    kernelConfig.generation = generation;
    kernelConfig.algorithmId = 1; // CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION
    kernelConfig.rfBaseFreqHz = 433919830u;
    kernelConfig.rfChannelSpacingHz = 199951u;
    kernelConfig.rfSyncWord = 0xD391u;
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
        std::cerr << "[fhss_ota] configureFhss 실패\n";
        transport.close();
        return 1;
    }
    if (transport.startFhss(Cc1101FhssRole::Master) != Cc1101Status::Ok) {
        std::cerr << "[fhss_ota] startFhss(MASTER) 실패\n";
        transport.close();
        return 1;
    }

    std::cout << "[fhss_ota] 3단계: ESP32가 SYNC_ACQUIRED까지 갈 시간(" << kSyncSettleMs
               << "ms) 대기...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(kSyncSettleMs));
    {
        const auto status = transport.getFhssStatus();
        std::cout << "[fhss_ota] 호핑 상태: enabled=" << status.enabled
                   << " synchronized=" << status.synchronized
                   << " channel=" << static_cast<int>(status.currentChannel) << "\n";
    }

    // [4단계] 호핑 중인 채널 위에서 OtaSession으로 실제 파일 전송.
    // smoke_session_send_main.cpp와 동일한 사용법 — transport.send()/recv()는
    // 지금이 어느 채널이든 그대로 동작한다(커널이 채널 전환을 알아서 함).
    std::cout << "[fhss_ota] 4단계: OtaSession으로 파일 전송 시작...\n";
    SlotAwareTransport slotAwareTransport(transport, generation, policy.slotDurationUs);
    if (chunkDelayMs != 0) {
        std::cout << "[fhss_ota] slot-aware TX가 패킷 간격을 소유하므로 chunkDelayMs="
                  << chunkDelayMs << "는 사용하지 않습니다.\n";
    }
    OtaSession session(slotAwareTransport, batchSize, timeoutMs, maxRetry,
                       /*chunkDelayMs=*/0);
    session.setOnStateChanged([](OtaSessionState s) {
        std::cout << "[fhss_ota] 상태 -> " << otaSessionStateName(s) << "\n";
    });
    session.setOnLog([](const std::string &msg) {
        std::cout << "[fhss_ota][log] " << msg << "\n";
    });

    if (!session.start(binFile, targetDeviceId, sessionId)) {
        std::cerr << "[fhss_ota] session.start() 실패: " << session.errorMessage() << "\n";
        transport.close();
        return 1;
    }

    uint32_t lastAcked = 0;
    while (session.state() != OtaSessionState::Completed
           && session.state() != OtaSessionState::Failed) {
        session.tick(otaSessionNowMs());

        const auto progress = session.progress();
        if (progress.ackedChunks != lastAcked) {
            std::cout << "[fhss_ota] 진행 " << progress.ackedChunks << "/"
                       << progress.totalChunks << " (배치 " << progress.currentBatchNumber << "/"
                       << progress.totalBatches << ")\n";
            lastAcked = progress.ackedChunks;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    transport.close();

    if (session.state() == OtaSessionState::Completed) {
        std::cout << "[fhss_ota] 완료 — 호핑 중 파일 전송 성공, 전체 "
                   << session.progress().totalChunks << "청크 배치 ACK까지 확인됨\n";
        return 0;
    }

    std::cerr << "[fhss_ota] 실패: " << session.errorMessage() << "\n";
    return 1;
}
