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
#include "simplereceiver.h"

extern "C" {
#include "ota_protocol.h"
}

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
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

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "사용법: " << argv[0] << " <device_path> [저장할_파일]\n"
                  << "  예: " << argv[0] << " /dev/cc1101\n"
                  << "      " << argv[0] << " /dev/cc1101 recv.bin\n"
                  << "\n"
                  << "  저장할_파일을 주면 받은 DATA를 sequence 순서대로 재조립해서\n"
                  << "  그 파일에 씁니다. 원본과 같은지는 아래처럼 확인하세요:\n"
                  << "    (송신측) sha256sum 원본.bin\n"
                  << "    (수신측) sha256sum recv.bin\n";
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
            std::cout << "[smoke_recv] 디코딩 실패 (" << packet.raw.size() << "byte): ";
            printHex(packet.raw);
            continue;
        }

        switch (packet.kind) {
        case ReceivedPacketKind::Start:
            dataCount = 0;
            totalChunksHint = packet.totalChunks;
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
                    std::cerr << "[smoke_recv]   ! seq=" << packet.sequence
                              << " 가 totalChunks(" << expectedChunks
                              << ") 범위를 넘음 - 저장 생략\n";
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
                    std::cout << "  누락 청크   : 없음\n"
                              << "  => 모든 청크가 제자리에 기록됨. 아래로 최종 확인:\n"
                              << "       (송신측) sha256sum <원본.bin>\n"
                              << "       (수신측) sha256sum " << outPath << "\n";
                }
            }
            break;
        default:
            std::cout << "[smoke_recv] " << kindToString(packet.kind) << "\n";
            break;
        }

        // Start/Data/End만 응답 대상 — 받았다는 확인(ACK)을 바로 돌려보냄.
        // 재전송 판단·대기 없이 "이거 받았다"만 반사적으로 알려주는 것
        // (session/simplereceiver.h의 sendAckFor 주석 참고).
        if (packet.kind == ReceivedPacketKind::Start || packet.kind == ReceivedPacketKind::Data
            || packet.kind == ReceivedPacketKind::End) {
            const bool acked = sendAckFor(transport, packet);
            std::cout << "[smoke_recv]   -> ACK " << (acked ? "전송함" : "전송 실패") << "\n";

            // [임시 우회책 2026-08-19 — 드라이버가 고쳐지면 이 줄만 지우면 됨]
            //
            // 증상: OtaSession(배치 ACK+재전송)으로 전송하면 이 프로그램이 DATA를
            // 6개쯤 받고 나서 그 뒤로 아무것도 수신하지 못했다. 송신측은 계속
            // 재전송하는데 dmesg에는 드롭 메시지조차 없이 수신 로그 자체가 끊겼고,
            // 칩은 MARCSTATE=0x0D(RX 정상)로 보고했다.
            //
            // 원인(추정): kernel-cc1101-spi의 cc1101_gdo0_thread()는 GDO0 하나로
            // TX 완료와 RX 완료를 모두 처리하고 그 갈림길이 드라이버 내부 변수
            // cc->state다. TX 완료 엣지를 놓치면 state가 CC1101_STATE_TX에 갇히고,
            // 그 뒤 들어오는 수신 인터럽트는 전부 TX 분기로 빠져 폐기된다 —
            // cc1101_handle_rx_packet()에 영영 도달하지 못한다.
            // (자세한 분석은 docs/note/bug-report-cc1101-rx-stall-2026-08-19.md)
            //
            // 우회 원리: CC1101_IOC_SET_RX(=startRx())가 부르는 cc1101_enter_rx()가
            // cc->state = CC1101_STATE_RX로 되돌린다(cc1101_core.c 268행). 그래서
            // ACK를 보낸 직후마다 한 번씩 불러 상태를 RX로 확정시킨다.
            //
            // 왜 flushRx()가 아니라 startRx()인가: FLUSH_RX는 kfifo_reset()까지
            // 해서 이미 도착해 대기 중인 패킷을 버린다(실제로 8/19에 송신측에
            // flushRx()를 넣었다가 도착한 ACK가 지워져 상황이 악화됐다).
            // SET_RX는 큐를 건드리지 않고 상태만 되돌리므로 안전하다.
            //
            // 참고: 실제 수신 대상은 ESP32이고 거기엔 이 리눅스 드라이버가 없다.
            // 즉 이 우회책은 라즈베리파이 2대로 하는 내 테스트에만 필요하다.
            transport.startRx();
        }
    }
}
