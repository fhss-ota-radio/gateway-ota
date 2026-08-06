#include "otamanager.h"
#include "ui_otamanager.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
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
}

void OtaManager::loadSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"), QStringLiteral("OtaManager"));

    const QString port = settings.value(QStringLiteral("transport/port"), QStringLiteral("/dev/cc1101")).toString();
    const int driverIndex = settings.value(QStringLiteral("transport/driverIndex"), 0).toInt();
    const int chunkSize = settings.value(QStringLiteral("file/chunkSize"), 56).toInt();
    const bool broadcast = settings.value(QStringLiteral("target/broadcast"), false).toBool();

    ui->portEdit->setText(port);
    if (driverIndex >= 0 && driverIndex < ui->driverCombo->count())
        ui->driverCombo->setCurrentIndex(driverIndex);
    ui->chunkSizeSpin->setValue(chunkSize);
    if (broadcast)
        ui->broadcastRadio->setChecked(true);
    else
        ui->unicastRadio->setChecked(true);
}

void OtaManager::saveSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"), QStringLiteral("OtaManager"));
    settings.setValue(QStringLiteral("transport/port"), ui->portEdit->text());
    settings.setValue(QStringLiteral("transport/driverIndex"), ui->driverCombo->currentIndex());
    settings.setValue(QStringLiteral("file/chunkSize"), ui->chunkSizeSpin->value());
    settings.setValue(QStringLiteral("target/broadcast"), ui->broadcastRadio->isChecked());
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
