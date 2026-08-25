#include "otamanager.h"
#include "ui_otamanager.h"

#include "cc1101transport.h" // Cc1101Transport — onConnectClicked()에서 실제로 생성
#include "fhssrollout.h"     // FhssHopPolicy/rolloutFhssConfig() — FHSS CONFIG/ACTIVATE 핸드셰이크

extern "C" {
#include "ota_protocol.h" // OTA_BROADCAST_DEVICE_ID
}

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cstdio>

OtaManager::OtaManager(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::OtaManager)
{
    ui->setupUi(this);

    // [2026-08-25] 로그 자동 파일 저장 — 실행 파일 옆 logs/ 폴더에 실행마다
    // 새 파일(ota_YYYYMMDD_HHMMSS.log)을 만듦. appendLog()가 나중에 이
    // m_logFile을 쓰므로, appendLog()를 처음 부르기 전(setupConnections()
    // 보다 먼저)에 열어야 함. mkpath()는 이미 폴더가 있어도 안전(무해)함.
    const QString logDirPath = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    QDir().mkpath(logDirPath);
    const QString logFileName = QStringLiteral("ota_%1.log")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    m_logFile.setFileName(logDirPath + QStringLiteral("/") + logFileName);
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qDebug() << "로그 파일:" << m_logFile.fileName();
    } else {
        // 파일을 못 열어도(권한 문제 등) 화면 로그창은 정상 동작해야 하므로
        // 여기서 앱을 막지 않음 — appendLog()가 m_logFile.isOpen()을 확인함.
        qWarning() << "로그 파일을 열지 못함:" << m_logFile.fileName();
    }

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

    // FHSS 상태 표시용 — m_fhssActive일 때만 실제로 getFhssStatus()를 부름
    // (onFhssStatusTick() 안에서 확인), 그 전까진 그냥 매 500ms마다 조용히
    // 리턴만 함. m_tickTimer와 같은 이유로 "필요할 때만 만들고 없애기"보다
    // "계속 돌리고 안에서 거른다"를 택함 — 코드가 더 단순해짐
    m_fhssStatusTimer = new QTimer(this);
    m_fhssStatusTimer->setInterval(500);
    connect(m_fhssStatusTimer, &QTimer::timeout, this, &OtaManager::onFhssStatusTick);
    m_fhssStatusTimer->start();

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
    connect(ui->fhssActivateButton, &QPushButton::clicked, this, &OtaManager::onFhssActivateClicked);
    connect(ui->fhssStopButton, &QPushButton::clicked, this, &OtaManager::onFhssStopClicked);

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
    const bool broadcast = settings.value(QStringLiteral("target/broadcast"), false).toBool();
    // 기본값 없음(빈 문자열) — 코드에 시드를 하드코딩하지 않기 위해서.
    // 사용자가 직접 입력하거나 "무작위 생성"으로 채워야 함
    const QString hopSeed = settings.value(QStringLiteral("fhss/hopSeed"), QString()).toString();
    const int channelCount = settings.value(QStringLiteral("fhss/channelCount"), 8).toInt();
    const int firstChannel = settings.value(QStringLiteral("fhss/firstChannel"), 1).toInt();
    const int generation = settings.value(QStringLiteral("fhss/generation"), 1).toInt();

    ui->portEdit->setText(port);
    if (broadcast)
        ui->broadcastRadio->setChecked(true);
    else
        ui->unicastRadio->setChecked(true);
    ui->hopSeedEdit->setText(hopSeed);
    ui->fhssChannelCountSpin->setValue(channelCount);
    ui->fhssFirstChannelSpin->setValue(firstChannel);
    ui->fhssGenerationSpin->setValue(generation);
}

void OtaManager::saveSettings()
{
    QSettings settings(QStringLiteral("gateway-ota"), QStringLiteral("OtaManager"));
    settings.setValue(QStringLiteral("transport/port"), ui->portEdit->text());
    settings.setValue(QStringLiteral("target/broadcast"), ui->broadcastRadio->isChecked());
    settings.setValue(QStringLiteral("fhss/hopSeed"), ui->hopSeedEdit->text());
    settings.setValue(QStringLiteral("fhss/channelCount"), ui->fhssChannelCountSpin->value());
    settings.setValue(QStringLiteral("fhss/firstChannel"), ui->fhssFirstChannelSpin->value());
    settings.setValue(QStringLiteral("fhss/generation"), ui->fhssGenerationSpin->value());
}

Cc1101Transport *OtaManager::fhssTransport() const
{
    // m_transport의 정적 타입은 ITransport*(하드웨어 종류에 상관없이 쓰려고
    // 일부러 그렇게 선언함, otamanager.h 참고)라서 stopFhss() 같은 CC1101
    // 전용 메서드를 바로 못 부른다 — dynamic_cast로 실제로 가리키는 게
    // Cc1101Transport가 맞는지 확인해서 그 타입의 포인터로 돌려줌. 2026-08-25
    // 기준 onConnectClicked()는 항상 Cc1101Transport만 만들므로 지금은 항상
    // 성공하지만, 나중에 다른 ITransport 구현체가 추가되면 그때는 nullptr이
    // 될 수 있으므로 호출부는 여전히 null 체크를 해야 함.
    return dynamic_cast<Cc1101Transport *>(m_transport.get());
}

void OtaManager::appendLog(const QString &tag, const QString &message)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    const QString line = QStringLiteral("%1 [%2] %3").arg(timestamp, tag, message);
    ui->logView->appendPlainText(line);

    // [2026-08-25] 화면 로그창은 앱이 죽으면(강제종료, killall 등) 같이
    // 사라짐 — "배치 재전송 한도 초과" 같은 실패 원인을 나중에 다시 보려면
    // 파일로도 남아 있어야 함. 매 줄마다 바로 flush해서, 비정상 종료돼도
    // 그 직전까지의 로그는 확실히 디스크에 남게 함(생성자의 m_logFile 주석
    // 참고).
    if (m_logFile.isOpen()) {
        QTextStream out(&m_logFile);
        out.setEncoding(QStringConverter::Utf8);
        out << line << '\n';
        out.flush();
        m_logFile.flush();
    }
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
        if (m_fhssBusy) {
            // FHSS 활성화/중지 워커 스레드가 도는 중 — 같은 이유로 막음
            appendLog(QStringLiteral("WARN"), tr("FHSS 처리가 끝난 뒤 연결을 해제하세요"));
            return;
        }
        if (m_fhssActive) {
            // 연결을 끊기 전에 호핑을 꺼서 칩을 정상 상태(채널 0)로 되돌려둠 —
            // 안 그러면 다음 연결 때도 칩이 계속 호핑 중이라 CONFIG/ACTIVATE가
            // 채널 0에 있는 ESP32에 안 닿음(smoke_fhss_activate_main.cpp 상단
            // 주석과 같은 문제). 결과는 굳이 안 따짐 — 연결 자체를 끊는 게 목적.
            if (Cc1101Transport *cc1101 = fhssTransport())
                (void)cc1101->stopFhss();
            m_fhssActive = false;
            ui->fhssStatusLabel->setText(tr("비활성"));
            ui->fhssActivateButton->setEnabled(true);
            ui->fhssStopButton->setEnabled(false);
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

    // 2026-08-25: 드라이버 선택 콤보 삭제됨 — LocalFileTransport가 끝내
    // 구현되지 않아서 실제로는 항상 CC1101뿐이었다(docs/roadmap.md 마일스톤 3
    // 체크리스트 참고). 나중에 다른 ITransport 구현체가 실제로 생기면 그때
    // 다시 선택 UI를 추가한다.
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
    ui->connectionStatusLabel->setText(tr("연결됨 (CC1101)"));
    ui->connectButton->setText(tr("연결 해제"));
    appendLog(QStringLiteral("INFO"), tr("연결됨: CC1101 / %1").arg(ui->portEdit->text()));
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
    if (m_fhssBusy) {
        // FHSS 활성화/중지 워커 스레드도 같은 이유로 같은 m_transport를 씀
        appendLog(QStringLiteral("WARN"), tr("FHSS 처리가 끝난 뒤 다시 시도하세요"));
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

    // FHSS가 이 대상과 이미 활성화돼 있으면(onFhssActivateClicked() 참고),
    // FHSS_CONFIG/ACTIVATE 때 쓴 session_id를 OtaSession에도 그대로 재사용함
    // — smoke_fhss_ota_transfer_main.cpp 21~23행 주석과 같은 이유("이 실행
    // 한 번이 하나의 세션"이라는 개념을 일관되게 유지, 로그 추적도 쉬움).
    // 0을 넘기면(FHSS 비활성 또는 대상이 다름) OtaSession이 내부적으로
    // 새 session_id를 무작위 생성함(otasession.h start() 기본값 0의 의미).
    const uint32_t sessionIdToReuse =
        (m_fhssActive && targetDeviceId == m_fhssTargetDeviceId) ? m_fhssSessionId : 0;

    m_retransmitEventCount = 0;
    // otasession.h 67~72행 기본값(batchSize=5, timeoutMs=300, maxRetry=5,
    // chunkDelayMs=40) 그대로 씀 — 전부 실기기 검증으로 확정된 값
    // (docs/roadmap.md 3절)
    m_session = std::make_unique<OtaSession>(*m_transport);
    m_session->setOnStateChanged([this](OtaSessionState state) { handleSessionStateChanged(state); });

    if (!m_session->start(m_selectedFilePath.toStdString(), targetDeviceId, sessionIdToReuse)) {
        appendLog(QStringLiteral("ERROR"),
                  tr("전송 시작 실패: %1").arg(QString::fromStdString(m_session->errorMessage())));
        m_session.reset();
        return;
    }

    ui->startButton->setEnabled(false);
    ui->pauseButton->setEnabled(true);
    ui->pauseButton->setText(tr("일시정지"));
    appendLog(QStringLiteral("INFO"),
              tr("전송 시작 (session_id=0x%1, %2%3)")
                  .arg(QString::number(m_session->sessionId(), 16), m_selectedFilePath,
                       sessionIdToReuse != 0 ? tr(", FHSS 호핑 중") : QString()));
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
    // 정확히 맞음 — 실제로 파이/ESP32에 넘길 때 값 변환이 필요 없음.
    //
    // [2026-08-25 갱신] 이 버튼을 만들 당시엔 화면에 값만 채우고 끝이었지만,
    // Task #4(FHSS 섹션 UI + rollout 연동)가 끝난 지금은 onFhssActivateClicked()가
    // hopSeedEdit의 값을 그대로 읽어 FhssHopPolicy::seed로 실제 CONFIG/ACTIVATE
    // 핸드셰이크에 실어 보낸다 — 아래는 그 값을 미리 채워두는 편의 기능이고,
    // "FHSS 활성화"를 누를 때 비어있으면 그때 자동으로도 채워진다(같은 로직).
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
    if (m_fhssBusy) {
        appendLog(QStringLiteral("WARN"), tr("FHSS 처리가 끝난 뒤 조회하세요"));
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
    //
    // [2026-08-25] DISCOVER 전 FHSS 잔류 상태 초기화(이전엔 이 화면 경로에만
    // 빠져 있던 CLI 전용 리셋이었음 — 실기기로 재현된 문제)는 이제
    // discoverDevices() 자신이 CC1101이면 항상 해준다(session/discovery.cpp
    // resetLeftoverFhssStateIfCc1101() 참고). 이 화면은 CLI와 마찬가지로
    // discoverDevices()만 부르면 되고, 리셋을 따로 신경 쓸 필요가 없다 —
    // "CLI로 검증한 로직을 화면도 그대로 쓴다"는 원칙을 지키기 위해 화면
    // 전용으로 따로 만들지 않았다.
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

void OtaManager::onFhssActivateClicked()
{
    if (!m_connected || !m_transport) {
        appendLog(QStringLiteral("WARN"), tr("먼저 연결하세요"));
        return;
    }
    if (m_fhssBusy) {
        appendLog(QStringLiteral("WARN"), tr("이미 FHSS 처리 중입니다"));
        return;
    }
    if (m_discovering) {
        appendLog(QStringLiteral("WARN"), tr("기기 조회가 끝난 뒤 시도하세요"));
        return;
    }
    if (m_session && m_session->state() != OtaSessionState::Idle
        && m_session->state() != OtaSessionState::Completed
        && m_session->state() != OtaSessionState::Failed) {
        appendLog(QStringLiteral("WARN"), tr("전송이 진행 중입니다 — 완료/실패 후에 시도하세요"));
        return;
    }
    if (m_fhssActive) {
        appendLog(QStringLiteral("WARN"), tr("이미 활성화돼 있습니다 — 먼저 \"FHSS 중지\"를 누르세요"));
        return;
    }
    // rolloutFhssConfig()는 기기를 한 번에 하나씩만 처리하는 설계라서
    // (fhssrollout.h 39~45행 주석: ACK에 발신 기기 구분 필드가 없어서 병렬
    // 처리가 불가능함) targetCombo에서 명시적으로 고른 기기 하나가 필요함 —
    // 브로드캐스트나 "아직 조회 안 함" 상태로는 시작할 수 없음.
    if (!ui->unicastRadio->isChecked()) {
        appendLog(QStringLiteral("WARN"), tr("FHSS는 유니캐스트 모드에서 대상을 고른 뒤에만 가능합니다"));
        return;
    }
    const QVariant selected = ui->targetCombo->currentData();
    if (!selected.isValid()) {
        appendLog(QStringLiteral("WARN"),
                  tr("먼저 \"기기 조회 (DISCOVER)\"로 대상을 찾은 뒤 목록에서 선택하세요"));
        return;
    }
    const uint32_t targetDeviceId = selected.toUInt();

    // fhssTransport()가 nullptr이면(지금은 사실상 항상 CC1101이라 안 일어나지만,
    // 나중에 로컬 파일 드라이버가 생기면 그때는 진짜로 nullptr이 됨) FHSS
    // 자체를 쓸 수 없는 드라이버라는 뜻 — 워커 스레드를 만들기 전에 여기서 막음.
    Cc1101Transport *fhssTransportPtr = fhssTransport();
    if (!fhssTransportPtr) {
        appendLog(QStringLiteral("WARN"), tr("이 드라이버는 FHSS를 지원하지 않습니다 (CC1101만 지원)"));
        return;
    }

    // 시드가 비어있으면 여기서 바로 무작위로 채움(onHopSeedRandomClicked()와
    // 같은 로직) — 매번 "무작위 생성"을 따로 누르게 하지 않기 위한 편의.
    if (ui->hopSeedEdit->text().isEmpty()) {
        const quint32 seed = QRandomGenerator::global()->generate();
        ui->hopSeedEdit->setText(QString::number(seed));
        appendLog(QStringLiteral("INFO"), tr("호핑 난수 무작위 생성: %1").arg(seed));
    }
    // setupConnections()의 QRegularExpressionValidator("[0-9]{0,10}")는 자릿수만
    // 막지, uint32_t 범위(~42억) 초과는 안 막아준다("실제 범위 초과 여부는
    // 쓰는 시점에 확인" — 그 주석이 가리키던 바로 이 시점) — 여기서 확인.
    bool seedOk = false;
    const uint32_t seed = ui->hopSeedEdit->text().toUInt(&seedOk);
    if (!seedOk) {
        appendLog(QStringLiteral("WARN"),
                  tr("호핑 난수가 uint32 범위(0~4294967295)를 벗어났습니다 — 값을 확인하세요"));
        return;
    }
    const uint8_t channelCount = static_cast<uint8_t>(ui->fhssChannelCountSpin->value());
    const uint8_t firstChannel = static_cast<uint8_t>(ui->fhssFirstChannelSpin->value());
    const uint32_t generation = static_cast<uint32_t>(ui->fhssGenerationSpin->value());
    // FHSS_CONFIG/ACTIVATE 핸드셰이크와 그 다음 OtaSession::start() 양쪽에서
    // 같은 session_id를 쓰기 위해 여기서 미리 뽑아둠(smoke_fhss_ota_transfer_main.cpp
    // 21~23행 주석과 같은 이유) — GUI 스레드에서 뽑아서 워커 스레드로 값만
    // 넘기고, 성공하면 handleFhssActivationResult()에서 m_fhssSessionId에 저장함.
    const uint32_t sessionId = QRandomGenerator::global()->generate();

    m_fhssBusy = true;
    ui->fhssActivateButton->setEnabled(false);
    ui->fhssActivateButton->setText(tr("활성화 중..."));
    ui->fhssStatusLabel->setText(tr("CONFIG/ACTIVATE 전송 중..."));
    appendLog(QStringLiteral("INFO"),
              tr("FHSS 활성화 시작: target=0x%1 channel_count=%2 first_channel=%3 generation=%4")
                  .arg(QString::number(targetDeviceId, 16))
                  .arg(channelCount)
                  .arg(firstChannel)
                  .arg(generation));

    // rolloutFhssConfig()도 discoverDevices()처럼 blocking(재시도까지 포함하면
    // 최악의 경우 timeoutMs*maxRetry*2단계만큼 걸림, 기본값 300ms*5*2=3초)이고,
    // 그 뒤 이어지는 kFhssSyncSettleMs(2000ms) 대기까지 있어서 훨씬 더 김 —
    // onDiscoverClicked()와 똑같은 이유로 QThread::create() 워커에서 돌림.
    // Cc1101Transport*로 캡처(ITransport*가 아님) — stopFhss() 등 FHSS 전용
    // 메서드가 ITransport 인터페이스엔 없어서 ITransport*로는 호출이 아예
    // 컴파일이 안 됨(fhssTransport() 주석 참고).
    Cc1101Transport *transport = fhssTransportPtr;
    QThread *worker = QThread::create([this, transport, targetDeviceId, sessionId, seed,
                                        channelCount, firstChannel, generation]() {
        const auto logLine = [this](const QString &msg) {
            QMetaObject::invokeMethod(
                this, [this, msg]() { appendLog(QStringLiteral("FHSS"), msg); }, Qt::QueuedConnection);
        };

        // [smoke_fhss_activate_main.cpp 127~166행과 동일한 순서] 이전 실행이
        // 호핑 상태를 남겨뒀을 수 있으므로 CONFIG를 보내기 전에 항상 정리 —
        // stopFhss()는 내부적으로 채널을 reserved_channel(0)로 되돌리지만,
        // 명시적으로 setChannel(0)도 한 번 더 해서 상태를 확실히 맞춤.
        (void)transport->stopFhss();
        if (transport->setChannel(0) != Cc1101Status::Ok)
            logLine(QStringLiteral("setChannel(0) 실패 — 계속 진행"));
        if (transport->startRx() != Cc1101Status::Ok)
            logLine(QStringLiteral("startRx 실패 — 계속 진행"));

        FhssHopPolicy policy;
        policy.generation = generation;
        policy.algorithmVersion = 1; // OTA_FHSS_ALGORITHM_VERSION
        policy.channelProfileId = 0;
        policy.firstChannel = firstChannel;
        policy.rendezvousChannel = firstChannel; // 프로토콜 검증 조건: rendezvous == first
        policy.channelCount = channelCount;
        policy.reservedChannel = 0; // OTA 전용 채널 0 — 호핑 범위에서 제외
        policy.seed = seed;
        policy.slotDurationUs = 300000;       // smoke_fhss_activate_main.cpp와 동일값(hop_policy 주석 기준)
        policy.channelSwitchGuardUs = 5000;   // 위와 동일

        const auto onRolloutLog = [&logLine](const std::string &msg) {
            logLine(QStringLiteral("rollout: ") + QString::fromStdString(msg));
        };
        const auto outcomes = rolloutFhssConfig(*transport, sessionId, {targetDeviceId}, policy,
                                                 300, 5, onRolloutLog);
        if (outcomes.empty() || outcomes.front().stage != FhssRolloutStage::Activated) {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    handleFhssActivationResult(false, 0, tr("FHSS_CONFIG/ACTIVATE 실패 — 대상이 MENU_OTA 화면인지 확인하세요"));
                },
                Qt::QueuedConnection);
            return;
        }
        logLine(QStringLiteral("CONFIG/ACTIVATE 성공. Gateway 커널 호핑(MASTER) 시작..."));

        // [RF 프로필 — smoke_fhss_activate_main.cpp/smoke_fhss_ota_transfer_main.cpp와
        // 동일값, 2026-08-22 실기기로 확정됨] 무선 레벨 상수라 화면 입력값이 아니라
        // 여기 고정값을 씀 — ESP32(firmware-esp32의 rf_transport.c) 쪽 값과 반드시
        // 일치해야 하고, 팀 전체가 공유하는 값이라 사용자가 화면에서 바꿀 이유가 없음.
        Cc1101FhssConfig kernelConfig;
        kernelConfig.generation = generation;
        kernelConfig.algorithmId = 1; // CC1101_FHSS_ALGORITHM_SEEDED_PERMUTATION
        kernelConfig.rfBaseFreqHz = 433919830u;
        kernelConfig.rfChannelSpacingHz = 199951u;
        kernelConfig.rfSyncWord = 0xD391u;
        kernelConfig.rfMdmcfg4 = 0xCA;
        kernelConfig.rfMdmcfg3 = 0x83;
        kernelConfig.rfPktctrl1 = 0x04;
        kernelConfig.rfPktctrl0 = 0x05;
        kernelConfig.seed = policy.seed;
        kernelConfig.slotDurationUs = policy.slotDurationUs;
        kernelConfig.channelSwitchGuardUs = policy.channelSwitchGuardUs;
        kernelConfig.channelCount = policy.channelCount;
        kernelConfig.firstChannel = policy.firstChannel;
        kernelConfig.rendezvousChannel = policy.rendezvousChannel;
        kernelConfig.reservedChannel = policy.reservedChannel;
        kernelConfig.algorithmVersion = policy.algorithmVersion;
        kernelConfig.channelProfileId = policy.channelProfileId;

        if (transport->configureFhss(kernelConfig) != Cc1101Status::Ok) {
            QMetaObject::invokeMethod(
                this,
                [this]() { handleFhssActivationResult(false, 0, tr("configureFhss() 실패")); },
                Qt::QueuedConnection);
            return;
        }
        if (transport->startFhss(Cc1101FhssRole::Master) != Cc1101Status::Ok) {
            QMetaObject::invokeMethod(
                this,
                [this]() { handleFhssActivationResult(false, 0, tr("startFhss(MASTER) 실패")); },
                Qt::QueuedConnection);
            return;
        }

        // ESP32가 랑데부 채널에서 SYNC_ACQUIRED까지 가는 데 보통 1~2초 걸림
        // (smoke_fhss_ota_transfer_main.cpp 76~81행 주석과 같은 값/이유) —
        // 그 전에 OTA_START를 보내면 응답이 없을 수 있어서 여유를 두고 기다림.
        constexpr int kFhssSyncSettleMs = 2000;
        QThread::msleep(kFhssSyncSettleMs);

        const auto status = transport->getFhssStatus();
        const QString message =
            status.synchronized
                ? tr("활성화 완료 — 동기화됨 (channel=%1)").arg(status.currentChannel)
                : tr("활성화는 됐지만 아직 동기화 전 (channel=%1) — 잠시 후 상태를 다시 확인하세요")
                      .arg(status.currentChannel);
        QMetaObject::invokeMethod(
            this,
            [this, sessionId, message]() { handleFhssActivationResult(true, sessionId, message); },
            Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();

    // targetDeviceId는 워커 완료 후 handleFhssActivationResult()에서
    // m_fhssTargetDeviceId에 저장해야 하는데, 그 함수는 성공 여부만 받으므로
    // 여기서 미리 저장해둠(실패해도 남아있는 게 무해함 — m_fhssActive가
    // false인 한 onStartClicked()의 session_id 재사용 조건에서 안 걸림).
    m_fhssTargetDeviceId = targetDeviceId;
}

void OtaManager::onFhssStopClicked()
{
    if (!m_fhssActive) {
        appendLog(QStringLiteral("WARN"), tr("활성화된 FHSS가 없습니다"));
        return;
    }
    if (m_fhssBusy) {
        appendLog(QStringLiteral("WARN"), tr("FHSS 처리가 끝난 뒤 시도하세요"));
        return;
    }
    if (m_session && m_session->state() != OtaSessionState::Idle
        && m_session->state() != OtaSessionState::Completed
        && m_session->state() != OtaSessionState::Failed) {
        appendLog(QStringLiteral("WARN"), tr("전송이 진행 중입니다 — 완료/실패 후에 시도하세요"));
        return;
    }
    Cc1101Transport *transport = fhssTransport();
    if (!transport) {
        appendLog(QStringLiteral("WARN"), tr("연결이 없습니다"));
        return;
    }

    // stopFhss()/setChannel()/startRx()는 전부 즉시 반환하는 ioctl 호출이라
    // (discoverDevices()/rolloutFhssConfig()처럼 몇백ms~몇 초씩 기다리는 게
    // 아님) 워커 스레드 없이 GUI 스레드에서 그냥 동기 호출해도 창이 안 얼어붙음.
    const Cc1101Status result = transport->stopFhss();
    (void)transport->setChannel(0);
    (void)transport->startRx();

    m_fhssActive = false;
    ui->fhssActivateButton->setEnabled(true);
    ui->fhssActivateButton->setText(tr("FHSS 활성화"));
    ui->fhssStopButton->setEnabled(false);
    ui->fhssStatusLabel->setText(tr("비활성"));
    appendLog(QStringLiteral("INFO"),
              tr("FHSS 중지됨 (stopFhss 결과 코드=%1)").arg(static_cast<int>(result)));
}

void OtaManager::onFhssStatusTick()
{
    // 활성화 워커가 도는 중이거나 애초에 비활성 상태면 커널에 물어볼 필요
    // 없음. 세션이 활성 전송 중일 때도 건너뜀 — GUI 스레드는 하나뿐이라
    // 데이터 레이스는 안 나지만(onSessionTick()도 같은 스레드), 전송
    // 중에는 send()/recv() 타이밍이 더 중요하므로 불필요한 ioctl 트래픽을
    // 안 섞으려는 것(smoke_fhss_ota_transfer_main.cpp도 전송 중엔 상태를
    // 안 찍음).
    Cc1101Transport *transport = fhssTransport();
    if (!m_fhssActive || m_fhssBusy || !transport)
        return;
    if (m_session && m_session->state() != OtaSessionState::Idle
        && m_session->state() != OtaSessionState::Completed
        && m_session->state() != OtaSessionState::Failed)
        return;

    const auto status = transport->getFhssStatus();
    ui->fhssStatusLabel->setText(
        tr("활성 · %1 · channel=%2 · sync_packets=%3 · sync_misses=%4")
            .arg(status.synchronized ? tr("동기화됨") : tr("동기화 대기"))
            .arg(status.currentChannel)
            .arg(status.syncPackets)
            .arg(status.syncMisses));
}

void OtaManager::handleFhssActivationResult(bool activated, uint32_t sessionId, const QString &message)
{
    m_fhssBusy = false;
    ui->fhssActivateButton->setText(tr("FHSS 활성화"));

    if (!activated) {
        ui->fhssActivateButton->setEnabled(true);
        ui->fhssStopButton->setEnabled(false);
        ui->fhssStatusLabel->setText(tr("비활성 (실패)"));
        appendLog(QStringLiteral("ERROR"), message);
        return;
    }

    m_fhssActive = true;
    m_fhssSessionId = sessionId;
    // m_fhssTargetDeviceId는 onFhssActivateClicked() 끝에서 이미 저장해둠
    ui->fhssActivateButton->setEnabled(false); // 재설정하려면 먼저 중지해야 함
    ui->fhssStopButton->setEnabled(true);
    ui->fhssStatusLabel->setText(message);
    appendLog(QStringLiteral("INFO"), tr("FHSS 활성화 성공 (session_id=0x%1): %2")
                                           .arg(QString::number(sessionId, 16), message));

    // generation은 "오래된 설정 거르기" 용도라, 재활성화할 때 같은 값을
    // 실수로 재사용하지 않도록 성공할 때마다 자동으로 1 올려둠.
    ui->fhssGenerationSpin->setValue(ui->fhssGenerationSpin->value() + 1);
}
