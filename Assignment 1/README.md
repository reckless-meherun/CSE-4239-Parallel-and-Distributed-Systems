
# Knock‑Knock Joke Server (PDS Lab)

Multi‑client TCP **knock‑knock joke** server with a SQLite joke “database”, an interactive client, and an automated tester.  
Designed for Linux, macOS, and Windows (via **WSL**).

---

## What you get

- **server** — multi‑threaded TCP server (pthreads), graceful idle shutdown after 10s
- **client** — interactive terminal client (follows the `<input>` prompts)
- **tester** — automated checker for happy path, corrections, concurrency, and idle shutdown
- **jokes.db** — SQLite database of jokes (table: `jokes(setup, punchline)`)
- **Makefile** — builds all three tools

> Protocol (spelling‑sensitive, case‑insensitive):
```
Server: Knock knock! <input>
Client: Who's there?
Server: <setup> <input>
Client: <setup> who?
Server: <punchline>
Server: Would you like to listen to another? (Y/N) <input>
```
If the client answers incorrectly, the server explains the expected reply and restarts the joke from the beginning.

---

## Quick start 

```bash
make                 # build server, client, tester
./server             # terminal 1: start server (listens on port 8079)
./client localhost 8079     # terminal 2: talk to server
./tester localhost 8079     # terminal 3: run automated tests (optional)
```

The server prints `Server will shutdown in 10s if no other client comes up.` when the last client disconnects; if no one connects within 10 seconds, it exits cleanly.

---

## Full installation guide

### A) Linux (Debian/Ubuntu)

1) **Install toolchain + SQLite dev**  
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential g++ make pkg-config libsqlite3-dev
   ```

2) **Build**  
   ```bash
   make
   ```

3) **Run**  
   - Server (terminal 1): `./server`  
   - Client (terminal 2): `./client localhost 8079`  
   - Tester (terminal 3): `./tester localhost 8079`

---

### B) macOS

1) **Developer tools** (includes Clang/Make):  
   ```bash
   xcode-select --install
   ```

2) **(Recommended) Homebrew + SQLite dev headers**  
   ```bash
   # install Homebrew if you don't have it: https://brew.sh
   brew install sqlite
   ```

3) **Build**  
   ```bash
   make
   ```

4) **Run**  
   - Server (terminal 1): `./server`  
   - Client (terminal 2): `./client localhost 8079`  
   - Tester (terminal 3): `./tester localhost 8079`

> Note: macOS already has BSD sockets; no extra networking packages required.

---

### C) Windows (WSL recommended)

Because the server uses **POSIX sockets and pthreads**, the simplest path on Windows is **WSL (Windows Subsystem for Linux)**.

1) **Install WSL (from PowerShell as Administrator):**
   ```powershell
   wsl --install -d Ubuntu
   ```
   - If prompted to reboot, do so, then create your Linux username/password.
   - Launch Ubuntu from Start Menu (or `wsl.exe` in a new terminal).

   *If `wsl --install` isn’t recognized on older Windows builds:*
   ```powershell
   dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
   dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
   wsl --set-default-version 2
   wsl --install -d Ubuntu
   ```

2) **Inside WSL (Ubuntu) — install toolchain + SQLite dev:**
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential g++ make pkg-config libsqlite3-dev
   ```

3) **Move project files into your WSL home** (or access via `/mnt/c/...`).

4) **Build & run (same as Linux):**
   ```bash
   make
   ./server
   ./client localhost 8079
   ./tester localhost 8079
   ```

> Alternative (advanced): You could use **MSYS2** or **MinGW** and port to Windows threads/WinSock, but WSL is strongly recommended for POSIX compatibility.

---

## Database: `jokes.db`

- Expected schema:
  ```sql
  CREATE TABLE jokes (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      setup TEXT NOT NULL,
      punchline TEXT NOT NULL
  );
  ```

- To open or edit:
  ```bash
  sqlite3 jokes.db
  .tables
  SELECT * FROM jokes LIMIT 5;
  ```

- To add a joke:
  ```sql
  INSERT INTO jokes (setup, punchline) VALUES ('Snape', 'Snape to it and let me in before Filch catches me.');
  ```

The server loads all jokes at startup; no restart is needed unless you want to reload from disk.

---

## Building & cleaning

- Build everything:
  ```bash
  make
  ```
- Clean binaries:
  ```bash
  make clean
  ```

> The `Makefile` links the server against `-lsqlite3` and `-pthread` automatically.

---

## Running the server and clients

### Same machine (localhost)

1) **Server** (terminal 1):
   ```bash
   ./server
   ```
2) **Client** (terminal 2):
   ```bash
   ./client localhost 8079
   ```
3) **More clients** (terminal 3, 4, ...):
   ```bash
   ./client localhost 8079
   ```
4) **Automated tests** (optional, terminal 5):
   ```bash
   ./tester localhost 8079
   ```

### Different machines on the same LAN

1) **Find the server’s IP**  
   - Linux: `hostname -I` (pick a `192.168.*.*` or `10.*.*.*` address)  
   - macOS: `ipconfig getifaddr en0` (Wi‑Fi) or `ipconfig getifaddr en1` (Ethernet)  
   - WSL: `ip addr show eth0 | grep 'inet '`, or from Windows: `ipconfig`

2) **Open firewall** (if needed) on the server machine for TCP **8079**.

3) **Start server** on the server machine:
   ```bash
   ./server
   ```

4) **Start client(s)** on other machines (replace `<SERVER_IP>`):
   ```bash
   ./client <SERVER_IP> 8079
   ```

> If the last client disconnects, the server prints  
> `Server will shutdown in 10s if no other client comes up.`  
> and exits after 10s of inactivity.

---

## Tester (automated checks)

Run:
```bash
./tester <host> <port>
# examples
./tester localhost 8079
./tester 192.168.1.25 8079
```

It verifies:
- Happy path (complete one joke, answer N)
- Wrong first/second reply → correction + restart
- Multiple concurrent clients
- Idle shutdown after 10s with no clients

Exit status is non‑zero if a test fails.

---

## Troubleshooting

- **`bind: Address already in use` (server):** Previous server is still running or port in TIME_WAIT. Wait a bit or `pkill server` and run again. The server uses `SO_REUSEADDR`.
- **`connect: Connection refused` (client/tester):** Server not running, wrong host/port, or firewall blocked. Verify with `nc -zv <host> 8079`.
- **Can’t find SQLite headers (`sqlite3.h`)**: Install dev package (see install steps). On macOS, `brew install sqlite`.
- **Windows**: Prefer WSL. If you must run natively, you’ll need to replace pthreads with std::thread or Win32 threads and link against WinSock—outside this assignment’s scope.

---

## Project layout

```
.
├── server.cpp     # multi-client server (pthreads, SQLite-backed jokes)
├── client.cpp     # interactive client
├── tester.cpp     # automated tester for the protocol
├── jokes.db       # SQLite database
├── Makefile       # builds server, client, tester
└── README.md
```

— Happy hacking, and remember: if someone knocks, always ask **"Who’s there?"** 😉
