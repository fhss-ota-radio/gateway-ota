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
#include <QThread>
#include <QTimer>

#include <cstdio>

OtaManager::OtaManager(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::OtaManager)
{
    ui->setupUi(this);
    setupConnections();
    loadSettings();

    // 청크 크기는 사용자가 정하는 값이 아니라 OTA_MAX_PAYLOAD_SIZE(무선 패킷
    // 본문 최대 크기에서 정해지는 프로토콜 고정값)라서, .ui의 텍스트를 여기서
    // 그 상수로 덮어씀 — 나중에 프로토콜이 바뀌어도 .ui를 따로 안 고쳐도 됨
    ui->chunkSizeValueLabel->setText(tr("%1 (고정)").arg(OTA_MAX_PAYLOAD_SIZE));

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
    connect(ui->startButton, &QPushButton::clicked, this, &OtaManager::onStartClicked);
    connect(ui->pauseButton, &QPushButton::clicked, this, &OtaManager::onPauseClicked);
    connect(ui->hopSeedRandomButton, &QPushButton::clicked, this, &OtaManager::onHopSeedRandomClicked);
    connect(ui->discoverButton, &QPushButton::clicked, this, &OtaManager::onDiscoverClicked);

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
    const bool broadcast = settings.value(QStringLiteral("target/broadcast"), false).toBool();
    // 기본값 없음(빈 문자열) — 코드에 시드를 하드코딩하지 않기 위해서.
    // 사용자가 직접 입력하거나 "무작위 생성"으로 채워야 함
    const QString hopSeed = settings.value(QStringLiteral("fhss/hopSeed"), QString()).toString();

    ui->portEdit->setText(port);
    if (driverIndex >= 0 && driverIndex < ui->driverCombo->count())
        ui->driverCombo->setCurrentIndex(driverIndex);
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
    if (m_selectedFileSize <= 0) {
        ui->totalChunksValueLabel->setText(QStringLiteral("-"));
        return;
    }
    // 청크 크기는 사용자가 정하는 값이 아니라 OTA_MAX_PAYLOAD_SIZE 고정값 —
    // 실제 전송(OtaSession)이 이 값으로만 분할하므로 여기서도 반드시 같은
    // 값을 써야 화면에 보이는 "총 청크 수"가 실제와 일치함
    const qint64 chunkSize = OTA_MAX_PAYLOAD_SIZE;
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
        if (m_discovering) {
            // discoverDevices()를 돌리는 워커 스레드가 아직 m_transport를 쓰는
            // 중이므로, 여기서 닫아버리면 다른 스레드가 닫힌 fd를 건드리게 됨.
            appendLog(QStringLiteral("WARN"), tr("기기 조회가 끝난 뒤 연결을 해제하세요"));
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
    // 2026-08-23에 발견해서 추가: open()은 문자 디바이스 fd만 여는 것이고,
    // 실제로 "지금부터 수신 대기"로 CC1101 칩을 전환하는 건 별도 ioctl
    // (CC1101_IOC_SET_RX, cc1101transport.cpp startRx() 참고)이라 여기서
    // 빠져 있으면 DISCOVER_ACK/ACK/NACK을 하나도 못 받는다 — CLI 도구들
    // (ota_smoke_discover 등)은 전부 open() 직후 startRx()를 부르는데
    // 이 화면 코드엔 없었음. 상세 경위: design-notes-gateway-ota-es.md 44절
    if (transport->startRx() != Cc1101Status::Ok) {
        appendLog(QStringLiteral("ERROR"), tr("CC1101 연결 실패: startRx 실패"));
        transport->close();
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
    const bool unicast = ui->unicastRadio->isChecked();
    ui->targetCombo->setEnabled(unicast);
    ui->discoverButton->setEnabled(unicast);
    // 2026-08-23: DISCOVER 연동 완료 — targetCombo가 discoverDevices() 결과로
    // 채워지면 실제 device_id로 유니캐스트 전송 가능. 아직 조회를 안 했으면
    // (targetCombo가 비어있으면) 안내만 하고, 진짜 막는 지점은 onStartClicked().
    if (unicast && ui->targetCombo->count() == 0)
        appendLog(QStringLiteral("WARN"),
                  tr("먼저 \"기기 조회 (DISCOVER)\"로 대상을 찾으세요"));
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
    if (m_discovering) {
        // DISCOVER 워커 스레드가 m_transport를 쓰는 중에 세션까지 같은
        // transport로 send()/recv()를 시작하면 두 스레드가 fd를 동시에
        // 건드리게 됨 — discoverDevices()가 끝날 때까지 기다리게 함.
        appendLog(QStringLiteral("WARN"), tr("기기 조회가 끝난 뒤 다시 시도하세요"));
        return;
    }
    // 2026-08-23: DISCOVER 연동 완료 — targetCombo의 item data(Qt::UserRole)에
    // handleDiscoveredDevices()가 실제 device_id(uint32_t)를 넣어두므로, 여기선
    // 그 값을 그대로 씀. 유니캐스트인데 아직 아무것도 조회 못 했으면
    // (targetCombo가 비어있으면 currentData()가 무효 QVariant) 여기서 막음.
    uint32_t targetDeviceId = OTA_BROADCAST_DEVICE_ID;
    if (ui->unicastRadio->isChecked()) {
        const QVariant selected = ui->targetCombo->currentData();
        if (!selected.isValid()) {
            appendLog(QStringLiteral("WARN"),
                      tr("먼저 \"기기 조회 (DISCOVER)\"로 대상을 찾은 뒤 목록에서 선택하세요"));
            return;
        }
        targetDeviceId = selected.toUInt();
    }

    m_retransmitEventCount = 0;
    // otasession.h 67~72행 기본값(batchSize=5, timeoutMs=300, maxRetry=5,
    // chunkDelayMs=40) 그대로 씀 — 전부 실기기 검증으로 확정된 값
    // (docs/roadmap.md 3절)
    m_session = std::make_unique<OtaSession>(*m_transport);
    m_session->setOnStateChanged([this](OtaSessionState state) { handleSessionStateChanged(state); });

    if (!m_session->start(m_selectedFilePath.toStdString(), targetDeviceId)) {
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

void OtaManager::onDiscoverClicked()
{
    if (!m_connected || !m_transport) {
        appendLog(QStringLiteral("WARN"), tr("먼저 연결하세요"));
        return;
    }
    if (m_discovering) {
        appendLog(QStringLiteral("WARN"), tr("이미 기기 조회 중입니다"));
        return;
    }
    if (m_session && m_session->state() != OtaSessionState::Idle
        && m_session->state() != OtaSessionState::Completed
        && m_session->state() != OtaSessionState::Failed) {
        // OtaSession이 tick()마다 같은 m_transport로 send()/recv()를 하고
        // 있는 도중에 discoverDevices()까지 같은 fd를 건드리면 응답이 서로
        // 뒤섞일 수 있음 — 전송 중엔 조회를 막음.
        appendLog(QStringLiteral("WARN"), tr("전송이 진행 중입니다 — 완료/실패 후에 조회하세요"));
        return;
    }

    m_discovering = true;
    ui->discoverButton->setEnabled(false);
    ui->discoverButton->setText(tr("조회 중..."));
    appendLog(QStringLiteral("INFO"), tr("DISCOVER 브로드캐스트 전송, 1000ms 대기..."));

    // discoverDevices()는 blocking 호출임(discovery.h 33행 주석: "Qt 화면에서
    // 쓸 때는 이 호출 자체를 별도 스레드로 돌리거나...") — GUI 스레드에서
    // 그대로 부르면 대기하는 동안 창이 얼어붙는다(진행률 바·로그창도 멈춰
    // 보임). QThread::create()로 워커 스레드에서 돌리고, 끝나면
    // QMetaObject::invokeMethod(..., Qt::QueuedConnection)로 결과를 GUI
    // 스레드로 다시 넘겨서 handleDiscoveredDevices()가 위젯을 건드리게 함
    // (Qt 위젯은 자신을 만든 스레드에서만 건드려야 하므로).
    //
    // 스레드 안전성: discoverDevices()가 워커 스레드에서 m_transport(실제
    // Cc1101Transport)를 send()/recv()하는 동안, GUI 스레드가 같은
    // m_transport를 동시에 건드리면 안 됨 — 그래서 위에서 세션 진행 중이면
    // 막았고, onStartClicked()/onConnectClicked() 쪽에도 m_discovering 검사를
    // 추가해서 조회가 끝나기 전엔 세션 시작·연결 해제를 못 하게 막아둠.
    ITransport *transport = m_transport.get();
    QThread *worker = QThread::create([this, transport]() {
        const std::vector<DiscoveredDevice> devices = discoverDevices(*transport, 1000);
        QMetaObject::invokeMethod(
            this, [this, devices]() { handleDiscoveredDevices(devices); }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void OtaManager::handleDiscoveredDevices(const std::vector<DiscoveredDevice> &devices)
{
    m_discovering = false;
    ui->discoverButton->setEnabled(ui->unicastRadio->isChecked());
    ui->discoverButton->setText(tr("기기 조회 (DISCOVER)"));

    ui->targetCombo->clear();
    if (devices.empty()) {
        appendLog(QStringLiteral("WARN"), tr("조회된 기기가 없습니다"));
        return;
    }

    for (const DiscoveredDevice &device : devices) {
        // MAC 뒤 3byte를 "AA-BB-CC" 형식으로 보여줌 — CLI 도구들
        // (smoke_discover_session_send_main.cpp의 displayDeviceId())과 같은
        // 형식으로 맞춰서, 로그를 비교할 때 헷갈리지 않게 함
        char idText[16];
        std::snprintf(idText, sizeof(idText), "%02X-%02X-%02X",
                      static_cast<unsigned>((device.deviceId >> 16) & 0xFFU),
                      static_cast<unsigned>((device.deviceId >> 8) & 0xFFU),
                      static_cast<unsigned>(device.deviceId & 0xFFU));
        const QString label = tr("%1 (fw %2.%3.%4)")
                                   .arg(QString::fromLatin1(idText))
                                   .arg(device.fwMajor)
                                   .arg(device.fwMinor)
                                   .arg(device.fwPatch);
        // 표시 텍스트는 사람이 읽는 형식이라 파싱하지 않고, 실제 device_id는
        // item data(Qt::UserRole)에 그대로 넣어서 onStartClicked()가 문자열
        // 파싱 없이 바로 씀
        ui->targetCombo->addItem(label, QVariant(static_cast<quint32>(device.deviceId)));
    }
    appendLog(QStringLiteral("INFO"), tr("기기 %1개 조회됨").arg(devices.size()));
}
