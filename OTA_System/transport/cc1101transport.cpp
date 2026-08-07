#include "cc1101transport.h"

// TODO(부품 입고 후): 실제 구현은 아래 POSIX 헤더들을 이용해서 /dev/cc1101에 접근한다.
//   #include <fcntl.h>     // ::open, O_RDWR
//   #include <unistd.h>    // ::close, ::write, ::read
//   #include <poll.h>      // ::poll  (수신 대기, Qt의 QSocketNotifier와 연동 예정)
//   #include <sys/ioctl.h> // ::ioctl (채널 변경/RX 시작/FIFO 초기화 등)
// 지금은 하드웨어와 커널 드라이버가 없으므로, 항상 실패/빈 값을 돌려주는 스텁으로 둔다.
// (그래야 화면 쪽 "CC1101" 드라이버 옵션을 골라도 앱이 죽지 않고 실패 로그만 남음)

Cc1101Transport::Cc1101Transport(const std::string &devicePath)
    : m_devicePath(devicePath)
{
}

Cc1101Transport::~Cc1101Transport()
{
    close();
}

bool Cc1101Transport::open()
{
    // TODO: m_fd = ::open(m_devicePath.c_str(), O_RDWR);
    m_lastStatus = Cc1101Status::NotInitialized;
    return false;
}

void Cc1101Transport::close()
{
    // TODO: if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

bool Cc1101Transport::isOpen() const
{
    return m_fd >= 0;
}

bool Cc1101Transport::send(const std::vector<uint8_t> &data)
{
    (void)data;
    // TODO: ::write(m_fd, data.data(), data.size())
    m_lastStatus = Cc1101Status::NotInitialized;
    return false;
}

std::vector<uint8_t> Cc1101Transport::recv()
{
    // TODO: ::poll()로 읽기 가능 여부 확인 후 ::read(m_fd, ...) + m_lastRxMetadata 채우기
    m_lastStatus = Cc1101Status::NotInitialized;
    return {};
}

Cc1101Status Cc1101Transport::setChannel(uint8_t channel)
{
    (void)channel;
    // TODO: ::ioctl(m_fd, CC1101_IOC_SET_CHANNEL, &channel)
    return Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::startRx()
{
    // TODO: ::ioctl(m_fd, CC1101_IOC_START_RX)
    return Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::flushRx()
{
    // TODO: ::ioctl(m_fd, CC1101_IOC_FLUSH_RX)
    return Cc1101Status::NotInitialized;
}

Cc1101Status Cc1101Transport::flushTx()
{
    // TODO: ::ioctl(m_fd, CC1101_IOC_FLUSH_TX)
    return Cc1101Status::NotInitialized;
}
