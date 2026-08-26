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
    //                      ^^^^  ^^^^  = SYNC1, SYNC0
    // [변경 2026-08-16] 싱크워드를 TI 레퍼런스 기본값(0xD3/0x91)에서 OTA 전용
    // 값(0x2D/0xD4)으로 분리. 같은 프로젝트의 다른 팀 장비들도 433.92MHz +
    // 같은 레퍼런스 설정을 써서 싱크워드가 전부 겹쳐 있었고, 그래서 우리
    // 수신기가 남의 패킷("FHSS"=0x46485353로 시작하는 것, 0xA5로 시작하는
    // 49byte짜리 등)까지 CRC 통과시켜 받아버렸음. CC1101은 싱크워드가 다르면
    // 하드웨어 단에서 아예 무시하므로(인터럽트도 안 울림) 이게 가장 깨끗한
    // 격리 방법.
    // [중요] kernel-cc1101-spi의 cc1101_core.c에 있는 cc1101_default_regs[]와
    // 반드시 같은 값이어야 함 — 한쪽만 바꾸면 두 경로가 서로 통신 못 함.
    // 인덱스 10 = CHANNR = 0x00 (채널 0, 기준 주파수 433.92MHz 그대로).
    // 채널 이동으로 다른 팀과 물리적으로 분리하는 것도 검토했으나 싱크워드
    // 분리로 충분하다고 판단해 0 유지. 나중에 옮길 일이 있으면 국내 433MHz
    // ISM 밴드(433.05~434.79MHz) 때문에 채널 4가 상한이라는 점 주의 —
    // 자세한 계산은 kernel-cc1101-spi/cc1101_core.c의 CHANNR 주석 참고.
    0x07, 0x2E, 0x06, 0x47, 0x2D, 0xD4, 0x3D, 0x0C, 0x05, 0x00, 0x00, 0x06, 0x00,
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
    if (rxbytes & 0x80) { // RX FIFO overflow
        strobe(kStrobeSIDLE);
        strobe(kStrobeSFRX);
        strobe(kStrobeSRX);
        return {};
    }
    if ((rxbytes & 0x7F) == 0)
        return {}; // 아직 아무 것도 안 옴 (ITransport::recv() 계약: 없으면 빈 벡터)

    // [버그 수정 2/2, 2026-08-16] "수신이 끝난 뒤에" 읽기 시작한다.
    //
    // RXBYTES가 0이 아니라는 건 "패킷이 도착하기 시작했다"일 뿐, "다 도착했다"가
    // 아니다. 가변길이 모드에서는 첫 바이트가 길이바이트인데, 도착 중인 상태에서
    // 성급히 읽으면 그 1바이트가 아직 FIFO에 안정적으로 올라오지 않아 엉뚱한
    // 값을 길이로 읽고, 이후 본문 전체가 1바이트씩 밀려버린다.
    //
    // 실기기 증상(2026-08-16): 깨진 패킷들이 하나같이 길이바이트(0x3C=60)가
    // 페이로드 앞에 그대로 붙은 채 "3C 02 EE BE 07 51..." 형태로 올라왔고,
    // 그 결과 session_id가 0x5107beee -> 0x07beeeee처럼 한 바이트씩 시프트됨.
    //
    // 해결: RXBYTES가 더 이상 늘어나지 않을 때(= 패킷 하나가 다 들어왔을 때)
    // 까지 기다렸다가 읽는다. 우리 패킷은 전부 64byte FIFO 안에 들어가는
    // 크기라 이 방식이 안전하다.
    {
        uint8_t prev = rxbytes & 0x7F;
        for (int i = 0; i < 40; ++i) {      // 최대 ~20ms
            ::usleep(500);
            const uint8_t now = readStatusReg(kAddrRXBYTES);
            if (now & 0x80) {               // 대기 중 오버플로
                strobe(kStrobeSIDLE);
                strobe(kStrobeSFRX);
                strobe(kStrobeSRX);
                return {};
            }
            if ((now & 0x7F) == prev)
                break;                      // 안 늘어남 = 수신 완료
            prev = now & 0x7F;
        }
    }

    uint8_t lenTx[2] = {static_cast<uint8_t>(kAddrFIFO | kReadBurst), 0};
    uint8_t lenRx[2];
    xfer(lenTx, lenRx, 2);
    const uint8_t len = lenRx[1];

    std::vector<uint8_t> result;
    if (len > 0 && len <= kMaxPacketBody) {
        // [버그 수정, 2026-08-16] 패킷 전체가 FIFO에 도착할 때까지 기다린다.
        //
        // 가변길이(variable length) 모드에서 RX FIFO는 무선으로 바이트가
        // 들어오는 대로 조금씩 채워진다. 즉 RXBYTES는 수신 도중 계속 늘어난다.
        // 그런데 이전 코드는 RXBYTES가 0만 아니면 곧바로 길이바이트를 읽고
        // 이어서 본문 len개를 통째로 읽어버렸다 — 아직 도착하지 않은 부분까지
        // 읽으려 하면 FIFO 언더플로(underflow)가 나서 CC1101이 쓰레기 값을
        // 돌려주고, 그게 패킷에 섞여 디코딩이 깨진다.
        //
        // 실기기 증상(2026-08-16): RXBYTES=0x05(5바이트만 도착)인데 길이바이트는
        // 49로 읽혀서 49바이트를 마저 읽어버림 -> target_device_id가
        // 0xffffffff여야 하는데 0xfffffff9로, imageSize/totalChunks도 엉뚱한
        // 값으로 깨져서 올라옴. (design-notes 17절에서 "SPI 트랜잭션 3번으로
        // 나뉘어 있어 타이밍이 어긋날 수 있다"고 원인 추정만 해뒀던 그 버그)
        //
        // 해결: 길이를 알았으니 "본문(len) + 상태바이트(RSSI, LQI/CRC 2개)"가
        // 전부 FIFO에 들어올 때까지 기다렸다가 읽는다. 48byte짜리 최대 패킷도
        // 38.4kbps에서 ~13ms면 다 들어오므로 50ms 타임아웃이면 충분하다.
        constexpr int kStatusBytes = 2;               // APPEND_STATUS=1 -> RSSI, LQI/CRC
        const int needed = static_cast<int>(len) + kStatusBytes;
        bool complete = false;
        for (int i = 0; i < 50; ++i) {
            const uint8_t now = readStatusReg(kAddrRXBYTES);
            if (now & 0x80)                            // 대기 중 오버플로 -> 폐기
                break;
            if ((now & 0x7F) >= needed) {
                complete = true;
                break;
            }
            ::usleep(1000);
        }

        if (complete) {
            // 헤더에코(1) + 데이터(len) + RSSI(1) + LQI/CRC(1)
            std::vector<uint8_t> buf(static_cast<size_t>(len) + 3, 0);
            buf[0] = kAddrFIFO | kReadBurst;
            xfer(buf.data(), buf.data(), static_cast<int>(buf.size()));
            result.assign(buf.begin() + 1, buf.begin() + 1 + len);
            // CRC_AUTOFLUSH=1이라 CRC 실패 패킷은 애초에 FIFO에 안 들어오므로
            // 여기 도달했다면 CRC는 이미 통과한 것으로 간주해도 됨.
        } else {
            std::fprintf(stderr,
                         "[spidev] 패킷 미완성 폐기 (len=%u, 대기 타임아웃)\n",
                         static_cast<unsigned>(len));
        }
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
