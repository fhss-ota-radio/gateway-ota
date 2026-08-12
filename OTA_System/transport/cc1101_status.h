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

#endif // CC1101_STATUS_H
