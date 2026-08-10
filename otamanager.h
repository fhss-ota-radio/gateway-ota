#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include "binsplitter.h"

#include <QMainWindow>
#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

QT_BEGIN_NAMESPACE
namespace Ui {
class OtaManager;
}
class QCloseEvent;
class QTimer;
QT_END_NAMESPACE

class Cc1101Transport;

class OtaManager : public QMainWindow
{
    Q_OBJECT

public:
    explicit OtaManager(QWidget *parent = nullptr);
    ~OtaManager() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onConnectClicked();
    void onModeChanged();
    void onSelectFileClicked();
    void onChunkSizeChanged(int value);
    void onStartClicked();
    void onPauseClicked();
    void sendNextChunk();

private:
    void setupConnections();
    void loadSettings();
    void saveSettings();
    void appendLog(const QString &tag, const QString &message);
    void recalcChunkInfo();
    void setConnectedUi(bool connected);
    void finishTransfer(bool success, const QString &message);

    Ui::OtaManager *ui;

    std::unique_ptr<Cc1101Transport> m_transport;

    bool m_connected = false;
    bool m_transferring = false;
    bool m_paused = false;

    QString m_selectedFilePath;
    qint64 m_selectedFileSize = 0;

    std::vector<OtaChunk> m_chunks;
    std::size_t m_nextChunk = 0;

    QTimer *m_sendTimer = nullptr;
};

#endif // OTAMANAGER_H
