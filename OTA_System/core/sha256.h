#ifndef SHA256_H
#define SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

// SHA-256 계산 유틸리티.
//
// [왜 필요한가] OtaSession이 OTA_START 패킷에 파일의 SHA256을 실어 보내야
// ESP32 수신측(firmware-esp32의 ota_writer_finish())이 전송 완료 후 무결성을
// 검증할 수 있다. 지금까지는 이 필드를 0으로만 채워서 보냈는데(수신측이
// 라즈베리파이일 땐 검증을 안 해서 문제가 없었음), ESP32는 실제로 이 값을
// 계산해서 비교하므로 반드시 진짜 값을 넣어야 한다 — 안 그러면 데이터
// 전송은 전부 성공하고도 마지막 OTA_END에서 항상 거절당한다(2026-08-19
// 발견, docs/roadmap.md 3절 참고).
//
// [의도적으로 외부 라이브러리 의존 없음] OpenSSL/mbedtls 같은 게 라즈베리파이
// Yocto 이미지에 항상 있다는 보장이 없고(이 샌드박스에도 없었음), core/ 전체가
// "g++/clang++만으로 컴파일 가능"이라는 원칙을 지키고 있어서(다른 core/·
// transport/ 파일과 같은 이유, design-notes 14절 참고) 공개 도메인 SHA-256
// 구현을 그대로 가져와 최소 형태로 넣었다. Qt 의존성도 없음.
class Sha256
{
public:
    Sha256();

    // 데이터를 몇 번에 나눠서 넣어도 결과는 한 번에 넣은 것과 같다
    // (파일을 통째로 메모리에 올리지 않고 스트리밍으로 처리하기 위함).
    void update(const uint8_t *data, size_t len);

    // 32byte 해시값을 outHash에 채운다. 이후 이 객체를 재사용하지 않는다
    // (필요하면 새로 만들 것 — 재시작 로직은 안 만들어둠, 지금 쓰임새가
    // "파일 하나 통째로 한 번 계산"뿐이라 불필요).
    void finish(uint8_t outHash[32]);

private:
    void transform(const uint8_t chunk[64]);

    uint32_t m_state[8];
    uint64_t m_bitLength;
    uint8_t m_buffer[64];
    size_t m_bufferLength;
};

// 파일 전체를 스트리밍으로 읽어 SHA-256을 계산한다(64KB 버퍼 단위 — BinSplitter가
// 청크 낼 때처럼 파일 전체를 한 번에 메모리에 올리지 않음).
//
// 실패(파일 열기 실패 등)하면 false를 반환하고 errorMessage(NULL 아니면)에
// 이유를 채운다. 성공하면 outHash[32]에 결과가 채워진다.
bool sha256File(const std::string &filePath, uint8_t outHash[32], std::string *errorMessage);

#endif // SHA256_H
