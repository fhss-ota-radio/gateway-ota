#include "otamanager.h"
#include "ui_otamanager.h"

#include "cc1101transport.h" // Cc1101Transport — driverCombo에서 "CC1101" 선택 시 실제로 생성

extern "C" {
#include "ota_protocol.h" // OTA_BROADCAST_DEVICE_ID
}

#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QTimer>

OtaManager::OtaManager(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::OtaManager)
{
    ui->setupUi(this);
    setupConnections();
    loadSettings();

    // otasession.h 84행: "Qt라면 QTimer로 주기 호출, 예: 10ms 간격" — 그대로.
    // 세션이 없을 때(m_session == nullptr)는 onSessionTick()이 조용히 리턴하므로
    // 타이머는 앱 시작 시부터 그냥 계속 돌려도 안전함 (매번 세션 유무를 따로
    // 챙기는 것보다 단순함).
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(10);
    connect(m_tickTimer, &QTimer::timeout, this, &OtaManager::onSessionTick);
    m_tickTimer->start();

    appendLog(QStringLiteral("INFO"), tr("화면 초기화 완료"));
}

OtaManager::~OtaManager()
{
    // m_session이 m_transport를 참조(ITransport&)로 들고 있어서, m_transport가
    // 먼저 사라지면 안 됨 — unique_ptr 소멸 순서는 선언 역순(m_tickTimer,
    // m_session, m_transport 순으로 선언했으니 소멸은 그 반대)이라 자동으로
    // m_session이 m_transport보다 먼저 사라져서 문제없지만, 명시적으로 한 번
    // 더 순서를 강제해서 이 불변조건이 헤더 선언 순서에 몰래 의존하지 않게 함.
    m_session.reset();
    m_transport.reset();
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
    if (m_connected) {
        // 전송 중(Idle/Completed/Failed가 아닌 상태)에는 연결을 끊지 못하게 막음 —
        // OtaSession이 ITransport&(참조)로 transport를 들고 있어서, 세션이 살아있는
        // 채로 transport를 닫아버리면 이후 send()/recv() 호출이 닫힌 fd를 건드리게 됨.
        if (m_session && m_session->state() != OtaSessionState::Idle
            && m_session->state() != OtaSessionState::Completed
            && m_session->state() != OtaSessionState::Failed) {
            appendLog(QStringLiteral("WARN"),
                      tr("전송이 진행 중입니다 — 완료/실패 후에 연결을 해제하세요"));
            return;
        }
        m_session.reset();
        if (m_transport)
            m_transport->close();
        m_transport.reset();

        m_connected = false;
        ui->connectionStatusDot->setStyleSheet(QStringLiteral("background-color:#c0392b; border-radius:5px;"));
        ui->connectionStatusLabel->setText(tr("연결 안 됨"));
        ui->connectButton->setText(tr("연결"));
        appendLog(QStringLiteral("INFO"), tr("연결 해제됨"));
        return;
    }

    // driverCombo: 0="로컬 파일 (테스트용)", 1="CC1101 (/dev/cc1101)" (otamanager.ui 순서 그대로)
    // 로컬 파일 백엔드(LocalFileTransport)는 아직 구현되지 않음(docs/roadmap.md
    // 마일스톤 3 체크리스트 참고) — CC1101만 실제로 연결됨.
    if (ui->driverCombo->currentIndex() != 1) {
        appendLog(QStringLiteral("WARN"),
                  tr("\"%1\" 드라이버는 아직 미구현입니다 — CC1101을 선택하세요")
                      .arg(ui->driverCombo->currentText()));
        return;
    }

    auto transport = std::make_unique<Cc1101Transport>(ui->portEdit->text().toStdString());
    if (!transport->open()) {
        appendLog(QStringLiteral("ERROR"),
                  tr("CC1101 연결 실패: %1 (경로/권한을 확인하세요)").arg(ui->portEdit->text()));
        return;
    }
    m_transport = std::move(transport);

    m_connected = true;
    ui->connectionStatusDot->setStyleSheet(QStringLiteral("background-color:#2ecc71; border-radius:5px;"));
    ui->connectionStatusLabel->setText(tr("연결됨 (%1)").arg(ui->driverCombo->currentText()));
    ui->connectButton->setText(tr("연결 해제"));
    appendLog(QStringLiteral("INFO"), tr("연결됨: %1 / %2").arg(ui->driverCombo->currentText(), ui->portEdit->text()));
}

void OtaManager::onModeChanged()
{
    ui->targetCombo->setEnabled(ui->unicastRadio->isChecked());
    // 특정 기기 지정 전송은 DISCOVER 연동(기기 목록 → 실제 device_id 매핑) 후에나
    // 가능함 — targetCombo가 지금은 "Node 01/02/03" 같은 자리표시자 문자열이라
    // 실제 device_id로 못 바꿈. 진짜 막는 지점은 onStartClicked()에 있고, 여기선
    // 미리 안내만 함
    if (ui->unicastRadio->isChecked())
        appendLog(QStringLiteral("WARN"),
                  tr("유니캐스트는 DISCOVER 연동 전까지 실제 전송이 안 됩니다 — 지금은 브로드캐스트만 가능"));
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
    if (m_selectedFilePath.isEmpty()) {
        appendLog(QStringLiteral("WARN"), tr("파일을 먼저 선택하세요"));
        return;
    }
    if (!m_connected || !m_transport) {
        appendLog(QStringLiteral("WARN"), tr("먼저 연결하세요"));
        return;
    }
    if (m_session && m_session->state() != OtaSessionState::Idle
        && m_session->state() != OtaSessionState::Completed
        && m_session->state() != OtaSessionState::Failed) {
        appendLog(QStringLiteral("WARN"), tr("이미 전송이 진행 중입니다"));
        return;
    }
    // onModeChanged()에서 미리 경고했던 것과 같은 이유(targetCombo가 실제
    // device_id로 못 바꾸는 자리표시자라서) — 여기가 실제로 막는 지점.
    if (ui->unicastRadio->isChecked()) {
        appendLog(QStringLiteral("WARN"),
                  tr("특정 기기 지정 전송은 아직 지원되지 않습니다 — 브로드캐스트를 선택하세요"));
        return;
    }

    m_retransmitEventCount = 0;
    // otasession.h 67~72행 기본값(batchSize=5, timeoutMs=300, maxRetry=5,
    // chunkDelayMs=40) 그대로 씀 — 전부 실기기 검증으로 확정된 값
    // (docs/roadmap.md 3절)
    m_session = std::make_unique<OtaSession>(*m_transport);
    m_session->setOnStateChanged([this](OtaSessionState state) { handleSessionStateChanged(state); });

    // OTA_BROADCAST_DEVICE_ID 고정 — 위에서 unicastRadio면 이미 막고 리턴했으므로
    // 여기 도달했다는 건 broadcastRadio가 체크된 상태
    if (!m_session->start(m_selectedFilePath.toStdString(), OTA_BROADCAST_DEVICE_ID)) {
        appendLog(QStringLiteral("ERROR"),
                  tr("전송 시작 실패: %1").arg(QString::fromStdString(m_session->errorMessage())));
        m_session.reset();
        return;
    }

    ui->startButton->setEnabled(false);
    ui->pauseButton->setEnabled(true);
    ui->pauseButton->setText(tr("일시정지"));
    appendLog(QStringLiteral("INFO"),
              tr("전송 시작 (session_id=0x%1, %2)")
                  .arg(QString::number(m_session->sessionId(), 16), m_selectedFilePath));
    updateProgressUi();
}

void OtaManager::onPauseClicked()
{
    if (!m_session) {
        appendLog(QStringLiteral("WARN"), tr("진행 중인 전송이 없습니다"));
        return;
    }
    // pause()/resume() 둘 다 내부에서 setState()를 동기 호출하므로
    // (otasession.cpp의 setState() 구현 참고), 버튼 텍스트·활성화 상태는 여기서
    // 직접 안 바꾸고 handleSessionStateChanged()에 맡김 — 상태 하나만 보고
    // 판단하는 곳을 하나로 유지하기 위함
    if (m_session->state() == OtaSessionState::Paused)
        m_session->resume(otaSessionNowMs());
    else
        m_session->pause(otaSessionNowMs());
}

void OtaManager::onSessionTick()
{
    if (!m_session)
        return;
    m_session->tick(otaSessionNowMs());
    updateProgressUi();
}

void OtaManager::handleSessionStateChanged(OtaSessionState state)
{
    appendLog(QStringLiteral("INFO"),
              tr("상태 변경: %1").arg(QString::fromUtf8(otaSessionStateName(state))));

    // Retransmitting은 "슬롯 하나 재전송"마다 순간적으로 거쳐가는 상태라서
    // (otasession.h 36~38행 주석 참고) 이 콜백이 불릴 때마다 카운트하면 곧
    // 재전송 이벤트 수의 근사치가 됨 — 정확한 재시도 횟수(슬롯별 retryCount
    // 합)는 progress()가 아직 안 담고 있어서 못 씀
    if (state == OtaSessionState::Retransmitting)
        ++m_retransmitEventCount;

    switch (state) {
    case OtaSessionState::Paused:
        ui->pauseButton->setText(tr("재개"));
        ui->pauseButton->setEnabled(true);
        break;
    case OtaSessionState::Completed:
        appendLog(QStringLiteral("INFO"), tr("전송 완료"));
        ui->startButton->setEnabled(true);
        ui->pauseButton->setEnabled(false);
        ui->pauseButton->setText(tr("일시정지"));
        break;
    case OtaSessionState::Failed:
        appendLog(QStringLiteral("ERROR"),
                  tr("전송 실패: %1")
                      .arg(QString::fromStdString(m_session ? m_session->errorMessage() : std::string())));
        ui->startButton->setEnabled(true);
        ui->pauseButton->setEnabled(false);
        ui->pauseButton->setText(tr("일시정지"));
        break;
    case OtaSessionState::Idle:
        break;
    default: // Handshaking/SendingBatch/WaitingBatchAck/Retransmitting/WaitingEndAck — 진행 중
        ui->pauseButton->setText(tr("일시정지"));
        ui->pauseButton->setEnabled(true);
        ui->startButton->setEnabled(false);
        break;
    }

    updateProgressUi();
}

void OtaManager::updateProgressUi()
{
    if (!m_session) {
        ui->progressBar->setValue(0);
        return;
    }
    const OtaSessionProgress p = m_session->progress();
    ui->progressBar->setMaximum(static_cast<int>(p.totalChunks));
    ui->progressBar->setValue(static_cast<int>(p.ackedChunks));
    ui->ackStatusLabel->setText(tr("ACK %1 / %2 · 배치 %3/%4 · 재전송 %5회")
                                     .arg(p.ackedChunks)
                                     .arg(p.totalChunks)
                                     .arg(p.currentBatchNumber)
                                     .arg(p.totalBatches)
                                     .arg(m_retransmitEventCount));
    ui->progressStatusLabel->setText(QString::fromUtf8(otaSessionStateName(m_session->state())));
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
