#ifndef CC1101TRANSPORT_H
#define CC1101TRANSPORT_H

#include "cc1101_status.h"
#include "itransport.h"

#include <cstdint>
#include <string>

class Cc1101Transport : public ITransport
{
public:
    explicit Cc1101Transport(const std::string &devicePath = "/dev/cc1101");
    ~Cc1101Transport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;

    bool send(const std::vector<uint8_t> &data) override;
    std::vector<uint8_t> recv() override;

    Cc1101Status setChannel(uint8_t channel);
    Cc1101Status startRx();
    Cc1101Status flushRx();
    Cc1101Status flushTx();

    Cc1101RxMetadata lastRxMetadata() const { return m_lastRxMetadata; }
    Cc1101Status lastStatus() const { return m_lastStatus; }

private:
    Cc1101Status statusFromErrno(int err) const;

    std::string m_devicePath;
    int m_fd = -1;
    uint8_t m_channel = 0;
    Cc1101Status m_lastStatus = Cc1101Status::NotInitialized;
    Cc1101RxMetadata m_lastRxMetadata;
};

#endif // CC1101TRANSPORT_H
