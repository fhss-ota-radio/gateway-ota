#ifndef CC1101TRANSPORT_H
#define CC1101TRANSPORT_H

#include "cc1101_status.h"
#include "itransport.h"

#include <QString>

// ITransport의 CC1101 구현체.
//
// 실제 SPI/레지스터 제어는 팀원 4·5의 커널 드라이버가 담당하고, 여기서는 그 드라이버가
// 만드는 /dev/cc1101 캐릭터 디바이스 노드를 open/write/poll+read/ioctl로 다루는
// "사용자 공간 래퍼"만 구현한다 (cc1101-radio-api.md 9절 매핑 표 기준):
//   채널 변경 -> ioctl() / RX 시작 -> ioctl() / 송신 -> write()
//   수신 -> poll() + read() / FIFO 초기화 -> ioctl()
//
// TODO(부품 입고 후): 지금은 하드웨어·드라이버가 없어 open()이 항상 실패하는 스텁 상태.
// CC1101 커널 드라이버가 준비되면 open()/send()/recv()/setChannel() 등을 실제
// POSIX 시스템콜(::open, ::write, ::poll, ::read, ::ioctl)로 채운다.
class Cc1101Transport : public ITransport
{
public:
    explicit Cc1101Transport(const QString &devicePath = QStringLiteral("/dev/cc1101"));
    ~Cc1101Transport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;

    bool send(const QByteArray &data) override;
    QByteArray recv() override;

    // cc1101-radio-api.md 5절 공통 API 대응 (라디오 API 담당 영역 — 팀원 4·5 드라이버가 실제 처리)
    Cc1101Status setChannel(quint8 channel);
    Cc1101Status startRx();
    Cc1101Status flushRx();
    Cc1101Status flushTx();

    // 마지막 recv() 성공 시 RSSI/LQI/CRC/수신시각 (cc1101-radio-api.md 4절)
    Cc1101RxMetadata lastRxMetadata() const { return m_lastRxMetadata; }
    // 마지막으로 실행한 동작의 상태값 (실패 원인 파악용)
    Cc1101Status lastStatus() const { return m_lastStatus; }

private:
    QString m_devicePath;
    int m_fd = -1;
    Cc1101Status m_lastStatus = Cc1101Status::NotInitialized;
    Cc1101RxMetadata m_lastRxMetadata;
};

#endif // CC1101TRANSPORT_H
