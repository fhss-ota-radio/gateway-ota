#include <QtTest>
#include <QTemporaryFile>

#include "binsplitter.h"

// BinSplitter + 공유 ota_protocol.h를 검증하는 테스트.
// QWidget/QMainWindow를 전혀 만들지 않으므로 화면(창)이 뜨지 않고,
// 터미널에서 실행 파일만 돌려도 결과를 볼 수 있음 (QTEST_APPLESS_MAIN).
class TestBinSplitter : public QObject
{
    Q_OBJECT

private slots:
    void splitsExactMultipleFile();
    void lastChunkIsShorterNotPadded();
    void corruptedPacketFailsCrcOnDecode();
    void reportsErrorForMissingFile();
    void reportsErrorForChunkSizeOutOfRange();
    void defaultChunkSizeMatchesProtocolMax();
};

void TestBinSplitter::splitsExactMultipleFile()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const int chunkSize = 20;
    file.write(QByteArray(chunkSize * 2, 'A')); // 정확히 2개 청크
    file.close();

    QString error;
    const auto chunks = BinSplitter::split(file.fileName(), chunkSize, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(chunks.size(), 2);
    QCOMPARE(chunks[0].header.seq, quint16(0));
    QCOMPARE(chunks[1].header.seq, quint16(1));
    QCOMPARE(chunks[0].header.total_chunks, quint16(2));
    QCOMPARE(int(chunks[0].header.payload_length), chunkSize);
    QCOMPARE(chunks[0].packet.size(), int(OTA_PACKET_HEADER_SIZE) + chunkSize);
}

void TestBinSplitter::lastChunkIsShorterNotPadded()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    const int chunkSize = 20;
    file.write(QByteArray(chunkSize + 7, 'B')); // 20 + 7 -> 마지막 청크는 7byte
    file.close();

    QString error;
    const auto chunks = BinSplitter::split(file.fileName(), chunkSize, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(chunks.size(), 2);
    // ota_protocol의 payload_length 필드가 실제 길이를 담기 때문에 0x00 패딩이 필요 없어짐
    QCOMPARE(int(chunks.last().header.payload_length), 7);
    QCOMPARE(chunks.last().packet.size(), int(OTA_PACKET_HEADER_SIZE) + 7);
}

void TestBinSplitter::corruptedPacketFailsCrcOnDecode()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(QByteArray(10, 'C'));
    file.close();

    QString error;
    auto chunks = BinSplitter::split(file.fileName(), 10, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(chunks.size(), 1);

    // payload 한 byte를 일부러 깨뜨림
    QByteArray corrupted = chunks[0].packet;
    const int payloadOffset = int(OTA_PACKET_HEADER_SIZE);
    corrupted[payloadOffset] = char(corrupted[payloadOffset] ^ 0xFF);

    ota_packet_header_t header;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;
    const bool ok = ota_protocol_decode(
        reinterpret_cast<const uint8_t *>(corrupted.constData()),
        static_cast<size_t>(corrupted.size()),
        &header, &payload, &payloadLen);

    QVERIFY(!ok); // CRC16 불일치라 디코딩이 실패해야 정상 (-> 상위 로직이 NACK 보내야 함)
}

void TestBinSplitter::reportsErrorForMissingFile()
{
    QString error;
    const auto chunks = BinSplitter::split(QStringLiteral("/no/such/file.bin"), 20, &error);
    QVERIFY(chunks.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestBinSplitter::reportsErrorForChunkSizeOutOfRange()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(QByteArray(10, 'D'));
    file.close();

    QString error;
    QVERIFY(BinSplitter::split(file.fileName(), 0, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    error.clear();
    // ota-protocol의 OTA_MAX_PAYLOAD_SIZE(55byte, CC1101 FIFO 64byte 제약)를 넘으면 거부
    QVERIFY(BinSplitter::split(file.fileName(), int(OTA_MAX_PAYLOAD_SIZE) + 1, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestBinSplitter::defaultChunkSizeMatchesProtocolMax()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    file.write(QByteArray(int(OTA_MAX_PAYLOAD_SIZE) + 10, 'E'));
    file.close();

    // chunkSize를 생략하면 BinSplitter.h의 기본값(OTA_MAX_PAYLOAD_SIZE)이 쓰여야 함
    const auto chunks = BinSplitter::split(file.fileName());

    QVERIFY(!chunks.isEmpty());
    QCOMPARE(int(chunks[0].header.payload_length), int(OTA_MAX_PAYLOAD_SIZE));
}

QTEST_APPLESS_MAIN(TestBinSplitter)
#include "tst_binsplitter.moc"
