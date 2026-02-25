#include "common.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>

using namespace std;


/*
GLOBAL STATE (Seed-level membership & consensus storage)
*/

// Final verified peer list (PL)
set<string> peer_list;

// Registration votes
// Format:  peer -> set of seed ports that voted
map<string, set<int>> votes;

// Dead node reports
// Format: peer -> set of seed ports that reported
map<string, set<int>> dead_reports;

mutex mtx;                  // Protect shared state
vector<pair<string,int>> SEEDS;

string HOST;
int PORT;

ofstream log_file;

// This runs before main() -- if you never see this, it is a linker/binary issue
struct EarlyDebug {
    EarlyDebug() {
        fprintf(stderr, "[PRE-MAIN] Static init OK\n");
        fflush(stderr);
    }
} _early;


/*
=========================================================
UTILITY FUNCTIONS
=========================================================
*/

// Trim whitespace
string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == string::npos) return "";
    return s.substr(start, end - start + 1);
}

/*
Load seed addresses from config.txt or config.csv
Supports:
127.0.0.1:5001
127.0.0.1,5001
*/
vector<pair<string,int>> load_seeds() {
    vector<pair<string,int>> seeds;
    vector<string> files = {"config.txt", "config.csv"};

    for (auto& filename : files) {
        ifstream file(filename);
        if (!file) continue;

        string line;
        while (getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;

            string ip;
            int port;

            if (line.find(",") != string::npos) {
                replace(line.begin(), line.end(), ',', ' ');
            } else {
                replace(line.begin(), line.end(), ':', ' ');
            }

            stringstream ss(line);
            ss >> ip >> port;

            seeds.push_back({ip, port});
        }

        if (!seeds.empty()) break;
    }

    if (seeds.empty()) {
        cout << "Warning: No seeds found in config file\n";
    }

    return seeds;
}


/*
Log to console + file
*/
void log_seed(const string& message) {
    string id = HOST + ":" + to_string(PORT);
    string full = "[" + id + "] " + message;

    cout << full << endl;

    if (log_file.is_open()) {
        log_file << full << endl;
        log_file.flush();
    }
}


/*
Broadcast JSON-like message to all other seeds
*/
void broadcast_to_seeds(const string& message) {

    for (auto& seed : SEEDS) {

        if (seed.second == PORT) continue;  // Don't send to self

        int retries = 3;

        while (retries--) {

            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == INVALID_SOCKET) continue;

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(seed.second);
            inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {

                send_json(s, message);
                closesocket(s);
                break;

            } else {
                closesocket(s);
                this_thread::sleep_for(chrono::milliseconds(500));
            }
        }
    }
}


/*
=========================================================
CONSENSUS LOGIC
=========================================================
*/

void process_vote(const string& peer, int reporting_seed) {

    lock_guard<mutex> lock(mtx);

    votes[peer].insert(reporting_seed);

    int current = votes[peer].size();
    int required = SEEDS.size() / 2 + 1;

    if (current >= required) {

        if (peer_list.find(peer) == peer_list.end()) {
            peer_list.insert(peer);
            log_seed("[CONSENSUS OUTCOME] Peer added: " + peer +
                     " | Votes: " + to_string(current) +
                     "/" + to_string(SEEDS.size()));
        }

    } else {
        log_seed("[CONSENSUS PROGRESS] " + peer +
                 " | Votes: " + to_string(current) +
                 "/" + to_string(required));
    }
}


void process_dead_vote(const string& target, int reporting_seed) {

    lock_guard<mutex> lock(mtx);

    dead_reports[target].insert(reporting_seed);

    int current = dead_reports[target].size();
    int required = SEEDS.size() / 2 + 1;

    if (current >= required) {

        if (peer_list.find(target) != peer_list.end()) {

            peer_list.erase(target);
            log_seed("[SEED CONSENSUS OUTCOME] Removed dead peer: " +
                     target + " | Votes: " +
                     to_string(current) + "/" +
                     to_string(SEEDS.size()));
        }

        dead_reports.erase(target);

    } else {
        log_seed("[DEAD NODE CONSENSUS PROGRESS] " + target +
                 " | Votes: " + to_string(current) +
                 "/" + to_string(required));
    }
}


/*
=========================================================
HANDLE CLIENT CONNECTION
=========================================================
*/

void handle_client(SOCKET conn) {

    string message = recv_json(conn);

    if (message.empty()) {
        closesocket(conn);
        return;
    }

    /*
    ---------------------------------------------------------
    CASE 1: DEAD NODE STRING FORMAT
    Format:
    Dead Node:IP:PORT:timestamp:reporterIP
    ---------------------------------------------------------
    */
    if (message.rfind("Dead Node:", 0) == 0) {

        log_seed(message);

        vector<string> parts;
        stringstream ss(message);
        string segment;

        while (getline(ss, segment, ':'))
            parts.push_back(segment);

        if (parts.size() >= 3) {

            string target = parts[1] + ":" + parts[2];

            {
                lock_guard<mutex> lock(mtx);
                dead_reports[target].insert(PORT);
            }

            string vote_msg = "TYPE:SEED_DEAD_VOTE;TARGET:" + target +
                              ";SEED:" + to_string(PORT);

            broadcast_to_seeds(vote_msg);
        }

        closesocket(conn);
        return;
    }

    /*
    ---------------------------------------------------------
    OTHERWISE: JSON-LIKE STRING FORMAT
    (Manual parsing since we avoid JSON library)
    ---------------------------------------------------------
    */

    // REGISTER
    if (message.find("TYPE:REGISTER") != string::npos) {

        size_t pos = message.find("PEER:");
        if (pos == string::npos) {
            closesocket(conn);
            return;
        }

        string peer = message.substr(pos + 5);

        log_seed("[REGISTRATION PROPOSAL] " + peer);

        {
            lock_guard<mutex> lock(mtx);
            votes[peer].insert(PORT);
        }

        string vote_msg = "TYPE:VOTE;PEER:" + peer +
                          ";SEED:" + to_string(PORT);

        broadcast_to_seeds(vote_msg);
    }

    // VOTE
    else if (message.find("TYPE:VOTE") != string::npos) {

        string peer;
        int reporting_seed;

        size_t p1 = message.find("PEER:");
        size_t p2 = message.find(";SEED:");

        if (p1 != string::npos && p2 != string::npos) {

            peer = message.substr(p1 + 5, p2 - (p1 + 5));
            reporting_seed = stoi(message.substr(p2 + 6));

            process_vote(peer, reporting_seed);
        }
    }

    // GET_PEERS
    else if (message.find("TYPE:GET_PEERS") != string::npos) {

        lock_guard<mutex> lock(mtx);

        string response = "TYPE:PEER_LIST;";

        for (auto& p : peer_list)
            response += p + ",";

        send_json(conn, response);
    }

    // SEED DEAD VOTE
    else if (message.find("TYPE:SEED_DEAD_VOTE") != string::npos) {

        string target;
        int reporting_seed;

        size_t p1 = message.find("TARGET:");
        size_t p2 = message.find(";SEED:");

        if (p1 != string::npos && p2 != string::npos) {

            target = message.substr(p1 + 7, p2 - (p1 + 7));
            reporting_seed = stoi(message.substr(p2 + 6));

            process_dead_vote(target, reporting_seed);
        }
    }

    closesocket(conn);
}


/*
=========================================================
START SEED SERVER
=========================================================
*/

void start_server() {

    cout << "[DEBUG] start_server() entered\n";

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        cout << "[FATAL ERROR] Socket creation failed. WSAError: " << WSAGetLastError() << "\n";
        log_seed("[FATAL ERROR] Socket creation failed. WSAError: " + to_string(WSAGetLastError()));
        return;
    }
    cout << "[DEBUG] Socket created OK\n";

    // Allow port reuse so restarting quickly doesn't get EADDRINUSE
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, HOST.c_str(), &addr.sin_addr);

    cout << "[DEBUG] Attempting bind on " << HOST << ":" << PORT << "\n";

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        cout << "[FATAL ERROR] Bind failed. WSAError: " << err << "\n";
        cout << "  Common causes:\n";
        cout << "  10048 = Port already in use — kill other seed.exe processes\n";
        cout << "  10049 = IP address not valid on this machine\n";
        cout << "  10013 = Permission denied — try running as Administrator\n";
        log_seed("[FATAL ERROR] Bind failed. WSAError: " + to_string(err));
        closesocket(server);
        return;
    }
    cout << "[DEBUG] Bind OK\n";

    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        cout << "[FATAL ERROR] Listen failed. WSAError: " << WSAGetLastError() << "\n";
        closesocket(server);
        return;
    }
    cout << "[DEBUG] Listening OK\n";

    log_seed("[SEED STARTED] Running at " + HOST + ":" + to_string(PORT));

    while (true) {

        SOCKET client = accept(server, nullptr, nullptr);

        if (client == INVALID_SOCKET) continue;

        thread(handle_client, client).detach();
    }
}


/*
MAIN
*/
int main(int argc, char* argv[]) {
    
    fprintf(stderr, "[MAIN] main() reached\n");
    fflush(stderr);
    cout << "Seed main started" << endl;

    if (argc < 3) {
        cout << "Usage: seed <IP> <PORT>\n";
        return 1;
    }

    HOST = argv[1];
    PORT = stoi(argv[2]);
    cout << "[DEBUG] HOST=" << HOST << " PORT=" << PORT << "\n";

    if (!initialize_winsock()) {
        cout << "Winsock initialization failed\n";
        return 1;
    }
    cout << "[DEBUG] Winsock initialized OK\n";

    SEEDS = load_seeds();
    cout << "[DEBUG] Loaded " << SEEDS.size() << " seed(s) from config\n";
    if (SEEDS.empty()) {
        cout << "[WARNING] No seeds loaded — make sure config.txt exists in the working directory\n";
        cout << "          Current dir should contain: 127.0.0.1:5001  127.0.0.1:5002  127.0.0.1:5003\n";
    }

    log_file.open("outputfile.txt", ios::app);
    if (!log_file.is_open()) {
        cout << "[WARNING] Could not open outputfile.txt for logging\n";
    }

    cout << "[DEBUG] Calling start_server()...\n";
    start_server();

    cout << "[DEBUG] start_server() returned — this should NOT happen in normal operation\n";
    cleanup_winsock();
    return 0;
}