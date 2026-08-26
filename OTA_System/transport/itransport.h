#ifndef ITRANSPORT_H
#define ITRANSPORT_H

#include <cstdint>
#include <vector>

// 전송 계층 추상화 (마일스톤 2).
// 상위 로직(BinSplitter가 만든 청크, 앞으로 만들 재전송 큐, OtaManager 화면)은
// 이 인터페이스만 보고 동작하고, "로컬 파일에 쓰기"든 "CC1101로 무선 전송"이든
// 실제 구현체만 갈아끼우면 되도록 분리해둔 것.
//
// [의도적으로 Qt 의존성 없음] core/binsplitter와 같은 이유로 std::vector<uint8_t>를
// 씀 — OtaChunk::packet(core/binsplitter.h)이 이미 std::vector<uint8_t>라서 타입이
// 그대로 맞아떨어짐. 화면(Qt)에서 호출할 땐 QByteArray로 감싸는 얇은 변환이 필요하며,
// 그 변환은 화면 코드(otamanager.cpp) 쪽 책임입니다.
class ITransport
{
public:
    virtual ~ITransport() = default;

    // 연결을 열고 성공 여부를 반환 (예: /dev/cc1101 open)
    virtual bool open() = 0;
    // 연결을 닫음
    virtual void close() = 0;
    // 현재 연결된 상태인지
    virtual bool isOpen() const = 0;

    // 직렬화된 청크(OtaChunk::packet) 하나를 보냄. 성공 여부 반환.
    virtual bool send(const std::vector<uint8_t> &data) = 0;
    // 수신 가능한 데이터가 있으면 읽어서 반환, 없으면 빈 벡터.
    virtual std::vector<uint8_t> recv() = 0;

    // [2026-08-19 추가] 수신 버퍼를 강제로 비움. CC1101처럼 실제 하드웨어
    // RX FIFO를 가진 구현체(Cc1101Transport)는 이걸로 RXFIFO_OVERFLOW 같은
    // "쌓여서 막힌" 상태를 복구합니다(kernel-cc1101-spi 8/18 디버깅에서
    // 확인된 것과 같은 계열의 문제 — SET_RX만으로는 안 풀리고 FLUSH_RX로
    // FIFO를 강제로 비워야 회복됨). FakeTransport처럼 실제 FIFO가 없는
    // 구현체는 아무 것도 할 필요가 없어서 기본 구현(빈 몸통)을 둡니다 —
    // 그런 구현체는 override 안 해도 됨.
    virtual void flushRx() {}
};

#endif // ITRANSPORT_H
