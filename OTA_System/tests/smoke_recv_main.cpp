// 실기기(CC1101) 스모크테스트 — 단순 수신 CLI (스텁).
//
// "스텁"(stub, 나중에 채울 임시 뼈대)인 이유: 받은 패킷을 콘솔에 보여주기만
// 하고, 보낸 쪽에 ACK/NACK을 돌려보내지 않습니다. 실제 프로토콜 대화(ACK
// 응답, 재전송 요청)는 나중에 만들 OtaSession(FSM)이 담당할 영역이고,
// 지금은 "송신측(ota_smoke_send)이 실제로 뭔가를 보내면 여기서 보이는지"만
// 확인하는 목적입니다.
//
// tst_binsplitter.cpp(ctest로 자동 실행)와 달리 실제 /dev/cc1101 디바이스가
// 있어야 동작하는 수동 실행 도구라서 add_test()에 등록하지 않습니다.
//
// 핵심 수신/디코딩 로직은 session/simplereceiver.h의 tryReceiveOnce()에
// 있고, 이 파일은 그 함수를 반복 호출하며 콘솔에 출력만 하는 얇은 CLI
// 진입점입니다 — smoke_send_main.cpp와 같은 구조.
//
// 사용법:
//   ota_smoke_recv <device_path>
//
//   Ctrl+C로 종료할 때까지 계속 수신 대기하며, 패킷이 도착할 때마다
//   종류와 필드를 콘솔에 출력합니다.

#include "cc1101transport.h"
#include "sha256.h"          // sha256File() — 재조립 결과 자동 검증용
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char *kindToString(ReceivedPacketKind kind)
{
    switch (kind) {
    case ReceivedPacketKind::Start:       return "OTA_START";
    case ReceivedPacketKind::Data:        return "OTA_DATA";
    case ReceivedPacketKind::End:         return "OTA_END";
    case ReceivedPacketKind::Ack:         return "OTA_ACK";
    case ReceivedPacketKind::Nack:        return "OTA_NACK";
    case ReceivedPacketKind::Discover:    return "OTA_DISCOVER";
    case ReceivedPacketKind::DiscoverAck: return "OTA_DISCOVER_ACK";
    case ReceivedPacketKind::Unknown:
    default:
        return "UNKNOWN";
    }
}

void printHex(const std::vector<uint8_t> &data)
{
    for (uint8_t byte : data)
        std::printf("%02X ", byte);
    std::printf("\n");
}

// SHA256 32byte를 소문자 hex 문자열로 (sha256sum 출력 형식과 맞춤 — 눈으로
// 비교할 때 바로 대조할 수 있게).
std::string sha256ToHex(const uint8_t hash[32])
{
    char buf[65];
    for (int i = 0; i < 32; ++i)
        std::snprintf(buf + i * 2, 3, "%02x", hash[i]);
    return std::string(buf, 64);
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path> [저장할_파일]\n"
                  << "  예: " << argv[0] << " /dev/cc1101\n"
                  << "      " << argv[0] << " /dev/cc1101 recv.bin\n"
                  << "\n"
                  << "  저장할_파일을 주면 받은 DATA를 sequence 순서대로 재조립해서\n"
                  << "  그 파일에 씁니다. 재조립이 끝나면(누락 0개) OTA_START에 실려온\n"
                  << "  SHA256과 직접 비교해서 일치 여부를 자동으로 출력합니다.\n";
        return 1;
    }

    const std::string devicePath = argv[1];

    // [추가 2026-08-16] 파일 재조립 기능.
    //
    // 그동안 이 프로그램은 로그만 찍어서, "1067개 전부 받았다"는 건 알아도
    // "내용이 원본과 같은지"는 확인할 수 없었다. CRC는 패킷 단위 검사일 뿐
    // 파일 전체가 올바르게 복원됐는지는 보장하지 않는다(순서 뒤바뀜, 중복,
    // 특정 청크만 유실 등은 CRC를 다 통과하고도 파일을 깨뜨릴 수 있다).
    //
    // sequence 값을 그대로 파일 오프셋으로 써서(seq * OTA_MAX_PAYLOAD_SIZE)
    // 순서와 무관하게 제자리에 기록한다 — 패킷이 뒤바뀌어 도착해도 결과는
    // 같고, 유실된 구간은 0으로 남으므로 어디가 빠졌는지도 드러난다.
    const std::string outPath = (argc >= 3) ? argv[2] : std::string();
    std::fstream outFile;
    std::vector<bool> seqSeen;      // 중복/누락 판정용
    uint32_t expectedChunks = 0;
    uint32_t writtenChunks = 0;
    uint32_t duplicateChunks = 0;
    uint8_t expectedSha256[32] = {}; // START에 실려온 값 — END에서 재계산해 비교
    uint32_t currentSessionId = 0;   // START에서 저장 — CRC 오류 NACK을 지금
                                      // 진행 중인 세션에 대해서만 보내기 위함
                                      // (엉뚱한 노이즈까지 NACK 보내지 않게)

    Cc1101Transport transport(devicePath);
    if (!transport.open()) {
        std::cerr << "[smoke_recv] transport open 실패: " << devicePath << "\n";
        return 1;
    }
    if (transport.startRx() != Cc1101Status::Ok)
        std::cerr << "[smoke_recv] startRx 실패 — 그래도 수신 대기는 계속 시도함\n";

    std::cout << "[smoke_recv] 수신 대기 시작 (device=" << devicePath << ", Ctrl+C로 종료)\n";

    uint32_t dataCount = 0;
    uint32_t totalChunksHint = 0;

    while (true) {
        const auto packet = tryReceiveOnce(transport);

        // 아직 아무것도 안 왔음 — 에러 아님, 잠깐 쉬고 다시 확인
        if (packet.kind == ReceivedPacketKind::Unknown && packet.raw.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // 뭔가 왔는데 디코딩은 실패함 (길이 이상, CRC 불일치 등)
        if (packet.kind == ReceivedPacketKind::Unknown) {
            // [추가 2026-08-19] 지금까지는 여기서 그냥 화면에 찍고 버렸다 —
            // "NACK 경로는 시뮬레이션으로만 검증됨"(docs/roadmap.md)의 원인
            // 중 하나. ota_protocol_decode_data()는 CRC가 안 맞으면 통째로
            // 실패해서 어떤 청크였는지조차 모르지만, 헤더 자체는 CRC 검사
            // 대상이 아니므로(session/simplereceiver.h peekDataHeaderForNack
            // 주석 참고) 그 청크 번호는 여전히 알아낼 수 있다. 알아낼 수
            // 있으면 NACK을 보내 즉시 재전송을 유도하고, 알 수 없으면(DATA도
            // 아니거나 헤더 자체가 깨졌으면) 예전처럼 그냥 버린다(송신측이
            // 타임아웃으로 알아서 재전송함).
            uint32_t badSessionId = 0, badSequence = 0;
            if (peekDataHeaderForNack(packet.raw, &badSessionId, &badSequence)
                && badSessionId == currentSessionId) {
                std::cout << "[smoke_recv] OTA_DATA CRC 오류 감지 session=0x" << std::hex
                           << badSessionId << std::dec << " seq=" << badSequence
                           << " (" << packet.raw.size() << "byte)\n";
                ReceivedPacket crcFailPacket;
                crcFailPacket.kind = ReceivedPacketKind::Data;
                crcFailPacket.sessionId = badSessionId;
                crcFailPacket.sequence = badSequence;
                const bool nacked =
                    sendAckFor(transport, crcFailPacket, static_cast<uint8_t>(OTA_RESULT_INVALID_CRC));
                std::cout << "[smoke_recv]   -> NACK(CRC 오류) " << (nacked ? "전송함" : "전송 실패") << "\n";
            } else {
                std::cout << "[smoke_recv] 디코딩 실패 (" << packet.raw.size() << "byte): ";
                printHex(packet.raw);
            }
            continue;
        }

        // 이번 패킷 처리 중 seq 범위 초과 같은 "받았지만 유효하지 않음"이
        // 감지되면 true로 바뀜 — 맨 아래 응답 전송 분기에서 ACK 대신 NACK을
        // 보내는 데 씀. Data 케이스가 아니면 항상 false로 유지됨.
        bool sequenceOutOfRange = false;

        switch (packet.kind) {
        case ReceivedPacketKind::Start:
            dataCount = 0;
            totalChunksHint = packet.totalChunks;
            currentSessionId = packet.sessionId;
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " target=0x" << std::hex << packet.targetDeviceId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks << "\n";

            // START를 받은 시점에 파일을 새로 만든다(같은 세션이 재시작되면
            // 이전 내용이 남지 않도록 truncate). imageSize만큼 미리 늘려둬서,
            // 유실된 구간이 0으로 남아 어디가 빠졌는지 드러나게 한다.
            if (!outPath.empty()) {
                outFile.close();
                outFile.clear();
                outFile.open(outPath, std::ios::binary | std::ios::out | std::ios::trunc);
                if (!outFile) {
                    std::cerr << "[smoke_recv] 파일 열기 실패: " << outPath << "\n";
                } else {
                    if (packet.imageSize > 0) {
                        outFile.seekp(static_cast<std::streamoff>(packet.imageSize) - 1);
                        const char zero = 0;
                        outFile.write(&zero, 1);
                    }
                    // 재사용 위해 읽기까지 가능한 모드로 다시 열기
                    outFile.close();
                    outFile.open(outPath,
                                  std::ios::binary | std::ios::in | std::ios::out);
                    expectedChunks = packet.totalChunks;
                    seqSeen.assign(expectedChunks, false);
                    writtenChunks = 0;
                    duplicateChunks = 0;
                    std::memcpy(expectedSha256, packet.imageSha256, sizeof(expectedSha256));
                    std::cout << "[smoke_recv]   -> " << outPath
                              << " 생성 (" << packet.imageSize << "byte 예약)\n";
                }
            }
            break;
        case ReceivedPacketKind::Data: {
            ++dataCount;
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " seq=" << packet.sequence
                       << " len=" << static_cast<int>(packet.payloadLength)
                       << " (누적 " << dataCount;
            if (totalChunksHint > 0)
                std::cout << "/" << totalChunksHint;
            std::cout << ")\n";

            // payload는 raw의 헤더 뒤부터 — ReceivedPacket에 payload 전용
            // 필드가 없어서 raw에서 잘라 쓴다(헤더 12byte + payload).
            if (outFile.is_open() && packet.payloadLength > 0
                && packet.raw.size() >= OTA_DATA_HEADER_SIZE + packet.payloadLength) {
                if (packet.sequence < expectedChunks) {
                    if (seqSeen[packet.sequence])
                        ++duplicateChunks;
                    else
                        ++writtenChunks;
                    seqSeen[packet.sequence] = true;

                    const std::streamoff offset =
                        static_cast<std::streamoff>(packet.sequence) * OTA_MAX_PAYLOAD_SIZE;
                    outFile.seekp(offset);
                    outFile.write(
                        reinterpret_cast<const char *>(packet.raw.data() + OTA_DATA_HEADER_SIZE),
                        packet.payloadLength);
                    outFile.flush();
                } else {
                    // [수정 2026-08-19] 예전에는 여기서 로그만 찍고 아래
                    // 공용 응답 분기에서 무조건 ACK을 보냈다 — 저장은
                    // 안 했으면서 성공했다고 답한 셈이라 송신측이 이 청크를
                    // 다시는 재전송할 기회가 없었다(ACK을 받았으니 끝난
                    // 줄 앎). sequenceOutOfRange를 세워서 NACK
                    // (OTA_RESULT_INVALID_SEQUENCE)이 나가도록 고침.
                    std::cerr << "[smoke_recv]   ! seq=" << packet.sequence
                              << " 가 totalChunks(" << expectedChunks
                              << ") 범위를 넘음 - 저장 생략, NACK 보냄\n";
                    sequenceOutOfRange = true;
                }
            }
            break;
        }
        case ReceivedPacketKind::End:
            std::cout << "[smoke_recv] " << kindToString(packet.kind)
                       << " session=0x" << std::hex << packet.sessionId << std::dec
                       << " imageSize=" << packet.imageSize
                       << " totalChunks=" << packet.totalChunks
                       << " (실제 받은 DATA 개수=" << dataCount << ")\n";

            if (outFile.is_open()) {
                outFile.flush();
                std::cout << "[smoke_recv] === 재조립 결과 ===\n"
                          << "  파일        : " << outPath << "\n"
                          << "  기대 청크   : " << expectedChunks << "\n"
                          << "  채워진 청크 : " << writtenChunks << "\n"
                          << "  중복 수신   : " << duplicateChunks << "\n";

                // 빠진 seq를 앞쪽 몇 개만 보여준다(전부 찍으면 화면이 넘침).
                if (writtenChunks < expectedChunks) {
                    std::cout << "  누락 청크   : " << (expectedChunks - writtenChunks)
                              << "개 — seq ";
                    int shown = 0;
                    for (uint32_t i = 0; i < expectedChunks && shown < 20; ++i) {
                        if (!seqSeen[i]) {
                            std::cout << i << " ";
                            ++shown;
                        }
                    }
                    if (expectedChunks - writtenChunks > 20)
                        std::cout << "...";
                    std::cout << "\n"
                              << "  => 누락 구간은 0으로 남아 있으므로 원본과 다릅니다.\n";
                } else {
                    std::cout << "  누락 청크   : 없음\n";

                    // [추가 2026-08-19] 지금까지는 "누락 0개"까지만 여기서
                    // 확인하고, 실제 바이트 일치 여부는 사람이 양쪽에서
                    // sha256sum을 손으로 돌려서 비교해야 했다. OTA_START의
                    // image_sha256을 이제 송신측이 실제로 채워 보내므로
                    // (session/otasession.cpp, 2026-08-19), 여기서 같은 방식
                    // (core/sha256.h)으로 재조립된 파일의 해시를 직접 계산해
                    // 비교하면 자동으로 판정할 수 있다 — ESP32의
                    // ota_writer_finish()가 하는 검증과 정확히 같은 절차를
                    // 미리 리허설하는 셈이기도 하다.
                    outFile.flush();
                    uint8_t actualSha256[32] = {};
                    std::string sha256Error;
                    if (!sha256File(outPath, actualSha256, &sha256Error)) {
                        std::cout << "  SHA256      : 계산 실패 (" << sha256Error << ")\n";
                    } else {
                        const bool match = std::memcmp(actualSha256, expectedSha256,
                                                         sizeof(actualSha256)) == 0;
                        std::cout << "  기대 SHA256 : " << sha256ToHex(expectedSha256) << "\n"
                                  << "  실제 SHA256 : " << sha256ToHex(actualSha256) << "\n"
                                  << "  => " << (match ? "일치 (무결성 확인됨)"
                                                        : "!! 불일치 — 파일이 손상됨 !!")
                                  << "\n";
                    }

                    std::cout << "  (송신측과 직접 대조하려면) sha256sum <원본.bin> / sha256sum "
                              << outPath << "\n";
                }
            }
            break;
        default:
            std::cout << "[smoke_recv] " << kindToString(packet.kind) << "\n";
            break;
        }

        // Start/Data/End만 응답 대상 — 받았다는 확인(ACK, 또는 이번에 감지된
        // 오류가 있으면 NACK)을 바로 돌려보냄. 재전송 판단·대기 없이 "이거
        // 받았다/못 받았다"만 반사적으로 알려주는 것 (session/
        // simplereceiver.h의 sendAckFor 주석 참고).
        if (packet.kind == ReceivedPacketKind::Start || packet.kind == ReceivedPacketKind::Data
            || packet.kind == ReceivedPacketKind::End) {
            const uint8_t resultCode = sequenceOutOfRange
                ? static_cast<uint8_t>(OTA_RESULT_INVALID_SEQUENCE)
                : static_cast<uint8_t>(OTA_RESULT_OK);
            const bool acked = sendAckFor(transport, packet, resultCode);
            std::cout << "[smoke_recv]   -> " << (sequenceOutOfRange ? "NACK(순서 오류) " : "ACK ")
                       << (acked ? "전송함" : "전송 실패") << "\n";

            // [우회책 2026-08-19 도입 → 2026-08-19 같은 날 제거하고 재검증]
            //
            // 증상이었던 것: OtaSession(배치 ACK+재전송)으로 전송하면 이 프로그램이
            // DATA를 6개쯤 받고 나서 그 뒤로 아무것도 수신하지 못했다(추정 원인:
            // kernel-cc1101-spi의 cc1101_gdo0_thread()가 GDO0 하나로 TX/RX 완료를
            // 모두 처리하는데, TX 완료 엣지를 놓치면 cc->state가 CC1101_STATE_TX에
            // 갇혀 그 뒤 수신 인터럽트가 전부 폐기됨 — 자세한 분석은
            // docs/note/bug-report-cc1101-rx-stall-2026-08-19.md).
            //
            // 드라이버 담당자가 이 부분을 고쳤다고 전달받아서, 우회책
            // (ACK 직후 transport.startRx() 강제 호출)을 지우고 다시 검증하는 중.
            // 이 줄이 없어도 기존과 동일하게 성공하면 드라이버 수정이 실제로
            // 효과가 있었다는 뜻 — docs/roadmap.md 3절 "완료 조건"이 요구하던
            // 바로 그 조건(우회책 없이 성공)이 이걸로 충족됨.
            //
            // 실패하면(다시 DATA 6개쯤에서 멎으면) 아래 줄을 되살리면 됨:
            //   transport.startRx();
        }
    }
}
