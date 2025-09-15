/*
 * server.cpp
 * -----------
 * Multi-client knock-knock joke server with:
 *  - SQLite-backed "database" of jokes (table `jokes(setup, punchline)`).
 *  - Strict, case-insensitive-but-spelling-sensitive protocol:
 *      Server: "Knock knock! <input>"
 *      Client: "Who's there?"
 *      Server: "<setup> <input>"
 *      Client: "<setup> who?"
 *      Server: "<punchline>"
 *      Server: "Would you like to listen to another? (Y/N) <input>"
 *  - Robust error handling: if client says the wrong thing, the server explains
 *    what to say and restarts the joke from the beginning immediately.
 *  - Parallel clients (pthreads).
 *  - Graceful termination: when active_clients == 0 for 10 seconds, the server exits.
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -O2 -pthread server.cpp -lsqlite3 -o server
 */

#include <sqlite3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std;

constexpr int PORT        = 8079;  // server port
constexpr int MAX_CLIENTS = 10;    // listen backlog & rough concurrency cap

// ------------------------------ Joke model ------------------------------

struct Joke {
    string setup;
    string punchline;
};

// Global in-memory list populated from SQLite at startup
static vector<Joke> jokes;

/*
 * SQLite row callback: appends each (setup, punchline) row to `jokes`.
 * `data` and `azColName` are not needed here; explicitly mark them unused to
 * silence -Wunused-parameter warnings.
 */
static int load_callback(void* /*unused*/,
                         int argc,
                         char** argv,
                         char** /*unused*/) {
    if (argc == 2) {
        Joke j{argv[0] ? argv[0] : "", argv[1] ? argv[1] : ""};
        jokes.push_back(std::move(j));
    }
    return 0;
}

/*
 * Load jokes from `filename` into global `jokes`.
 * Table schema: CREATE TABLE jokes (id INTEGER PRIMARY KEY, setup TEXT, punchline TEXT);
 */
static void load_jokes_from_db(const string& filename) {
    sqlite3* db = nullptr;
    if (sqlite3_open(filename.c_str(), &db)) {
        cerr << "Can't open database: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close(db);
        return;
    }

    const char* sql = "SELECT setup, punchline FROM jokes;";
    char* errmsg = nullptr;
    if (sqlite3_exec(db, sql, load_callback, nullptr, &errmsg) != SQLITE_OK) {
        cerr << "SQL error: " << (errmsg ? errmsg : "unknown") << "\n";
        sqlite3_free(errmsg);
    }

    sqlite3_close(db);
}

// --------------------------- Per-client session -------------------------

struct ClientSession {
    int fd = -1;                     // connected socket
    set<size_t> told_jokes;          // joke indices already told to this client
    mt19937 rng;                     // RNG for random joke order
    sockaddr_in client_addr{};       // for logging
};

// ------------------------------- Globals --------------------------------

static int listen_fd = -1;
static atomic<bool> server_running{true};
static atomic<int>  active_clients{0};

// ----------------------------- I/O utilities ----------------------------

/* Send a full line ending with '\n' (appends newline if missing). */
static bool send_line(int fd, const string& s) {
    string out = s;
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    const char* p = out.data();
    size_t left = out.size();
    while (left) {
        ssize_t n = ::send(fd, p, left, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p += n; left -= static_cast<size_t>(n);
    }
    return true;
}

/* Receive exactly one line (up to '\n'); strips '\r'. Returns false on EOF/error. */
static bool recv_line(int fd, string& line) {
    line.clear();
    char ch;
    while (true) {
        ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) return false;  // EOF/timeout/error
        if (ch == '\r') continue;
        if (ch == '\n') break;
        line.push_back(ch);
        if (line.size() > 4096) break;  // safety guard
    }
    return true;
}

/* Trim leading/trailing whitespace. */
static string trim(const string& s) {
    size_t i = s.find_first_not_of(" \t\r\n");
    if (i == string::npos) return "";
    size_t j = s.find_last_not_of(" \t\r\n");
    return s.substr(i, j - i + 1);
}

/* Lowercase a string (unsigned-safe). */
static string lower(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

/* Case-insensitive equality after trimming; spelling-sensitive (no fuzzy match). */
static bool iequals(const string& a, const string& b) {
    return lower(trim(a)) == lower(trim(b));
}

// --------------------------- Knock-knock logic --------------------------

/*
 * Drive exactly one *complete* knock-knock exchange.
 * Returns true if the joke completed; false if:
 *   - there are no more jokes for this client, OR
 *   - the connection failed during the exchange.
 */
static bool play_joke(ClientSession* session) {
    // Build list of jokes that haven't been told to this client
    vector<size_t> avail;
    avail.reserve(jokes.size());
    for (size_t i = 0; i < jokes.size(); ++i) {
        if (!session->told_jokes.count(i)) avail.push_back(i);
    }

    if (avail.empty()) {
        send_line(session->fd, "I have no more jokes to tell.");
        return false;  // session ends
    }

    // Select a random unused joke
    uniform_int_distribution<size_t> dist(0, avail.size() - 1);
    size_t idx = avail[dist(session->rng)];
    session->told_jokes.insert(idx);

    const Joke& jk = jokes[idx];
    const string expect1 = "Who's there?";
    const string expect2 = jk.setup + " who?";

    // Step 1: "Knock knock!"
    if (!send_line(session->fd, "Knock knock! <input>")) return false;
    string resp;
    while (true) {
        if (!recv_line(session->fd, resp)) return false;
        if (iequals(resp, expect1)) break;  // good
        // incorrect -> explain and immediately restart from the beginning
        if (!send_line(session->fd, "You are supposed to say, \"Who's there?\". Let's try again.")) return false;
        if (!send_line(session->fd, "Knock knock! <input>")) return false;
    }

    // Step 2: send setup and expect "<setup> who?"
    if (!send_line(session->fd, jk.setup + " <input>")) return false;
    if (!recv_line(session->fd, resp)) return false;
    if (!iequals(resp, expect2)) {
        if (!send_line(session->fd, "You are supposed to say, \"" + expect2 + "\". Let's try again.")) return false;
        // Restart the *same* joke from the top
        return play_joke(session);
    }

    // Step 3: punchline
    if (!send_line(session->fd, jk.punchline)) return false;
    return true;
}

// ---------------------------- Signal handling ---------------------------

/* Stop accepting new clients; running sessions will finish. */
static void signal_handler(int) {
    cout << "\nShutdown signal received. Waiting for clients to finish...\n";
    server_running.store(false);
    if (listen_fd >= 0) ::shutdown(listen_fd, SHUT_RDWR);  // wake poll()
}

// ------------------------------- Thread --------------------------------

/*
 * Thread entry per client. Manages the conversation loop and the "another?" prompt.
 * Decrements active_clients on exit.
 */
static void* handle_client(void* arg) {
    unique_ptr<ClientSession> session(static_cast<ClientSession*>(arg));

    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &session->client_addr.sin_addr, ip, INET_ADDRSTRLEN);
    cout << "Client connected from " << ip << ":" << ntohs(session->client_addr.sin_port) << "\n";

    random_device rd;
    session->rng.seed(rd());

    bool cont = true;
    while (cont) {
        if (!play_joke(session.get())) break;

        // Ask until we get a valid Y/N
        while (true) {
            if (!send_line(session->fd, "Would you like to listen to another? (Y/N) <input>")) { cont = false; break; }
            string choice;
            if (!recv_line(session->fd, choice)) { cont = false; break; }

            if (iequals(choice, "N") || iequals(choice, "no")) { cont = false; break; }
            if (iequals(choice, "Y") || iequals(choice, "yes")) { break; }

            if (!send_line(session->fd, "Please reply with Y or N.")) { cont = false; break; }
        }
    }

    ::close(session->fd);
    int left = --active_clients;
    cout << "Client disconnected. Active clients: " << left << "\n";
    if (left == 0) {
        cout << "Server will shutdown in 10s if no other client comes up.\n";
    }
    return nullptr;
}

// --------------------------------- Main ---------------------------------

int main() {
    // Load jokes from SQLite DB
    load_jokes_from_db("jokes.db");
    if (jokes.empty()) {
        cerr << "No jokes found in database!\n";
        return 1;
    }

    // Basic signal setup
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGINT,  signal_handler);
    ::signal(SIGTERM, signal_handler);

    // Listening socket
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (::listen(listen_fd, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    cout << "Server listening on port " << PORT << "...\n";
    cout << "Press Ctrl+C to stop the server gracefully.\n";

    // Idle shutdown timer bookkeeping
    bool timer_running = false;
    auto zero_since    = chrono::steady_clock::now();

    // Accept loop with poll() so we can check timers once per second
    while (server_running.load()) {
        pollfd pfd{listen_fd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 1000);  // 1-second tick

        if (pr > 0 && (pfd.revents & POLLIN)) {
            // New client
            sockaddr_in caddr{};
            socklen_t   clen = sizeof(caddr);
            int cfd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                continue;
            }

            active_clients.fetch_add(1);
            timer_running = false;  // reset idle timer

            auto* session     = new ClientSession();
            session->fd       = cfd;
            session->client_addr = caddr;

            pthread_t tid;
            if (pthread_create(&tid, nullptr, handle_client, session) != 0) {
                perror("pthread_create");
                ::close(cfd);
                delete session;
                active_clients.fetch_sub(1);
            } else {
                pthread_detach(tid);
            }
        } else if (pr == 0) {
            // poll() timeout -> check idle condition
            if (active_clients.load() == 0) {
                if (!timer_running) {
                    timer_running = true;
                    zero_since    = chrono::steady_clock::now();
                } else {
                    auto elapsed = chrono::steady_clock::now() - zero_since;
                    if (elapsed >= chrono::seconds(10)) {
                        cout << "No active clients for 10s. Shutting down server.\n";
                        break;
                    }
                }
            } else {
                timer_running = false;  // someone is active
            }
        } else {
            // Interrupted or error; loop condition will decide next step
            if (!server_running.load()) break;
        }
    }

    ::close(listen_fd);

    // Wait for threads finishing up (best effort)
    while (active_clients.load() > 0) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    cout << "Server shut down successfully.\n";
    return 0;
}
