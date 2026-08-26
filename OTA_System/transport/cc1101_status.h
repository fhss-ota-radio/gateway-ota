#ifndef CC1101_STATUS_H
#define CC1101_STATUS_H

#include <cstdint>

// 통신 담당 팀원(4·5)의 "CC1101 Radio API" 문서(cc1101-radio-api.md, Draft v0.1)
// 3절(cc1101_status_t)·4절(cc1101_rx_metadata_t)과 "같은 의미"로 맞춘 타입.
//
// 주의: ESP32 쪽 C enum/struct와 심볼을 공유하는 게 아니라, 각 플랫폼이 같은 뜻을
// 각자 언어에 맞게 정의한 것. gateway-ota(라즈베리파이·C++) <-> ESP32(C, ESP-IDF)는
// 컴파일 결과물을 공유하지 않고, "의미"만 문서 기준으로 맞춘다.
//
// [의도적으로 Qt 의존성 없음] qint64/quint8 같은 QtGlobal 타입 대신 <cstdint>의
// int64_t/uint8_t를 씀 — transport/ 전체를 core/처럼 Qt 없이 빌드 가능하게 하기 위함.
enum class Cc1101Status {
    Ok = 0,          // 정상 완료
    NoData,          // 현재 수신 데이터 없음
    Timeout,         // 제한 시간 내 완료되지 않음
    Busy,            // 다른 작업이 CC1101 사용 중
    InvalidArg,      // 잘못된 인자
    InvalidState,    // 현재 상태에서 실행 불가
    FrameTooLarge,   // 최대 프레임 크기 초과
    NotInitialized,  // 아직 open()되지 않음 (문서엔 없지만 스텁 상태 표현용으로 추가)
    CrcError,        // 하드웨어 CRC 실패
    FifoError,       // RX Overflow 또는 TX Underflow
    IoError,         // open/write/read/ioctl 등 시스템콜 실패
    HardwareError,   // CC1101 미응답 또는 비정상 상태
};

// 수신 시 함께 오는 상태 정보 (cc1101-radio-api.md 4절).
// rxTimestampUs 기준: "패킷 수신 완료를 처음 감지한 시각", monotonic clock(CLOCK_MONOTONIC) 사용.
// 상위 코드가 패킷을 읽거나 파싱한 시각을 쓰지 않도록 주의 (문서 4절 명시 사항).
struct Cc1101RxMetadata
{
    int64_t rxTimestampUs = 0;
    int16_t rssiDbmX10 = 0;
    uint8_t lqi = 0;
    bool    crcOk = false;
    uint8_t channel = 0;
};

// [2026-08-22 추가] FHSS(주파수 도약) 역할값 — kernel-cc1101-spi/cc1101_ioctl.h의
// CC1101_FHSS_ROLE_MASTER/SLAVE와 "의미"만 맞춘 값(Cc1101Status와 같은 원칙 —
// 이 파일은 Qt/리눅스 헤더 의존 없이 어디서나 컴파일돼야 해서, 커널 UAPI 상수를
// 직접 include하지 않고 우리가 같은 값으로 다시 선언한다).
enum class Cc1101FhssRole : uint8_t {
    Master = 1,
    Slave = 2,
};

// configureFhss()에 넘길 설정값. 두 부분으로 나뉘는 이유는 커널
// cc1101_fhss_config 구조체와 같다:
//   - rf* 필드: 무선 레벨 상수(기준 주파수/채널 간격/싱크워드/레지스터값).
//     ota_protocol의 FHSS_CONFIG 패킷엔 안 실림 — 팀이 정한 로컬 상수를
//     Gateway가 알아서 채워 넣는다 (예: docs의 channel-profile 표 참고).
//   - 나머지(hop policy) 필드: ESP32에도 그대로 방송(ota_fhss_config_fields_t)하는
//     값과 동일해야 한다 — Gateway와 ESP32가 "같은 호핑 순서"를 계산하려면
//     seed/channelCount/firstChannel 등이 한 글자도 안 틀리고 같아야 하기 때문.
struct Cc1101FhssConfig
{
    uint32_t generation = 0;
    uint32_t algorithmId = 1; // CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION

    // -- rf profile (로컬 상수, 와이어로 안 나감) --
    uint32_t rfBaseFreqHz = 0;
    uint32_t rfChannelSpacingHz = 0;
    uint16_t rfSyncWord = 0;
    uint8_t  rfMdmcfg4 = 0;
    uint8_t  rfMdmcfg3 = 0;
    uint8_t  rfPktctrl1 = 0;
    uint8_t  rfPktctrl0 = 0;

    // -- hop policy (ESP32에도 그대로 방송되는 값과 동일해야 함) --
    uint32_t seed = 0;
    uint32_t slotDurationUs = 0;
    uint32_t channelSwitchGuardUs = 0;
    uint16_t channelCount = 0;
    uint8_t  firstChannel = 1;      // 랑데부 채널과 같음 (오늘 계획 기준)
    uint8_t  rendezvousChannel = 1;
    uint8_t  reservedChannel = 0;   // OTA 전용(채널 0)은 호핑에서 제외
    uint8_t  algorithmVersion = 1;  // CC1101_FHSS_ALGORITHM_VERSION
    uint8_t  channelProfileId = 0;
};

// getFhssStatus()가 돌려주는, 커널 cc1101_fhss_status를 그대로 옮긴 값 —
// "지금 호핑 중인지"를 우리가 따로 기억하지 않고 항상 이 함수로 커널에
// 직접 물어보기 위한 것 (design-notes 참고: 상태를 이중으로 들고 있으면
// 어긋날 위험이 있어서 커널을 단일 진실 공급원으로 둔다).
struct Cc1101FhssStatus
{
    bool     enabled = false;
    bool     synchronized = false;
    uint8_t  currentChannel = 0;
    uint8_t  role = 0;          // Cc1101FhssRole 값 또는 0(미설정)
    uint32_t generation = 0;
    uint64_t currentSlot = 0;
    int32_t  lastError = 0;
    uint32_t syncMisses = 0;
    uint32_t syncPackets = 0;
};

#endif // CC1101_STATUS_H
