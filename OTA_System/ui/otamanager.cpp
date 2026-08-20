#include "otamanager.h"
#include "ui_otamanager.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>

OtaManager::OtaManager(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::OtaManager)
{
    ui->setupUi(this);
    setupConnections();
    loadSettings();
    appendLog(QStringLiteral("INFO"), tr("화면 초기화 완료"));
}

OtaManager::~OtaManager()
{
    delete ui;
}

void OtaManager::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}

void OtaManager::setupConnections()
{
    connect(ui->connectButton, &QPushButton::clicked, this, &OtaManager::onConnectClicked);
    // unicastRadio·broadcastRadio는 .ui의 modeButtonGroup으로 이미 배타적으로 묶여 있어서,
    // 하나만 연결해도 다른 라디오버튼이 바뀔 때 같이 toggled가 발생함
    connect(ui->unicastRadio, &QRadioButton::toggled, this, &OtaManager::onModeChanged);
    connect(ui->selectFileButton, &QPushButton::clicked, this, &OtaManager::onSelectFileClicked);
    connect(ui->chunkSizeSpin, &QSpinBox::valueChanged, this, &OtaManager::onChunkSizeChanged);
    connect(ui->startButton, &QPushButton::clicked, this, &OtaManager::onStartClicked);
    connect(ui->pauseButton, &QPushButton::clicked, this, &OtaManager::onPauseClicked);
    connect(ui->hopSeedRandomButton, &QPushButton::clicked, this, &OtaManager::onHopSeedRandomClicked);

    // 호핑 난수(FHSS seed)는 uint32_t(0~4294967295) 범위라 QIntValidator(int
    // 범위, ~21억까지)로는 못 담아서 QRegularExpressionValidator로 숫자만
    // 입력되게 막고(최대 10자리), 실제 범위 초과 여부는 쓰는 시점(현재는
    // onHopSeedRandomClicked()뿐, 나중에 파이/ESP32 연동 시 여기서 같이 검사)에
    // 확인하는 방식으로 함 — kernel-cc1101-spi의 cc1101_fhss_hop_policy.seed와
    // 같은 타입(uint32_t)에 맞춘 것 (조사 결과는 디자인노트 참고)
    auto *hopSeedValidator = new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{0,10}")), this);
    ui->hopSeedEdit->setValidator(hopSeedValidator);
}

void OtaManager::loadSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"), QStringLiteral("OtaManager"));

    const QString port = settings.value(QStringLiteral("transport/port"), QStringLiteral("/dev/cc1101")).toString();
    const int driverIndex = settings.value(QStringLiteral("transport/driverIndex"), 0).toInt();
    const int chunkSize = settings.value(QStringLiteral("file/chunkSize"), 48).toInt(); // ota-protocol OTA_MAX_PAYLOAD_SIZE (v0.2, 2026-08-11 갱신)
    const bool broadcast = settings.value(QStringLiteral("target/broadcast"), false).toBool();
    // 기본값 없음(빈 문자열) — 코드에 시드를 하드코딩하지 않기 위해서.
    // 사용자가 직접 입력하거나 "무작위 생성"으로 채워야 함
    const QString hopSeed = settings.value(QStringLiteral("fhss/hopSeed"), QString()).toString();

    ui->portEdit->setText(port);
    if (driverIndex >= 0 && driverIndex < ui->driverCombo->count())
        ui->driverCombo->setCurrentIndex(driverIndex);
    ui->chunkSizeSpin->setValue(chunkSize);
    if (broadcast)
        ui->broadcastRadio->setChecked(true);
    else
        ui->unicastRadio->setChecked(true);
    ui->hopSeedEdit->setText(hopSeed);
}

void OtaManager::saveSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"), QStringLiteral("OtaManager"));
    settings.setValue(QStringLiteral("transport/port"), ui->portEdit->text());
    settings.setValue(QStringLiteral("transport/driverIndex"), ui->driverCombo->currentIndex());
    settings.setValue(QStringLiteral("file/chunkSize"), ui->chunkSizeSpin->value());
    settings.setValue(QStringLiteral("target/broadcast"), ui->broadcastRadio->isChecked());
    settings.setValue(QStringLiteral("fhss/hopSeed"), ui->hopSeedEdit->text());
}

void OtaManager::appendLog(const QString &tag, const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    ui->logView->appendPlainText(QStringLiteral("%1 [%2] %3").arg(timestamp, tag, message));
}

void OtaManager::recalcChunkInfo()
{
    if (m_selectedFileSize <= 0 || ui->chunkSizeSpin->value() <= 0) {
        ui->totalChunksValueLabel->setText(QStringLiteral("-"));
        return;
    }
    const qint64 chunkSize = ui->chunkSizeSpin->value();
    // 올림 나눗셈: 마지막 청크가 청크 크기보다 작아도 1개로 세기 위함 (패딩 처리 대상)
    const qint64 totalChunks = (m_selectedFileSize + chunkSize - 1) / chunkSize;
    ui->totalChunksValueLabel->setText(QString::number(totalChunks));
    ui->progressStatusLabel->setText(tr("%1 청크 중 대기 중").arg(totalChunks));
}

void OtaManager::onConnectClicked()
{
    // TODO(부품 입고 후): 실제 ITransport 구현체(CC1101 fd open/ioctl)로 교체
    m_connected = !m_connected;
    if (m_connected) {
        ui->connectionStatusDot->setStyleSheet(QStringLiteral("background-color:#2ecc71; border-radius:5px;"));
        ui->connectionStatusLabel->setText(tr("연결됨 (%1)").arg(ui->driverCombo->currentText()));
        ui->connectButton->setText(tr("연결 해제"));
        appendLog(QStringLiteral("INFO"), tr("연결됨: %1 / %2").arg(ui->driverCombo->currentText(), ui->portEdit->text()));
    } else {
        ui->connectionStatusDot->setStyleSheet(QStringLiteral("background-color:#c0392b; border-radius:5px;"));
        ui->connectionStatusLabel->setText(tr("연결 안 됨"));
        ui->connectButton->setText(tr("연결"));
        appendLog(QStringLiteral("INFO"), tr("연결 해제됨"));
    }
}

void OtaManager::onModeChanged()
{
    ui->targetCombo->setEnabled(ui->unicastRadio->isChecked());
}

void OtaManager::onSelectFileClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("펌웨어 BIN 파일 선택"), QString(), tr("Binary files (*.bin);;All files (*)"));
    if (path.isEmpty())
        return;

    const QFileInfo info(path);
    m_selectedFilePath = path;
    m_selectedFileSize = info.size();
    ui->filePathLabel->setText(info.fileName());
    ui->fileSizeValueLabel->setText(QStringLiteral("%1 KB").arg(m_selectedFileSize / 1024.0, 0, 'f', 1));
    recalcChunkInfo();
    appendLog(QStringLiteral("INFO"), tr("파일 선택: %1 (%2 byte)").arg(info.fileName()).arg(m_selectedFileSize));
}

void OtaManager::onChunkSizeChanged(int value)
{
    Q_UNUSED(value);
    recalcChunkInfo();
}

void OtaManager::onStartClicked()
{
    // TODO(마일스톤 3/4): 실제 BIN 분할·CRC 계산·재전송 큐 로직 연결
    if (m_selectedFilePath.isEmpty()) {
        appendLog(QStringLiteral("WARN"), tr("파일을 먼저 선택하세요"));
        return;
    }
    ui->pauseButton->setEnabled(true);
    appendLog(QStringLiteral("INFO"), tr("전송 시작 (분할/재전송 로직은 마일스톤 3~4에서 연결 예정)"));
}

void OtaManager::onPauseClicked()
{
    // TODO(마일스톤 4): 재전송 큐 일시정지/재개 로직 연결
    appendLog(QStringLiteral("INFO"), tr("일시정지 (로직 연결 예정)"));
}

void OtaManager::onHopSeedRandomClicked()
{
    // QRandomGenerator::global()이 uint32_t 그대로인 quint32를 뽑아주므로
    // kernel-cc1101-spi의 cc1101_fhss_hop_policy.seed(uint32_t)와 범위가
    // 정확히 맞음 — 나중에 실제로 파이/ESP32에 넘길 때 값 변환이 필요 없음.
    //
    // TODO(다음 단계): 지금은 화면에만 값을 채우고 끝 — 실제로 라즈베리파이
    // CC1101 드라이버(ioctl CC1101_IOC_FHSS_SET_CONFIG, kernel-cc1101-spi/
    // cc1101_hop.c)에 넘기거나 ESP32로 전달하는 연동은 아직 없음. ESP32
    // 쪽은 현재 시드 개념 자체가 없어서(순차 채널 배열만 하드코딩, 담당자
    // 문서에 "시드 기반 셔플은 미구현"이라고 명시) 프로토콜 확장이 먼저
    // 필요함 — 자세한 조사 내용은
    // docs/note/design-notes-gateway-ota-es.md 참고
    const quint32 seed = QRandomGenerator::global()->generate();
    ui->hopSeedEdit->setText(QString::number(seed));
    appendLog(QStringLiteral("INFO"), tr("호핑 난수 무작위 생성: %1").arg(seed));
}
