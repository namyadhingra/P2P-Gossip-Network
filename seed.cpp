#include "common.h"
#include <iostream>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>

using namespace std;

/*
=========================================================
COMPILATION:
  g++ seed.cpp common.cpp -lws2_32 -std=c++17
      -static-libgcc -static-libstdc++
      -Wl,-Bstatic -lpthread -Wl,-Bdynamic
      -o seed.exe

USAGE:
  .\seed.exe <IP> <PORT>
  Example: .\seed.exe 127.0.0.1 5001

ROLE:
  Seeds are the "directory servers" of the P2P network.
  They do NOT participate in gossip. Their only jobs are:
    1. Accept REGISTER requests from peers joining the network
    2. Reach consensus with other seeds on who is a valid peer
    3. Respond to GET_PEERS requests so peers can find each other
    4. Accept and relay dead-node reports between seeds
=========================================================
*/


/*
=========================================================
GLOBAL STATE
(Shared across threads — always access under mtx lock)
=========================================================
*/

// The confirmed, consensus-approved list of live peers
set<string> peer_list;

// Tracks registration votes per peer.
// A peer is only added to peer_list once a quorum of seeds vote for it.
// Format: "IP:PORT" -> set of seed ports that have voted for it
map<string, set<int>> votes;

// Tracks dead-node reports per peer.
// A peer is only removed from peer_list once a quorum of seeds report it dead.
// Format: "IP:PORT" -> set of seed ports that reported it dead
map<string, set<int>> dead_reports;

mutex mtx;                      // Protects all state above
vector<pair<string,int>> SEEDS; // All seed addresses loaded from config

string HOST;   // This seed's IP address
int PORT;      // This seed's port number

ofstream log_file; // Persistent log written to outputfile.txt


/*
=========================================================
UTILITY: TRIM WHITESPACE
=========================================================
*/

// Strips leading/trailing spaces, tabs, newlines from a string.
// Used when parsing config file lines.
string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == string::npos) return "";
    return s.substr(start, end - start + 1);
}


/*
=========================================================
UTILITY: LOAD SEEDS FROM CONFIG FILE
=========================================================
*/

/*
Reads seed IP:PORT pairs from config.txt (or config.csv).
Each line should look like:
    127.0.0.1:5001
    127.0.0.1:5002
    127.0.0.1:5003
Lines starting with '#' are treated as comments and skipped.
Comma-separated format (127.0.0.1,5001) is also supported.
*/
vector<pair<string,int>> load_seeds() {
    vector<pair<string,int>> seeds;
    vector<string> files = {"config.txt", "config.csv"};

    for (auto& filename : files) {
        ifstream file(filename);
        if (!file) continue; // Try next file if this one doesn't exist

        string line;
        while (getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue; // Skip blank/comment lines

            string ip;
            int port;

            // Normalize: replace comma OR colon with space so stringstream can parse easily
            if (line.find(",") != string::npos)
                replace(line.begin(), line.end(), ',', ' ');
            else
                replace(line.begin(), line.end(), ':', ' ');

            stringstream ss(line);
            ss >> ip >> port;
            seeds.push_back({ip, port});
        }

        if (!seeds.empty()) break; // Stop after first successful file
    }

    if (seeds.empty())
        cout << "[WARNING] No seeds found in config file\n";

    return seeds;
}


/*
=========================================================
UTILITY: LOGGING
=========================================================
*/

// Prints a timestamped, leveled message to both console and outputfile.txt.
// Format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [IP:PORT] message
void log_seed(const string& message, const string& level = "INFO") {
    string ts   = get_timestamp();
    string id   = HOST + ":" + to_string(PORT);
    string full = ts + " [" + level + "] [" + id + "] " + message;

    cout << full << endl;

    if (log_file.is_open()) {
        log_file << full << endl;
        log_file.flush(); // Flush immediately so log isn't lost on crash
    }
}

// Returns a formatted snapshot of the current peer list.
// MUST be called while holding mtx.
string format_peer_list() {
    string result = "Current Peer List: {";
    bool first = true;
    for (auto& p : peer_list) {
        if (!first) result += ", ";
        result += p;
        first = false;
    }
    result += "} (size=" + to_string(peer_list.size()) + ")";
    return result;
}


/*
=========================================================
NETWORK: BROADCAST TO OTHER SEEDS
=========================================================
*/

/*
Sends a message to every other seed in the SEEDS list.
Used to propagate votes and dead-node reports across all seeds.
Retries up to 3 times per seed in case of temporary connection failure
(e.g., a seed that hasn't started yet).
*/
void broadcast_to_seeds(const string& message) {
    for (auto& seed : SEEDS) {

        if (seed.second == PORT) continue; // Don't send to ourselves

        int retries = 3;
        while (retries--) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == INVALID_SOCKET) continue;

            sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(seed.second);
            inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                send_json(s, message);
                closesocket(s);
                break; // Success — move on to next seed
            } else {
                closesocket(s);
                this_thread::sleep_for(chrono::milliseconds(500)); // Wait before retry
            }
        }
    }
}


/*
=========================================================
CONSENSUS: PEER REGISTRATION VOTING
=========================================================
*/

/*
Called when this seed receives a VOTE message from another seed
(or casts its own vote on receiving a REGISTER from a peer).

How it works:
  - Each seed that sees a REGISTER request votes for that peer
  - Once votes >= majority (n/2 + 1), the peer is officially added to peer_list
  - This prevents a single seed from unilaterally admitting peers
*/
void process_vote(const string& peer, int reporting_seed) {
    lock_guard<mutex> lock(mtx);

    votes[peer].insert(reporting_seed); // Record this seed's vote

    int current  = votes[peer].size();
    int required = SEEDS.size() / 2 + 1; // Majority quorum

    if (current >= required) {
        // Quorum reached — add peer to the official list
        if (peer_list.find(peer) == peer_list.end()) {
            peer_list.insert(peer);
            log_seed("[CONSENSUS OUTCOME] Peer added: " + peer +
                     " | Votes: " + to_string(current) +
                     "/" + to_string(SEEDS.size()));
            log_seed(format_peer_list());
        }
        // If already in peer_list, do nothing (duplicate vote arriving late)
    } else {
        // Still gathering votes
        log_seed("[CONSENSUS PROGRESS] " + peer +
                 " | Votes: " + to_string(current) +
                 "/" + to_string(required));
    }
}


/*
=========================================================
CONSENSUS: DEAD NODE VOTING
=========================================================
*/

/*
Called when this seed receives a SEED_DEAD_VOTE from another seed.

How it works:
  - Peers detect unresponsive neighbors and report them via "Dead Node:" messages
  - Seeds relay those reports to each other as SEED_DEAD_VOTE messages
  - Once dead reports >= majority, the peer is removed from peer_list
  - dead_reports entry is cleared after removal to allow re-registration later
*/
void process_dead_vote(const string& target, int reporting_seed) {
    lock_guard<mutex> lock(mtx);

    dead_reports[target].insert(reporting_seed);

    int current  = dead_reports[target].size();
    int required = SEEDS.size() / 2 + 1;

    if (current >= required) {
        // Quorum of seeds agree this peer is dead — evict it
        if (peer_list.find(target) != peer_list.end()) {
            peer_list.erase(target);
            log_seed("[SEED CONSENSUS OUTCOME] Removed dead peer: " +
                     target + " | Votes: " +
                     to_string(current) + "/" +
                     to_string(SEEDS.size()), "WARNING");
            log_seed(format_peer_list(), "WARNING");
        }
        dead_reports.erase(target); // Clean up so peer can re-register later
    } else {
        log_seed("[DEAD NODE CONSENSUS PROGRESS] " + target +
                 " | Votes: " + to_string(current) +
                 "/" + to_string(required), "WARNING");
    }
}


/*
=========================================================
SERVER: HANDLE INCOMING CLIENT CONNECTIONS
=========================================================
*/

/*
Each incoming connection is handled in its own thread.
We receive one message, act on it, then close the connection.
(Short-lived connections — no persistent sessions.)

Message types handled:
  1. "Dead Node:..."         — A peer reporting a dead neighbor
  2. "TYPE:REGISTER;PEER:…"  — A peer joining the network
  3. "TYPE:VOTE;..."         — Another seed relaying a registration vote
  4. "TYPE:GET_PEERS"        — A peer asking for the current peer list
  5. "TYPE:SEED_DEAD_VOTE;…" — Another seed relaying a dead-node vote
*/
void handle_client(SOCKET conn) {

    string message = recv_json(conn);

    if (message.empty()) {
        // Connection closed before any data was sent — ignore
        closesocket(conn);
        return;
    }

    // -------------------------------------------------------
    // CASE 1: DEAD NODE REPORT FROM A PEER
    // Format: "Dead Node:IP:PORT:timestamp:reporterIP"
    // -------------------------------------------------------
    if (message.rfind("Dead Node:", 0) == 0) {

        log_seed(message, "WARNING");

        // Split on ':' to extract the dead peer's IP and PORT
        vector<string> parts;
        stringstream ss(message);
        string segment;
        while (getline(ss, segment, ':'))
            parts.push_back(segment);

        if (parts.size() >= 3) {
            string target = parts[1] + ":" + parts[2]; // Reconstruct "IP:PORT"

            // Record our own vote for this dead node
            {
                lock_guard<mutex> lock(mtx);
                dead_reports[target].insert(PORT);
            }

            // Tell all other seeds about this dead node report
            string vote_msg = "TYPE:SEED_DEAD_VOTE;TARGET:" + target +
                              ";SEED:" + to_string(PORT);
            broadcast_to_seeds(vote_msg);
        }

        closesocket(conn);
        return;
    }

    // -------------------------------------------------------
    // CASE 2: PEER REGISTRATION REQUEST
    // A new peer is announcing itself to join the network.
    // We vote for it locally and broadcast the vote to other seeds.
    // -------------------------------------------------------
    if (message.find("TYPE:REGISTER") != string::npos) {

        size_t pos = message.find("PEER:");
        if (pos == string::npos) {
            closesocket(conn);
            return;
        }

        string peer = message.substr(pos + 5); // Extract "IP:PORT"
        log_seed("[REGISTRATION PROPOSAL] " + peer);

        // Cast our own vote for this peer and check if quorum is reached
        process_vote(peer, PORT);

        // Broadcast our vote to all other seeds so they can also count it
        string vote_msg = "TYPE:VOTE;PEER:" + peer +
                          ";SEED:" + to_string(PORT);
        broadcast_to_seeds(vote_msg);
    }

    // -------------------------------------------------------
    // CASE 3: VOTE FROM ANOTHER SEED
    // Another seed is telling us it voted to register a peer.
    // We record it and check if quorum has been reached.
    // -------------------------------------------------------
    else if (message.find("TYPE:VOTE") != string::npos) {

        size_t p1 = message.find("PEER:");
        size_t p2 = message.find(";SEED:");

        if (p1 != string::npos && p2 != string::npos) {
            string peer           = message.substr(p1 + 5, p2 - (p1 + 5));
            int    reporting_seed = stoi(message.substr(p2 + 6));
            process_vote(peer, reporting_seed);
        }
    }

    // -------------------------------------------------------
    // CASE 4: PEER REQUESTING THE PEER LIST
    // A peer asks "who else is in the network?" after registering.
    // We respond with our current consensus-approved peer list.
    // -------------------------------------------------------
    else if (message.find("TYPE:GET_PEERS") != string::npos) {

        lock_guard<mutex> lock(mtx);

        // Build response: "TYPE:PEER_LIST;IP:PORT,IP:PORT,..."
        string response = "TYPE:PEER_LIST;";
        for (auto& p : peer_list)
            response += p + ",";

        send_json(conn, response);
    }

    // -------------------------------------------------------
    // CASE 5: DEAD NODE VOTE FROM ANOTHER SEED
    // Another seed is reporting that a peer is dead.
    // We record it and check if quorum has been reached for removal.
    // -------------------------------------------------------
    else if (message.find("TYPE:SEED_DEAD_VOTE") != string::npos) {

        size_t p1 = message.find("TARGET:");
        size_t p2 = message.find(";SEED:");

        if (p1 != string::npos && p2 != string::npos) {
            string target         = message.substr(p1 + 7, p2 - (p1 + 7));
            int    reporting_seed = stoi(message.substr(p2 + 6));
            process_dead_vote(target, reporting_seed);
        }
    }

    // -------------------------------------------------------
    // CASE 6: PERIODIC SYNC FROM ANOTHER SEED
    // Another seed shares its peer list for consistency.
    // We merge any unknown peers into our own list.
    // -------------------------------------------------------
    else if (message.find("TYPE:SYNC_LIST") != string::npos) {

        size_t pos = message.find("PEERS:");
        if (pos != string::npos) {
            string peers_str = message.substr(pos + 6);
            stringstream ss(peers_str);
            string peer;

            lock_guard<mutex> lock(mtx);
            bool changed = false;
            while (getline(ss, peer, ',')) {
                if (!peer.empty() && peer_list.find(peer) == peer_list.end()) {
                    peer_list.insert(peer);
                    changed = true;
                }
            }
            if (changed) {
                log_seed("[SYNC] Merged peer list from remote seed");
                log_seed(format_peer_list());
            }
        }
    }

    closesocket(conn);
}


/*
=========================================================
BACKGROUND: PERIODIC SEED-TO-SEED SYNC
=========================================================
*/

/*
Every 30 seconds, broadcasts this seed's peer list to all other seeds.
This ensures eventual consistency even if some initial votes were lost
due to transient network failures or race conditions.
*/
void seed_sync_thread() {
    while (true) {
        this_thread::sleep_for(chrono::seconds(30));

        string our_peers;
        {
            lock_guard<mutex> lock(mtx);
            for (auto& p : peer_list)
                our_peers += p + ",";
        }

        if (our_peers.empty()) continue; // Nothing to sync yet

        string sync_msg = "TYPE:SYNC_LIST;PEERS:" + our_peers;

        for (auto& seed : SEEDS) {
            if (seed.second == PORT) continue; // Don't send to ourselves

            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == INVALID_SOCKET) continue;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(seed.second);
            inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                send_json(s, sync_msg);
            }
            closesocket(s);
        }

        log_seed("[SYNC] Periodic peer-list sync broadcasted to other seeds");
    }
}


/*
=========================================================
SERVER: MAIN ACCEPT LOOP
=========================================================
*/

/*
Creates a TCP server socket, binds it to HOST:PORT, and
enters an infinite loop accepting incoming connections.
Each connection is handed off to handle_client() in a new thread
so the accept loop is never blocked by slow clients.
*/
void start_server() {

    // Create a TCP socket
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        log_seed("[FATAL ERROR] Socket creation failed. WSAError: " +
                 to_string(WSAGetLastError()), "ERROR");
        return;
    }

    // SO_REUSEADDR: allows restarting the seed quickly without
    // waiting for the OS to release the port from a previous run
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    // Bind to this seed's address
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    inet_pton(AF_INET, HOST.c_str(), &addr.sin_addr);

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        log_seed("[FATAL ERROR] Bind failed. WSAError: " + to_string(err), "ERROR");
        if (err == 10048) cout << "  Port already in use — kill existing seed.exe processes\n";
        if (err == 10049) cout << "  IP address not available on this machine\n";
        if (err == 10013) cout << "  Permission denied — try running as Administrator\n";
        closesocket(server);
        return;
    }

    // Start listening — SOMAXCONN lets the OS choose the backlog queue size
    if (listen(server, SOMAXCONN) == SOCKET_ERROR) {
        log_seed("[FATAL ERROR] Listen failed. WSAError: " +
                 to_string(WSAGetLastError()), "ERROR");
        closesocket(server);
        return;
    }

    log_seed("[SEED STARTED] Running at " + HOST + ":" + to_string(PORT));

    // Accept loop — runs forever, one thread per client
    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue; // Transient error — keep going
        thread(handle_client, client).detach();  // Handle in background thread
    }
}


/*
=========================================================
ENTRY POINT
=========================================================
*/

int main(int argc, char* argv[]) {

    // Require IP and PORT as command-line arguments
    if (argc < 3) {
        cout << "Usage: seed <IP> <PORT>\n";
        cout << "Example: .\\seed.exe 127.0.0.1 5001\n";
        return 1;
    }

    HOST = argv[1];
    PORT = stoi(argv[2]);

    // Winsock must be initialized before any socket calls on Windows
    if (!initialize_winsock()) {
        cout << "Winsock initialization failed\n";
        return 1;
    }

    // Load peer addresses of all seeds (including ourselves) from config file
    SEEDS = load_seeds();

    // Open persistent log file (append mode so logs survive restarts)
    log_file.open("outputfile.txt", ios::app);
    if (!log_file.is_open())
        cout << "[WARNING] Could not open outputfile.txt for logging\n";

    // Start periodic seed-to-seed peer-list sync in background
    thread(seed_sync_thread).detach();

    // Start the server — this blocks forever in the accept loop
    start_server();

    // Only reached if start_server() returns due to a fatal error
    cleanup_winsock();
    return 0;
}