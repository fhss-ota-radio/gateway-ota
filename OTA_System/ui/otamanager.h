#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include <QMainWindow>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui {
class OtaManager;
}
class QCloseEvent;
QT_END_NAMESPACE

class OtaManager : public QMainWindow
{
    Q_OBJECT

public:
    explicit OtaManager(QWidget *parent = nullptr);
    ~OtaManager() override;

protected:
    // 창을 닫을 때 마지막 설정값(포트/청크크기/전송모드)을 저장하기 위해 오버라이드
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onConnectClicked();
    void onModeChanged();
    void onSelectFileClicked();
    void onChunkSizeChanged(int value);
    void onStartClicked();
    void onPauseClicked();

private:
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString &tag, const QString &message);
    void recalcChunkInfo();

    // otamanager.ui에서 setupUi()가 채워주는 위젯 트리 (driverCombo, portEdit,
    // connectButton, unicastRadio/broadcastRadio, targetCombo, selectFileButton,
    // chunkSizeSpin, progressBar, logView 등은 전부 Ui::OtaManager의 멤버)
    Ui::OtaManager *ui;

    bool m_connected = false;
    QString m_selectedFilePath;
    qint64 m_selectedFileSize = 0;
};
#endif // OTAMANAGER_H
