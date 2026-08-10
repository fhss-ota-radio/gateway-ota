#include "otamanager.h"
#include "ui_otamanager.h"

#include "cc1101transport.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <algorithm>

namespace {

// 현재 커널 드라이버 cc1101_write()가 허용하는 최대 userspace packet 길이.
constexpr int kCc1101MaxPacketSize = 61;

// BinSplitter packet = OTA header + firmware payload.
// 현재 OTA header는 OTA_PACKET_HEADER_SIZE(9 byte)이므로 실 payload 최대는 52 byte.
constexpr int maxOtaPayloadForDriver()
{
    return kCc1101MaxPacketSize - static_cast<int>(OTA_PACKET_HEADER_SIZE);
}

QString statusToString(Cc1101Status status)
{
    switch (status) {
    case Cc1101Status::Ok:             return QStringLiteral("OK");
    case Cc1101Status::NoData:         return QStringLiteral("NO_DATA");
    case Cc1101Status::Timeout:        return QStringLiteral("TIMEOUT");
    case Cc1101Status::Busy:           return QStringLiteral("BUSY");
    case Cc1101Status::InvalidArg:     return QStringLiteral("INVALID_ARG");
    case Cc1101Status::InvalidState:   return QStringLiteral("INVALID_STATE");
    case Cc1101Status::FrameTooLarge:  return QStringLiteral("FRAME_TOO_LARGE");
    case Cc1101Status::NotInitialized: return QStringLiteral("NOT_INITIALIZED");
    case Cc1101Status::CrcError:       return QStringLiteral("CRC_ERROR");
    case Cc1101Status::FifoError:      return QStringLiteral("FIFO_ERROR");
    case Cc1101Status::IoError:        return QStringLiteral("IO_ERROR");
    case Cc1101Status::HardwareError:  return QStringLiteral("HARDWARE_ERROR");
    }
    return QStringLiteral("UNKNOWN");
}

} // namespace

OtaManager::OtaManager(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::OtaManager)
{
    ui->setupUi(this);

    // 드라이버는 packet 전체 길이를 61 byte까지 허용한다.
    // OTA header를 제외한 실제 BIN chunk 크기를 UI에서 미리 제한한다.
    ui->chunkSizeSpin->setMaximum(maxOtaPayloadForDriver());
    if (ui->chunkSizeSpin->value() > maxOtaPayloadForDriver())
        ui->chunkSizeSpin->setValue(maxOtaPayloadForDriver());

    m_sendTimer = new QTimer(this);
    m_sendTimer->setSingleShot(true);

    setupConnections();
    loadSettings();
    appendLog(QStringLiteral("INFO"), tr("화면 초기화 완료"));
    appendLog(QStringLiteral("INFO"),
              tr("CC1101 드라이버 기준 OTA payload 최대 %1 byte")
                  .arg(maxOtaPayloadForDriver()));
}

OtaManager::~OtaManager()
{
    if (m_transport)
        m_transport->close();
    delete ui;
}

void OtaManager::closeEvent(QCloseEvent *event)
{
    saveSettings();
    if (m_transport)
        m_transport->close();
    QMainWindow::closeEvent(event);
}

void OtaManager::setupConnections()
{
    connect(ui->connectButton, &QPushButton::clicked,
            this, &OtaManager::onConnectClicked);
    connect(ui->unicastRadio, &QRadioButton::toggled,
            this, &OtaManager::onModeChanged);
    connect(ui->selectFileButton, &QPushButton::clicked,
            this, &OtaManager::onSelectFileClicked);
    connect(ui->chunkSizeSpin, &QSpinBox::valueChanged,
            this, &OtaManager::onChunkSizeChanged);
    connect(ui->startButton, &QPushButton::clicked,
            this, &OtaManager::onStartClicked);
    connect(ui->pauseButton, &QPushButton::clicked,
            this, &OtaManager::onPauseClicked);
    connect(m_sendTimer, &QTimer::timeout,
            this, &OtaManager::sendNextChunk);
}

void OtaManager::loadSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"),
                       QStringLiteral("OtaManager"));

    const QString port = settings.value(
        QStringLiteral("transport/port"),
        QStringLiteral("/dev/cc1101")).toString();

    const int driverIndex = settings.value(
        QStringLiteral("transport/driverIndex"), 1).toInt();

    const int chunkSize = settings.value(
        QStringLiteral("file/chunkSize"),
        maxOtaPayloadForDriver()).toInt();

    const bool broadcast = settings.value(
        QStringLiteral("target/broadcast"), false).toBool();

    ui->portEdit->setText(port);

    if (driverIndex >= 0 && driverIndex < ui->driverCombo->count())
        ui->driverCombo->setCurrentIndex(driverIndex);

    ui->chunkSizeSpin->setValue(
        std::clamp(chunkSize, 1, maxOtaPayloadForDriver()));

    if (broadcast)
        ui->broadcastRadio->setChecked(true);
    else
        ui->unicastRadio->setChecked(true);
}

void OtaManager::saveSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"),
                       QStringLiteral("OtaManager"));

    settings.setValue(QStringLiteral("transport/port"),
                      ui->portEdit->text());
    settings.setValue(QStringLiteral("transport/driverIndex"),
                      ui->driverCombo->currentIndex());
    settings.setValue(QStringLiteral("file/chunkSize"),
                      ui->chunkSizeSpin->value());
    settings.setValue(QStringLiteral("target/broadcast"),
                      ui->broadcastRadio->isChecked());
}

void OtaManager::appendLog(const QString &tag, const QString &message)
{
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));

    ui->logView->appendPlainText(
        QStringLiteral("%1 [%2] %3").arg(timestamp, tag, message));
}

void OtaManager::setConnectedUi(bool connected)
{
    m_connected = connected;

    if (connected) {
        ui->connectionStatusDot->setStyleSheet(
            QStringLiteral("background-color:#2ecc71; border-radius:5px;"));
        ui->connectionStatusLabel->setText(
            tr("연결됨 (%1)").arg(ui->driverCombo->currentText()));
        ui->connectButton->setText(tr("연결 해제"));
    } else {
        ui->connectionStatusDot->setStyleSheet(
            QStringLiteral("background-color:#c0392b; border-radius:5px;"));
        ui->connectionStatusLabel->setText(tr("연결 안 됨"));
        ui->connectButton->setText(tr("연결"));
    }
}

void OtaManager::recalcChunkInfo()
{
    if (m_selectedFileSize <= 0 || ui->chunkSizeSpin->value() <= 0) {
        ui->totalChunksValueLabel->setText(QStringLiteral("-"));
        return;
    }

    const qint64 chunkSize = ui->chunkSizeSpin->value();
    const qint64 totalChunks =
        (m_selectedFileSize + chunkSize - 1) / chunkSize;

    ui->totalChunksValueLabel->setText(QString::number(totalChunks));
    ui->progressStatusLabel->setText(
        tr("%1 청크 중 대기 중").arg(totalChunks));
}

void OtaManager::onConnectClicked()
{
    if (m_connected) {
        if (m_transferring) {
            appendLog(QStringLiteral("WARN"),
                      tr("전송 중에는 연결을 해제할 수 없습니다"));
            return;
        }

        if (m_transport)
            m_transport->close();

        m_transport.reset();
        setConnectedUi(false);
        appendLog(QStringLiteral("INFO"), tr("연결 해제됨"));
        return;
    }

    // 현재 v1에서 실제 구현한 transport는 CC1101뿐.
    if (!ui->driverCombo->currentText().contains(QStringLiteral("CC1101"))) {
        appendLog(QStringLiteral("WARN"),
                  tr("현재 테스트 버전은 CC1101 transport만 실제 연결됩니다"));
        return;
    }

    const std::string devicePath = ui->portEdit->text().toStdString();
    auto transport = std::make_unique<Cc1101Transport>(devicePath);

    if (!transport->open()) {
        appendLog(QStringLiteral("ERROR"),
                  tr("%1 open 실패: %2")
                      .arg(ui->portEdit->text(),
                           statusToString(transport->lastStatus())));
        return;
    }

    // 송신 후 드라이버가 RX로 돌아오지만, 시작 상태도 명시적으로 RX로 둔다.
    const Cc1101Status rxStatus = transport->startRx();
    if (rxStatus != Cc1101Status::Ok) {
        appendLog(QStringLiteral("ERROR"),
                  tr("RX 모드 진입 실패: %1")
                      .arg(statusToString(rxStatus)));
        transport->close();
        return;
    }

    m_transport = std::move(transport);
    setConnectedUi(true);

    appendLog(QStringLiteral("INFO"),
              tr("실제 드라이버 연결 성공: %1")
                  .arg(ui->portEdit->text()));
}

void OtaManager::onModeChanged()
{
    ui->targetCombo->setEnabled(ui->unicastRadio->isChecked());
}

void OtaManager::onSelectFileClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("펌웨어 BIN 파일 선택"),
        QString(),
        tr("Binary files (*.bin);;All files (*)"));

    if (path.isEmpty())
        return;

    const QFileInfo info(path);
    m_selectedFilePath = path;
    m_selectedFileSize = info.size();

    ui->filePathLabel->setText(info.fileName());
    ui->fileSizeValueLabel->setText(
        QStringLiteral("%1 KB")
            .arg(m_selectedFileSize / 1024.0, 0, 'f', 1));

    recalcChunkInfo();

    appendLog(QStringLiteral("INFO"),
              tr("파일 선택: %1 (%2 byte)")
                  .arg(info.fileName())
                  .arg(m_selectedFileSize));
}

void OtaManager::onChunkSizeChanged(int value)
{
    Q_UNUSED(value);
    recalcChunkInfo();
}

void OtaManager::onStartClicked()
{
    if (!m_connected || !m_transport || !m_transport->isOpen()) {
        appendLog(QStringLiteral("WARN"),
                  tr("먼저 /dev/cc1101에 연결하세요"));
        return;
    }

    if (m_selectedFilePath.isEmpty()) {
        appendLog(QStringLiteral("WARN"),
                  tr("파일을 먼저 선택하세요"));
        return;
    }

    if (m_transferring) {
        appendLog(QStringLiteral("WARN"),
                  tr("이미 전송 중입니다"));
        return;
    }

    std::string error;
    m_chunks = BinSplitter::split(
        m_selectedFilePath.toStdString(),
        ui->chunkSizeSpin->value(),
        &error);

    if (m_chunks.empty()) {
        appendLog(QStringLiteral("ERROR"),
                  tr("BIN 분할 실패: %1")
                      .arg(QString::fromStdString(error)));
        return;
    }

    // 드라이버 한계(61 byte)를 전송 전에 한 번 더 검증한다.
    for (const auto &chunk : m_chunks) {
        if (chunk.packet.size() > static_cast<std::size_t>(kCc1101MaxPacketSize)) {
            appendLog(QStringLiteral("ERROR"),
                      tr("OTA packet이 드라이버 최대 길이 %1 byte를 초과했습니다")
                          .arg(kCc1101MaxPacketSize));
            m_chunks.clear();
            return;
        }
    }

    // 이전 RX/TX 잔여 상태를 정리한 뒤 전송 시작.
    m_transport->flushRx();
    m_transport->flushTx();

    m_nextChunk = 0;
    m_transferring = true;
    m_paused = false;

    ui->progressBar->setRange(0, static_cast<int>(m_chunks.size()));
    ui->progressBar->setValue(0);
    ui->progressStatusLabel->setText(
        tr("0 / %1 청크").arg(m_chunks.size()));
    ui->ackStatusLabel->setText(
        tr("TX 0 / %1 · ACK 미구현").arg(m_chunks.size()));

    ui->startButton->setEnabled(false);
    ui->pauseButton->setEnabled(true);
    ui->pauseButton->setText(tr("일시정지"));

    appendLog(QStringLiteral("INFO"),
              tr("OTA 전송 시작: %1 chunks, payload=%2 byte")
                  .arg(m_chunks.size())
                  .arg(ui->chunkSizeSpin->value()));

    m_sendTimer->start(0);
}

void OtaManager::sendNextChunk()
{
    if (!m_transferring || m_paused)
        return;

    if (!m_transport || !m_transport->isOpen()) {
        finishTransfer(false, tr("전송 중 CC1101 연결이 끊어졌습니다"));
        return;
    }

    if (m_nextChunk >= m_chunks.size()) {
        finishTransfer(true, tr("모든 OTA 청크 송신 완료"));
        return;
    }

    const OtaChunk &chunk = m_chunks[m_nextChunk];

    if (!m_transport->send(chunk.packet)) {
        const QString reason = statusToString(m_transport->lastStatus());
        finishTransfer(
            false,
            tr("청크 %1 송신 실패: %2")
                .arg(m_nextChunk)
                .arg(reason));
        return;
    }

    ++m_nextChunk;

    ui->progressBar->setValue(static_cast<int>(m_nextChunk));
    ui->progressStatusLabel->setText(
        tr("%1 / %2 청크")
            .arg(m_nextChunk)
            .arg(m_chunks.size()));

    // 아직 상대 노드 ACK 프로토콜이 첨부 소스에서 확정되지 않았으므로,
    // 여기서는 "RF write 성공"까지만 표시한다.
    ui->ackStatusLabel->setText(
        tr("TX %1 / %2 · ACK 미구현")
            .arg(m_nextChunk)
            .arg(m_chunks.size()));

    // 수천 개 청크일 수 있으므로 로그 폭주를 피한다.
    if (m_nextChunk == 1 ||
        m_nextChunk == m_chunks.size() ||
        (m_nextChunk % 100) == 0) {
        appendLog(QStringLiteral("TX"),
                  tr("청크 %1 / %2 송신 완료")
                      .arg(m_nextChunk)
                      .arg(m_chunks.size()));
    }

    // 이벤트 루프로 제어권을 돌려 UI가 갱신된 뒤 다음 청크 송신.
    m_sendTimer->start(0);
}

void OtaManager::finishTransfer(bool success, const QString &message)
{
    m_transferring = false;
    m_paused = false;

    if (m_sendTimer->isActive())
        m_sendTimer->stop();

    ui->startButton->setEnabled(true);
    ui->pauseButton->setEnabled(false);
    ui->pauseButton->setText(tr("일시정지"));

    appendLog(success ? QStringLiteral("INFO") : QStringLiteral("ERROR"),
              message);
}

void OtaManager::onPauseClicked()
{
    if (!m_transferring)
        return;

    m_paused = !m_paused;

    if (m_paused) {
        if (m_sendTimer->isActive())
            m_sendTimer->stop();

        ui->pauseButton->setText(tr("재개"));
        appendLog(QStringLiteral("INFO"), tr("OTA 전송 일시정지"));
        return;
    }

    ui->pauseButton->setText(tr("일시정지"));
    appendLog(QStringLiteral("INFO"), tr("OTA 전송 재개"));
    m_sendTimer->start(0);
}
