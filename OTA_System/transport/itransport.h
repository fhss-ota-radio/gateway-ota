#ifndef ITRANSPORT_H
#define ITRANSPORT_H

#include <QByteArray>

// 전송 계층 추상화 (마일스톤 2).
// 상위 로직(BinSplitter가 만든 청크, 앞으로 만들 재전송 큐, OtaManager 화면)은
// 이 인터페이스만 보고 동작하고, "로컬 파일에 쓰기"든 "CC1101로 무선 전송"이든
// 실제 구현체만 갈아끼우면 되도록 분리해둔 것.
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

    // 직렬화된 청크(OtaChunk::serialized() 결과) 하나를 보냄. 성공 여부 반환.
    virtual bool send(const QByteArray &data) = 0;
    // 수신 가능한 데이터가 있으면 읽어서 반환, 없으면 빈 QByteArray.
    virtual QByteArray recv() = 0;
};

#endif // ITRANSPORT_H
