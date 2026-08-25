#ifndef TEELOGGER_H
#define TEELOGGER_H

// [2026-08-25] 실기기 스모크테스트 CLI들(ota_smoke_*)이 지금까지
// std::cout/std::cerr로만 출력해서, 실행할 때 사람이 직접
// "> gw_log_....txt"로 리다이렉트하지 않으면 로그가 하나도 안 남았다.
// 실제로 148 실기기 테스트에서 이 문제로 로그가 없어서("지금 ESP 로그는
// 21시 19분 실행인데 거기 대응하는 Gateway 로그가 없다") 원인 진단이
// 늦어진 적이 있었다.
//
// 이 헤더는 std::cout/std::cerr의 내부 버퍼(streambuf)를 화면 출력은
// 그대로 유지하면서 파일에도 동시에 기록하는 TeeStreambuf로 바꿔치기
// 한다 — 기존 CLI들의 std::cout << ... 코드는 한 줄도 안 건드려도 됨
// (Qt 쪽 otamanager.cpp의 appendLog()에 파일 저장을 추가한 것과 같은
// 목적, 같은 이유 — 그쪽은 화면 로그창 하나만 거치면 돼서 직접 넣었지만
// CLI들은 std::cout 호출이 파일마다 수십 곳에 흩어져 있어서 스트림
// 버퍼를 바꿔치기하는 방식을 씀).
//
// 사용법 (main() 맨 앞, 첫 로그 출력 전에 딱 한 줄):
//   TeeLogger logger("gw_log");  // gw_log_YYYYMMDD_HHMMSS.txt 자동 생성
// logger 객체가 스코프를 벗어나면(보통 main() 끝) 소멸자가 자동으로
// std::cout/cerr을 원래 상태로 복구한다 — main() 중간의 어느 return
// 문에서 나가도(스택 되감기로 소멸자가 항상 불림) 안전함.
//
// 여러 스모크테스트 CLI가 공유하는 유틸이라 tests/ 아래 header-only로
// 둠(신규 .cpp가 아니라서 CMakeLists.txt에 항목을 안 추가해도 됨) — 다른
// smoke_*_main.cpp에도 필요하면 이 헤더를 그대로 include해서 쓰면 된다.

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>

class TeeStreambuf : public std::streambuf
{
public:
    TeeStreambuf(std::streambuf *console, std::streambuf *file) : m_console(console), m_file(file) {}

protected:
    int overflow(int ch) override
    {
        if (ch == EOF)
            return !EOF;
        const int r1 = m_console ? m_console->sputc(static_cast<char>(ch)) : ch;
        const int r2 = m_file ? m_file->sputc(static_cast<char>(ch)) : ch;
        return (r1 == EOF || r2 == EOF) ? EOF : ch;
    }

    int sync() override
    {
        const int r1 = m_console ? m_console->pubsync() : 0;
        const int r2 = m_file ? m_file->pubsync() : 0;
        return (r1 == 0 && r2 == 0) ? 0 : -1;
    }

private:
    std::streambuf *m_console;
    std::streambuf *m_file;
};

class TeeLogger
{
public:
    // prefix: 로그 파일 이름 접두어 — "gw_log"면 gw_log_YYYYMMDD_HHMMSS.txt
    // (148 실기기 로그에서 이미 쓰이고 있던 이름 관례를 그대로 맞춤)
    explicit TeeLogger(const std::string &prefix)
    {
        char nameBuf[128];
        const std::time_t now = std::time(nullptr);
        std::tm tmNow{};
        localtime_r(&now, &tmNow);
        std::snprintf(nameBuf, sizeof(nameBuf), "%s_%04d%02d%02d_%02d%02d%02d.txt", prefix.c_str(),
                      tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday, tmNow.tm_hour,
                      tmNow.tm_min, tmNow.tm_sec);
        m_fileName = nameBuf;
        m_file.open(m_fileName);
        if (!m_file.is_open()) {
            // 파일을 못 열어도(권한 문제 등) 화면 출력은 그대로 진행돼야 하므로
            // 여기서 프로그램을 막지 않음 — rdbuf를 안 바꿔치기하고 그냥 리턴.
            std::cerr << "[TeeLogger] 로그 파일을 열지 못함(" << m_fileName << ") — 화면 출력만 진행\n";
            return;
        }
        m_coutOrig = std::cout.rdbuf();
        m_cerrOrig = std::cerr.rdbuf();
        m_coutTee = new TeeStreambuf(m_coutOrig, m_file.rdbuf());
        m_cerrTee = new TeeStreambuf(m_cerrOrig, m_file.rdbuf());
        std::cout.rdbuf(m_coutTee);
        std::cerr.rdbuf(m_cerrTee);
        std::cout << "[TeeLogger] 로그 파일: " << m_fileName << "\n";
    }

    ~TeeLogger()
    {
        if (m_coutOrig)
            std::cout.rdbuf(m_coutOrig);
        if (m_cerrOrig)
            std::cerr.rdbuf(m_cerrOrig);
        delete m_coutTee;
        delete m_cerrTee;
    }

    TeeLogger(const TeeLogger &) = delete;
    TeeLogger &operator=(const TeeLogger &) = delete;

    const std::string &fileName() const { return m_fileName; }

private:
    std::string m_fileName;
    std::ofstream m_file;
    std::streambuf *m_coutOrig = nullptr;
    std::streambuf *m_cerrOrig = nullptr;
    TeeStreambuf *m_coutTee = nullptr;
    TeeStreambuf *m_cerrTee = nullptr;
};

#endif // TEELOGGER_H
