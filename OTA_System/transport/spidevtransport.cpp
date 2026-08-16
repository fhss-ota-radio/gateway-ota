#include "spidevtransport.h"

#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

SpidevTransport::SpidevTransport(const std::string &devicePath)
    : m_devicePath(devicePath)
{
}

SpidevTransport::~SpidevTransport()
{
    close();
}

#if defined(__linux__)
// ================= 리눅스 실구현 (spidev로 CC1101 직접 제어) =================

namespace {

constexpr uint32_t kSpiSpeedHz = 1000000; // 1MHz, 안전하게 낮춘 값
constexpr uint8_t kSpiBits = 8;

// CC1101 레지스터/스트로브 주소 (kernel-cc1101-spi의 cc1101.h와 동일한 값)
constexpr uint8_t kAddrIOCFG2 = 0x00;
constexpr uint8_t kAddrPATABLE = 0x3E;
constexpr uint8_t kAddrFIFO = 0x3F;   // TXFIFO/RXFIFO 같은 주소 공유
constexpr uint8_t kAddrTXBYTES = 0x3A;
constexpr uint8_t kAddrRXBYTES = 0x3B;

constexpr uint8_t kStrobeSRES = 0x30;
constexpr uint8_t kStrobeSCAL = 0x33;
constexpr uint8_t kStrobeSRX = 0x34;
constexpr uint8_t kStrobeSTX = 0x35;
constexpr uint8_t kStrobeSIDLE = 0x36;
constexpr uint8_t kStrobeSFRX = 0x3A; // strobe 주소와 TXBYTES 상태레지스터가 같은 0x3A를 공유(burst비트로 구분)
constexpr uint8_t kStrobeSFTX = 0x3B; // TX FIFO flush. strobe 주소와 RXBYTES 상태레지스터가 같은 0x3B를 공유

constexpr uint8_t kReadBurst = 0xC0;
constexpr uint8_t kWriteBurst = 0x40;

constexpr int kMaxPacketBody = 60; // OTA_RF_PACKET_BODY_MAX_SIZE (ota_protocol.h)

// kernel-cc1101-spi/cc1101_core.c의 cc1101_default_regs[]와 동일한 값
// (433.92MHz / 2-FSK / 38.4kbps). 딱 하나, PKTCTRL1만 0x0D -> 0x0C로 바꿔서
// 주소필터(ADR_CHK)를 껐음 (spidevtransport.h 상단 주석 참고).
constexpr uint8_t kDefaultRegs[47] = {
    0x07, 0x2E, 0x06, 0x47, 0xD3, 0x91, 0x3D, 0x0C, 0x05, 0x00, 0x00, 0x06, 0x00,
    0x10, 0xB0, 0x71, 0xCA, 0x83, 0x13, 0x22, 0xF8, 0x35, 0x07, 0x3F, 0x18, 0x16,
    0x6C, 0x43, 0x40, 0x91, 0x87, 0x6B, 0xFB, 0x56, 0x10, 0xE9, 0x2A, 0x00, 0x1F,
    0x41, 0x00, 0x59, 0x7F, 0x3F, 0x81, 0x35, 0x09};

} // namespace

void SpidevTransport::xfer(uint8_t *tx, uint8_t *rx, int len)
{
    struct spi_ioc_transfer tr {};
    tr.tx_buf = reinterpret_cast<unsigned long>(tx);
    tr.rx_buf = reinterpret_cast<unsigned long>(rx);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = kSpiSpeedHz;
    tr.bits_per_word = kSpiBits;
    ::ioctl(m_fd, SPI_IOC_MESSAGE(1), &tr);
}

void SpidevTransport::strobe(uint8_t cmd)
{
    uint8_t tx[1] = {cmd};
    uint8_t rx[1];
    xfer(tx, rx, 1);
}

uint8_t SpidevTransport::readStatusReg(uint8_t addr)
{
    uint8_t tx[2] = {static_cast<uint8_t>((addr & 0x3F) | kReadBurst), 0};
    uint8_t rx[2];
    xfer(tx, rx, 2);
    return rx[1];
}

bool SpidevTransport::open()
{
    if (m_fd >= 0)
        return true;

    m_fd = ::open(m_devicePath.c_str(), O_RDWR);
    if (m_fd < 0)
        return false;

    uint8_t mode = SPI_MODE_0;
    uint32_t speed = kSpiSpeedHz;
    uint8_t bits = kSpiBits;
    ::ioctl(m_fd, SPI_IOC_WR_MODE, &mode);
    ::ioctl(m_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
    ::ioctl(m_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);

    // kernel-cc1101-spi의 cc1101_hw_reset()+cc1101_load_default_config()에
    // 해당하는 절차를 유저공간에서 그대로 재현.
    strobe(kStrobeSRES);
    ::usleep(1000);
    strobe(kStrobeSIDLE);

    uint8_t buf[48];
    buf[0] = kAddrIOCFG2 | kWriteBurst;
    std::memcpy(&buf[1], kDefaultRegs, sizeof(kDefaultRegs));
    xfer(buf, buf, sizeof(buf));

    uint8_t pa[2] = {static_cast<uint8_t>(kAddrPATABLE | kWriteBurst), 0xC0};
    xfer(pa, pa, 2);

    strobe(kStrobeSCAL);
    ::usleep(1000);

    strobe(kStrobeSRX);
    return true;
}

void SpidevTransport::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool SpidevTransport::isOpen() const
{
    return m_fd >= 0;
}

bool SpidevTransport::send(const std::vector<uint8_t> &data)
{
    if (m_fd < 0 || data.empty() || data.size() > static_cast<size_t>(kMaxPacketBody))
        return false;

    // [버그 수정, 2026-08-14] open()이 마지막에 RX 상태로 들어가 있는데,
    // RX 상태에서 곧바로 TXFIFO에 쓰고 STX를 날리면 제대로 안 실릴 수 있음
    // (실기기 테스트에서 확인 — 핸드셰이크가 계속 응답 없음으로 실패했음).
    // 실제 커널 드라이버(cc1101_write())와 동일하게, 반드시 먼저 IDLE로
    // 간 다음 TX FIFO를 한번 비우고(SFTX) 나서 새 데이터를 써야 함.
    strobe(kStrobeSIDLE);
    strobe(kStrobeSFTX);

    std::vector<uint8_t> pkt;
    pkt.reserve(data.size() + 2);
    pkt.push_back(kAddrFIFO | kWriteBurst);
    pkt.push_back(static_cast<uint8_t>(data.size()));
    pkt.insert(pkt.end(), data.begin(), data.end());
    xfer(pkt.data(), pkt.data(), static_cast<int>(pkt.size()));

    strobe(kStrobeSTX);
    for (int i = 0; i < 100; ++i) {
        ::usleep(2000);
        if ((readStatusReg(kAddrTXBYTES) & 0x7F) == 0)
            break;
    }
    // [버그 수정, 2026-08-14] TXBYTES==0은 "FIFO 비움"이지 "전파로 실제 다
    // 나감"이 아님 — 마지막 몇 바이트가 아직 공중으로 나가는 중일 수 있음.
    // 이 타이밍에 SRX를 수동으로 강제하면 칩이 애매한 상태에 빠질 수 있어서
    // (실기기 테스트에서 이 강제 SRX 이후 수신이 전혀 안 되는 문제 발견),
    // 수동 강제 대신 MCSM1(TXOFF_MODE=11)의 자동 RX 복귀를 믿고 여유시간만
    // 살짝 준다.
    ::usleep(3000);
    return true;
}

std::vector<uint8_t> SpidevTransport::recv()
{
    if (m_fd < 0)
        return {};

    const uint8_t rxbytes = readStatusReg(kAddrRXBYTES);
    if (rxbytes & 0x7F) // [임시 디버그, 문제 해결되면 지울 것]
        std::fprintf(stderr, "[spidev DEBUG] RXBYTES=0x%02x\n", rxbytes);
    if (rxbytes & 0x80) { // RX FIFO overflow
        strobe(kStrobeSIDLE);
        strobe(kStrobeSFRX);
        strobe(kStrobeSRX);
        return {};
    }
    if ((rxbytes & 0x7F) == 0)
        return {}; // 아직 아무 것도 안 옴 (ITransport::recv() 계약: 없으면 빈 벡터)

    uint8_t lenTx[2] = {static_cast<uint8_t>(kAddrFIFO | kReadBurst), 0};
    uint8_t lenRx[2];
    xfer(lenTx, lenRx, 2);
    const uint8_t len = lenRx[1];

    std::vector<uint8_t> result;
    if (len > 0 && len <= kMaxPacketBody) {
        // 헤더에코(1) + 데이터(len) + RSSI(1) + LQI/CRC(1)
        std::vector<uint8_t> buf(static_cast<size_t>(len) + 3, 0);
        buf[0] = kAddrFIFO | kReadBurst;
        xfer(buf.data(), buf.data(), static_cast<int>(buf.size()));
        result.assign(buf.begin() + 1, buf.begin() + 1 + len);
        // CRC_AUTOFLUSH=1이라 CRC 실패 패킷은 애초에 FIFO에 안 들어오므로
        // 여기 도달했다면 CRC는 이미 통과한 것으로 간주해도 됨.
    }

    strobe(kStrobeSIDLE);
    strobe(kStrobeSFRX);
    strobe(kStrobeSRX);
    return result;
}

#else
// ================= 비-리눅스(macOS 등 로컬 개발) 폴백 스텁 =================

bool SpidevTransport::open() { return false; }
void SpidevTransport::close() { m_fd = -1; }
bool SpidevTransport::isOpen() const { return false; }
bool SpidevTransport::send(const std::vector<uint8_t> &) { return false; }
std::vector<uint8_t> SpidevTransport::recv() { return {}; }

#endif // defined(__linux__)
