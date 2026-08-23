#include "cc1101transport.h"

#include <cerrno>

// [실구현, 2026-08-11] 팀원분(`feature/ota-tx` 브랜치)이 작성한 걸 이 브랜치로
// 가져왔습니다. cc1101_ioctl.h는 kernel-cc1101-spi의 UAPI 헤더와 동일한 것을
// 이 폴더에 복사해서 씁니다(커널 쪽과 유저 공간이 같은 ioctl 번호/구조체를
// 봐야 하는 계약 헤더 — ota_protocol.h와 같은 성격).
//
// [플랫폼 가드] cc1101_ioctl.h가 <linux/types.h>/<linux/ioctl.h>(리눅스 커널 전용
// 헤더)에 의존합니다. 로컬 macOS(Qt Creator 등)에는 이 헤더가 없어서, __linux__가
// 아닌 곳에서는 아래 실제 구현 대신 "항상 실패하는 스텁"으로 대체합니다 — 그래야
// mac에서도 ota_core(다른 팀원 로컬 빌드 포함)가 최소한 컴파일은 됩니다. 실제 CC1101
// 동작 검증은 라즈베리파이(Linux) 빌드/실기기에서 해야 합니다.
#if defined(__linux__)
#include "cc1101_ioctl.h"
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

Cc1101Transport::Cc1101Transport(const std::string &devicePath)
    : m_devicePath(devicePath)
{
}

Cc1101Transport::~Cc1101Transport()
{
    close();
}

#if defined(__linux__)
// ================= 리눅스(라즈베리파이) 실구현 =================

Cc1101Status Cc1101Transport::statusFromErrno(int err) const
{
    switch (err) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return Cc1101Status::NoData;
    case ETIMEDOUT:
        return Cc1101Status::Timeout;
    case EBUSY:
        return Cc1101Status::Busy;
    case EINVAL:
        return Cc1101Status::InvalidArg;
    case EMSGSIZE:
        return Cc1101Status::FrameTooLarge;
    case ENODEV:
    case ENXIO:
        return Cc1101Status::HardwareError;
    default:
        return Cc1101Status::IoError;
    }
}

bool Cc1101Transport::open()
{
    if (m_fd >= 0) {
        m_lastStatus = Cc1101Status::Ok;
        return true;
    }

    // O_NONBLOCK: recv()가 패킷이 없을 때 GUI를 멈추지 않도록 한다.
    m_fd = ::open(m_devicePath.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0) {
        m_lastStatus = statusFromErrno(errno);
        return false;
    }

    m_lastStatus = Cc1101Status::Ok;
    return true;
}

void Cc1101Transport::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_lastStatus = Cc1101Status::NotInitialized;
}

bool Cc1101Transport::isOpen() const
{
    return m_fd >= 0;
}

bool Cc1101Transport::send(const std::vector<uint8_t> &data)
{
    if (m_fd < 0) {
        m_lastStatus = Cc1101Status::NotInitialized;
        return false;
    }

    if (data.empty()) {
        m_lastStatus = Cc1101Status::InvalidArg;
        return false;
    }

    const ssize_t written = ::write(m_fd, data.data(), data.size());
    if (written < 0) {
        m_lastStatus = statusFromErrno(errno);
        return false;
    }

    if (static_cast<size_t>(written) != data.size()) {
        m_lastStatus = Cc1101Status::IoError;
        return false;
    }

    m_lastStatus = Cc1101Status::Ok;
    return true;
}

std::vector<uint8_t> Cc1101Transport::recv()
{
    if (m_fd < 0) {
        m_lastStatus = Cc1101Status::NotInitialized;
        return {};
    }

    struct pollfd pfd {};
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    const int pollResult = ::poll(&pfd, 1, 0); // 0 ms: 완전 비동기 확인
    if (pollResult == 0) {
        m_lastStatus = Cc1101Status::NoData;
        return {};
    }

    if (pollResult < 0) {
        m_lastStatus = statusFromErrno(errno);
        return {};
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        m_lastStatus = Cc1101Status::IoError;
        return {};
    }

    // 현재 커널 드라이버의 최대 페이로드보다 넉넉히 잡아 둔다.
    uint8_t buffer[256];
    const ssize_t received = ::read(m_fd, buffer, sizeof(buffer));

    if (received < 0) {
        m_lastStatus = statusFromErrno(errno);
        return {};
    }

    if (received == 0) {
        m_lastStatus = Cc1101Status::NoData;
        return {};
    }

    // 수신 완료 감지 시각. 현재 커널 UAPI가 per-packet timestamp를 직접 주지는
    // 않으므로 userspace가 read() 성공 직후 monotonic 시각을 기록한다.
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        m_lastRxMetadata.rxTimestampUs =
            static_cast<int64_t>(ts.tv_sec) * 1000000LL +
            static_cast<int64_t>(ts.tv_nsec) / 1000LL;
    }

    // RSSI/LQI/CRC는 현재 UAPI의 GET_STATUS로 보조 조회한다.
    // 주의: 커널이 패킷별 메타데이터를 read()와 함께 반환하지 않으므로
    // "정확히 그 패킷"에 결합된 메타데이터는 아니다.
    struct cc1101_status st {};
    if (::ioctl(m_fd, CC1101_IOC_GET_STATUS, &st) == 0) {
        m_lastRxMetadata.rssiDbmX10 = static_cast<int16_t>(st.rssi_dbm) * 10;
        m_lastRxMetadata.lqi = st.lqi;
        m_lastRxMetadata.crcOk = st.crc_ok != 0;
        m_lastRxMetadata.channel = m_channel;
    }

    m_lastStatus = Cc1101Status::Ok;
    return std::vector<uint8_t>(buffer, buffer + received);
}

Cc1101Status Cc1101Transport::setChannel(uint8_t channel)
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    __u8 ch = channel;
    if (::ioctl(m_fd, CC1101_IOC_SET_CHANNEL, &ch) < 0)
        return m_lastStatus = statusFromErrno(errno);

    m_channel = channel;
    return m_lastStatus = Cc1101Status::Ok;
}

Cc1101Status Cc1101Transport::startRx()
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    if (::ioctl(m_fd, CC1101_IOC_SET_RX) < 0)
        return m_lastStatus = statusFromErrno(errno);

    return m_lastStatus = Cc1101Status::Ok;
}

void Cc1101Transport::flushRx()
{
    if (m_fd < 0) {
        m_lastStatus = Cc1101Status::NotInitialized;
        return;
    }

    if (::ioctl(m_fd, CC1101_IOC_FLUSH_RX) < 0) {
        m_lastStatus = statusFromErrno(errno);
        return;
    }

    m_lastStatus = Cc1101Status::Ok;
}

Cc1101Status Cc1101Transport::flushTx()
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    if (::ioctl(m_fd, CC1101_IOC_FLUSH_TX) < 0)
        return m_lastStatus = statusFromErrno(errno);

    return m_lastStatus = Cc1101Status::Ok;
}

// [2026-08-22 추가, FHSS] 우리 쪽 Cc1101FhssConfig(cc1101_status.h, Qt/리눅스
// 헤더 의존 없는 이식성 있는 타입)을 커널 UAPI의 cc1101_fhss_config(리눅스
// 전용, cc1101_ioctl.h)로 그대로 옮겨 담는다. 필드 이름은 다르지만 뜻은
// 1:1로 대응 — cc1101_ioctl.h 상단 주석의 "계약 헤더" 원칙 그대로.
Cc1101Status Cc1101Transport::configureFhss(const Cc1101FhssConfig &config)
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    struct cc1101_fhss_config kernelConfig {};
    kernelConfig.version = CC1101_FHSS_VERSION;
    kernelConfig.size = sizeof(kernelConfig);
    kernelConfig.generation = config.generation;
    kernelConfig.algorithm_id = config.algorithmId;

    kernelConfig.rf.base_freq_hz = config.rfBaseFreqHz;
    kernelConfig.rf.channel_spacing_hz = config.rfChannelSpacingHz;
    kernelConfig.rf.sync_word = config.rfSyncWord;
    kernelConfig.rf.mdmcfg4 = config.rfMdmcfg4;
    kernelConfig.rf.mdmcfg3 = config.rfMdmcfg3;
    kernelConfig.rf.pktctrl1 = config.rfPktctrl1;
    kernelConfig.rf.pktctrl0 = config.rfPktctrl0;

    kernelConfig.hop.seed = config.seed;
    kernelConfig.hop.slot_duration_us = config.slotDurationUs;
    kernelConfig.hop.channel_switch_guard_us = config.channelSwitchGuardUs;
    kernelConfig.hop.channel_count = config.channelCount;
    kernelConfig.hop.first_channel = config.firstChannel;
    kernelConfig.hop.rendezvous_channel = config.rendezvousChannel;
    kernelConfig.hop.reserved_channel = config.reservedChannel;
    kernelConfig.hop.algorithm_version = config.algorithmVersion;
    kernelConfig.hop.channel_profile_id = config.channelProfileId;

    if (::ioctl(m_fd, CC1101_IOC_FHSS_SET_CONFIG, &kernelConfig) < 0)
        return m_lastStatus = statusFromErrno(errno);

    return m_lastStatus = Cc1101Status::Ok;
}

Cc1101Status Cc1101Transport::startFhss(Cc1101FhssRole role)
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    __u8 kernelRole = static_cast<__u8>(role);
    if (::ioctl(m_fd, CC1101_IOC_FHSS_START, &kernelRole) < 0)
        return m_lastStatus = statusFromErrno(errno);

    return m_lastStatus = Cc1101Status::Ok;
}

Cc1101Status Cc1101Transport::stopFhss()
{
    if (m_fd < 0)
        return m_lastStatus = Cc1101Status::NotInitialized;

    if (::ioctl(m_fd, CC1101_IOC_FHSS_STOP) < 0)
        return m_lastStatus = statusFromErrno(errno);

    return m_lastStatus = Cc1101Status::Ok;
}

// [설계 메모] 이 함수는 "지금 호핑 중인지" 우리가 따로 기억해둔 값이 아니라
// 매번 커널에 새로 물어본 값을 돌려준다 — Cc1101Transport가 로컬 상태를
// 별도로 들고 있다가 실제 커널 상태와 어긋나는 걸 막기 위함
// (design-notes-gateway-ota-es.md, FHSS 항목 참고).
Cc1101FhssStatus Cc1101Transport::getFhssStatus()
{
    Cc1101FhssStatus status;
    if (m_fd < 0) {
        m_lastStatus = Cc1101Status::NotInitialized;
        return status;
    }

    struct cc1101_fhss_status kernelStatus {};
    if (::ioctl(m_fd, CC1101_IOC_FHSS_GET_STATUS, &kernelStatus) < 0) {
        m_lastStatus = statusFromErrno(errno);
        return status;
    }

    status.enabled = kernelStatus.enabled != 0;
    status.synchronized = kernelStatus.synchronized != 0;
    status.currentChannel = kernelStatus.current_channel;
    status.role = kernelStatus.role;
    status.generation = kernelStatus.generation;
    status.currentSlot = kernelStatus.current_slot;
    status.lastError = kernelStatus.last_error;
    status.syncMisses = kernelStatus.sync_misses;
    status.syncPackets = kernelStatus.sync_packets;

    m_lastStatus = Cc1101Status::Ok;
    return status;
}

#else
// ================= 비-리눅스(macOS 등 로컬 개발) 폴백 스텁 =================
// 위 [플랫폼 가드] 설명 참고 — 여기 내려오면 항상 "미초기화" 취급으로 실패 반환만 함.

Cc1101Status Cc1101Transport::statusFromErrno(int) const
{
    return Cc1101Status::NotInitialized;
}

bool Cc1101Transport::open()
{
    m_lastStatus = Cc1101Status::NotInitialized;
    return false;
}

void Cc1101Transport::close()
{
    m_fd = -1;
    m_lastStatus = Cc1101Status::NotInitialized;
}

bool Cc1101Transport::isOpen() const
{
    return false;
}

bool Cc1101Transport::send(const std::vector<uint8_t> &)
{
    m_lastStatus = Cc1101Status::NotInitialized;
    return false;
}

std::vector<uint8_t> Cc1101Transport::recv()
{
    m_lastStatus = Cc1101Status::NotInitialized;
    return {};
}

Cc1101Status Cc1101Transport::setChannel(uint8_t)
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::startRx()
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

void Cc1101Transport::flushRx()
{
    m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::flushTx()
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::configureFhss(const Cc1101FhssConfig &)
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::startFhss(Cc1101FhssRole)
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::stopFhss()
{
    return m_lastStatus = Cc1101Status::NotInitialized;
}

Cc1101FhssStatus Cc1101Transport::getFhssStatus()
{
    m_lastStatus = Cc1101Status::NotInitialized;
    return Cc1101FhssStatus{};
}

#endif // defined(__linux__)
