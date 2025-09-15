/*
 * tester.cpp
 * ----------
 * Automated test runner for YOUR protocol (lowercase "<input>" markers).
 *
 * Scenarios:
 *  1) Happy path: complete one joke; answer N.
 *  2) Wrong first response: expect correction + immediate "Knock knock! <input>".
 *  3) Wrong second response: expect correction + restart.
 *  4) Concurrent clients (default: 3).
 *  5) Idle shutdown: wait ~12s; verify server refuses new connection after its 10s idle timeout.
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -O2 tester.cpp -o tester
 *
 * Run:
 *   ./server                 # terminal 1
 *   ./tester [host] [port]   # terminal 2 (defaults: 127.0.0.1 8079)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std;

static const int READ_TIMEOUT_MS = 7000;
static const int PORT_DEFAULT    = 8079;

// ---------------------------- socket helpers ----------------------------

static bool set_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

static int connect_to(const string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    sockaddr_in serv{};
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &serv.sin_addr) <= 0) {
        cerr << "[runner] invalid addr\n";
        ::close(fd);
        return -1;
    }
    if (::connect(fd, (sockaddr*)&serv, sizeof(serv)) < 0) {
        ::close(fd);
        return -1;
    }
    set_timeout(fd, READ_TIMEOUT_MS);
    return fd;
}

static bool send_line(int fd, const string& s) {
    string out = s;
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    const char* p = out.data();
    size_t left   = out.size();
    while (left) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p    += n;
        left -= size_t(n);
    }
    return true;
}

static bool recv_line(int fd, string& line) {
    line.clear();
    char ch;
    while (true) {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) return false;      // timeout/EOF/error
        if (ch == '\r') continue;
        if (ch == '\n') break;
        line.push_back(ch);
        if (line.size() > 8192) break; // safety
    }
    return true;
}

/* Read lines until one contains "<input>". Returns the prompt line in `line`. */
static bool read_until_prompt(int fd, string& line) {
    while (true) {
        if (!recv_line(fd, line)) return false;
        cout << "[S] " << line << "\n";
        if (line.find("<input>") != string::npos) return true;
    }
}

/* Remove "<input>" marker and trim trailing whitespace. */
static string strip_marker(string s) {
    auto pos = s.find("<input>");
    if (pos != string::npos) s.erase(pos, 7);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// ------------------------------- scenarios ------------------------------

static bool scenario_happy(const string& host, int port) {
    cout << "\n[TEST] happy path\n";
    int fd = connect_to(host, port);
    if (fd < 0) { cerr << "connect failed\n"; return false; }

    string line;

    // Knock knock <input>
    if (!read_until_prompt(fd, line) || line.find("Knock knock!") == string::npos) {
        cerr << "did not get 'Knock knock! <input>'\n"; ::close(fd); return false;
    }
    if (!send_line(fd, "Who's there?")) { ::close(fd); return false; }

    // Setup <input>
    if (!read_until_prompt(fd, line)) { cerr << "no setup prompt\n"; ::close(fd); return false; }
    string setup = strip_marker(line);
    string setup_word = setup;
    auto sp = setup_word.find(' ');
    if (sp != string::npos) setup_word.erase(sp);
    if (!send_line(fd, setup_word + " who?")) { ::close(fd); return false; }

    // Punchline
    if (!recv_line(fd, line)) { cerr << "no punchline\n"; ::close(fd); return false; }
    cout << "[S] " << line << "\n";

    // Y/N <input>
    if (!read_until_prompt(fd, line) || line.find("(Y/N)") == string::npos) {
        cerr << "no Y/N prompt\n"; ::close(fd); return false;
    }
    if (!send_line(fd, "N")) { ::close(fd); return false; }

    ::close(fd);
    cout << "[OK] happy path\n";
    return true;
}

static bool scenario_wrong_first(const string& host, int port) {
    cout << "\n[TEST] wrong first line -> correction\n";
    int fd = connect_to(host, port);
    if (fd < 0) { cerr << "connect failed\n"; return false; }

    string line;

    // Wrong reply to first prompt
    if (!read_until_prompt(fd, line) || line.find("Knock knock!") == string::npos) {
        cerr << "did not get initial knock prompt\n"; ::close(fd); return false;
    }
    if (!send_line(fd, "Who there?")) { ::close(fd); return false; }

    // Should get correction + immediate fresh "Knock knock! <input>"
    if (!recv_line(fd, line) || line.find("You are supposed to say") == string::npos) {
        cerr << "no correction for first step\n"; ::close(fd); return false;
    }
    cout << "[S] " << line << "\n";

    if (!recv_line(fd, line) || line.find("Knock knock!") == string::npos || line.find("<input>") == string::npos) {
        cerr << "no immediate fresh Knock knock after correction\n"; ::close(fd); return false;
    }
    cout << "[S] " << line << "\n";

    // Do it correctly now
    if (!send_line(fd, "Who's there?")) { ::close(fd); return false; }
    if (!read_until_prompt(fd, line)) { cerr << "no setup prompt\n"; ::close(fd); return false; }
    string setup = strip_marker(line);
    string setup_word = setup; auto sp = setup_word.find(' '); if (sp != string::npos) setup_word.erase(sp);
    if (!send_line(fd, setup_word + " who?")) { ::close(fd); return false; }
    if (!recv_line(fd, line)) { cerr << "no punchline\n"; ::close(fd); return false; }
    cout << "[S] " << line << "\n";
    if (!read_until_prompt(fd, line)) { cerr << "no Y/N prompt\n"; ::close(fd); return false; }
    if (!send_line(fd, "N")) { ::close(fd); return false; }

    ::close(fd);
    cout << "[OK] wrong-first correction\n";
    return true;
}

static bool scenario_wrong_second(const string& host, int port) {
    cout << "\n[TEST] wrong second line -> correction + restart\n";
    int fd = connect_to(host, port);
    if (fd < 0) { cerr << "connect failed\n"; return false; }

    string line;

    // Correct first reply
    if (!read_until_prompt(fd, line) || line.find("Knock knock!") == string::npos) {
        cerr << "did not get initial knock\n"; ::close(fd); return false;
    }
    if (!send_line(fd, "Who's there?")) { ::close(fd); return false; }

    // Setup -> deliberately wrong "<setup> whoo?"
    if (!read_until_prompt(fd, line)) { cerr << "no setup prompt\n"; ::close(fd); return false; }
    string setup = strip_marker(line);
    string setup_word = setup; auto sp = setup_word.find(' '); if (sp != string::npos) setup_word.erase(sp);
    if (!send_line(fd, setup_word + " whoo?")) { ::close(fd); return false; }

    // Expect correction, then restart from knock knock
    if (!recv_line(fd, line) || line.find("You are supposed to say") == string::npos) {
        cerr << "no correction for second step\n"; ::close(fd); return false;
    }
    cout << "[S] " << line << "\n";

    if (!recv_line(fd, line) || line.find("Knock knock!") == string::npos || line.find("<input>") == string::npos) {
        cerr << "did not restart with Knock knock! after wrong second\n"; ::close(fd); return false;
    }
    cout << "[S] " << line << "\n";

    // Finish correctly
    if (!send_line(fd, "Who's there?")) { ::close(fd); return false; }
    if (!read_until_prompt(fd, line)) { cerr << "no setup prompt after restart\n"; ::close(fd); return false; }
    setup = strip_marker(line);
    setup_word = setup; sp = setup_word.find(' '); if (sp != string::npos) setup_word.erase(sp);
    if (!send_line(fd, setup_word + " who?")) { ::close(fd); return false; }
    if (!recv_line(fd, line)) { cerr << "no punchline after restart\n"; ::close(fd); return false; }
    cout << "[S] " << line << "\n";
    if (!read_until_prompt(fd, line)) { cerr << "no Y/N prompt\n"; ::close(fd); return false; }
    if (!send_line(fd, "N")) { ::close(fd); return false; }

    ::close(fd);
    cout << "[OK] wrong-second correction\n";
    return true;
}

static bool scenario_concurrent(const string& host, int port, int nclients = 3) {
    cout << "\n[TEST] concurrent (" << nclients << " clients)\n";
    vector<thread> ths;
    mutex err_mtx;
    bool ok = true;

    auto job = [&](int id) {
        int fd = connect_to(host, port);
        if (fd < 0) { lock_guard<mutex> lk(err_mtx); ok = false; cerr << "[C" << id << "] connect failed\n"; return; }
        string line;

        if (!read_until_prompt(fd, line) || line.find("Knock knock!") == string::npos) { lock_guard<mutex> lk(err_mtx); ok=false; cerr<<"[C"<<id<<"] no knock\n"; ::close(fd); return; }
        send_line(fd, "Who's there?");

        if (!read_until_prompt(fd, line)) { lock_guard<mutex> lk(err_mtx); ok=false; cerr<<"[C"<<id<<"] no setup\n"; ::close(fd); return; }
        string setup = strip_marker(line);
        string setup_word = setup; auto sp = setup_word.find(' '); if (sp != string::npos) setup_word.erase(sp);
        send_line(fd, setup_word + " who?");

        if (!recv_line(fd, line)) { lock_guard<mutex> lk(err_mtx); ok=false; cerr<<"[C"<<id<<"] no punchline\n"; ::close(fd); return; }

        if (!read_until_prompt(fd, line)) { lock_guard<mutex> lk(err_mtx); ok=false; cerr<<"[C"<<id<<"] no YN\n"; ::close(fd); return; }
        send_line(fd, "N");
        ::close(fd);
    };

    for (int i = 0; i < nclients; ++i) ths.emplace_back(job, i);
    for (auto& t : ths) t.join();

    if (ok) cout << "[OK] concurrent clients\n";
    return ok;
}

// Wait ~12s after last clients; new connection should fail if server auto-shut down.
static bool scenario_idle_shutdown_check(const string& host, int port) {
    cout << "\n[TEST] idle shutdown (expect server to exit ~10s after last client)\n";
    this_thread::sleep_for(chrono::seconds(12));
    int fd = connect_to(host, port);
    if (fd >= 0) {
        cout << "[FAIL] server still accepts connections after idle timeout\n";
        ::close(fd);
        return false;
    }
    cout << "[OK] server refused new connection after idle timeout (likely shut down)\n";
    return true;
}

// --------------------------------- main ---------------------------------

int main(int argc, char** argv) {
    string host = "127.0.0.1";
    int    port = PORT_DEFAULT;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = stoi(argv[2]);

    bool ok = true;
    ok &= scenario_happy(host, port);
    ok &= scenario_wrong_first(host, port);
    ok &= scenario_wrong_second(host, port);
    ok &= scenario_concurrent(host, port, 3);
    ok &= scenario_idle_shutdown_check(host, port);

    cout << "\n========== SUMMARY ==========\n";
    if (ok) { cout << "ALL TESTS PASSED ✅\n"; return 0; }
    else    { cout << "SOME TESTS FAILED ❌\n"; return 1; }
}
