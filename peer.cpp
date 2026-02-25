#include "common.h"

#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <algorithm>
#include <random>
#include <numeric>   // for iota (used in weighted selection)
#include <cstdlib>   // for system()

using namespace std;

/*
COMPILATION:
  g++ peer.cpp common.cpp -lws2_32 -std=c++17
      -static-libgcc -static-libstdc++
      -Wl,-Bstatic -lpthread -Wl,-Bdynamic
      -o peer.exe

USAGE:
  .\peer.exe <IP> <PORT>
  Example: .\peer.exe 127.0.0.1 6001

ROLE:
  Peer nodes form the actual P2P overlay network.
  Each peer:
    1. Registers with a quorum of seed nodes
    2. Builds a neighbor set using preferential attachment (power-law)
    3. Generates and forwards gossip messages
    4. Periodically pings neighbors to detect failures
    5. Uses two-level consensus before declaring a node dead
*/


/*
GLOBAL CONFIGURATION
*/

string HOST;      // This peer's IP
int    PORT;      // This peer's port
string SELF_ID;   // "IP:PORT" string identifying this peer

ofstream log_file; // Persistent log


/*
THREAD-SAFE STATE
All of these are accessed from multiple threads.
Always acquire state_lock before reading or writing.
*/

set<string>              peer_list;       // All known peers (from seed peer lists)
set<string>              neighbors;       // Currently active topology neighbors
set<string>              message_list;    // Hashes of gossip messages already seen (ML)
map<string, set<string>> suspicion_votes; // peer -> set of neighbors that suspect it dead
map<string, int>         ping_failures;   // peer -> consecutive ping failure count
map<string, long long>   last_seen;       // peer -> timestamp of last successful ping

// degree_map tracks how many connections each known peer has.
// Used for preferential attachment in build_topology().
// Initialized from peer_list; incremented when we connect to a peer.
map<string, int> degree_map;

mutex state_lock;



// LIVENESS DETECTION CONSTANTS

// How many consecutive ping failures before we enter suspicion phase
const int PING_FAIL_THRESHOLD = 3;

// How many neighbors must independently suspect a node before
// we send a dead-node report to seeds (peer-level consensus threshold)
// We use majority of current neighbors: computed dynamically below.


// LOGGING utility
void log_peer(const string& msg) {
    string full = "[" + SELF_ID + "] " + msg;
    cout << full << endl;
    if (log_file.is_open()) {
        log_file << full << endl;
        log_file.flush();
    }
}


// LOAD SEEDS FROM CONFIG FILE
/*
Reads seed addresses from config.txt or config.csv.
Supports both "IP:PORT" and "IP,PORT" formats.
Lines starting with '#' are skipped as comments.
*/
vector<pair<string,int>> load_seeds() {

    vector<pair<string,int>> seeds;
    vector<string> files = {"config.txt", "config.csv"};

    for (auto& filename : files) {
        ifstream file(filename);
        if (!file) continue;

        string line;
        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;

            // Normalize: treat comma same as colon
            replace(line.begin(), line.end(), ',', ':');

            string ip;
            int port;
            stringstream ss(line);
            getline(ss, ip, ':');
            ss >> port;

            seeds.push_back({ip, port});
        }

        if (!seeds.empty()) break;
    }

    return seeds;
}


// SEED COMMUNICATION: REGISTER + GET PEER LIST
/*
Two-phase seed interaction:
  Phase 1: Send REGISTER to all seeds so they can reach consensus
           on adding this peer to their Peer Lists.
  Phase 2: Wait for seeds to reach consensus (2 second sleep),
           then fetch the resulting Peer List from each seed.
           The union of all lists becomes our peer_list.
*/
void register_and_get_peers() {

    auto seeds = load_seeds();
    if (seeds.empty()) {
        log_peer("[ERROR] No seeds found in config file.");
        return;
    }

    log_peer("[REGISTRATION] Registering with quorum of seeds");

    // --- PHASE 1: Send REGISTER to every seed ---
    for (auto& seed : seeds) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(seed.second);
        inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            string msg = "TYPE:REGISTER;PEER:" + SELF_ID;
            send_json(s, msg);
            log_peer("[REGISTRATION] Sent to " + seed.first + ":" + to_string(seed.second));
        }
        closesocket(s);
    }

    // Give seeds time to exchange votes and reach consensus
    this_thread::sleep_for(chrono::seconds(2));

    // --- PHASE 2: Fetch peer lists from each seed ---
    for (auto& seed : seeds) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(seed.second);
        inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            send_json(s, "TYPE:GET_PEERS");
            string res = recv_json(s);

            if (res.find("TYPE:PEER_LIST") != string::npos) {
                // Response format: "TYPE:PEER_LIST;IP:PORT,IP:PORT,..."
                string peers_str = res.substr(res.find(";") + 1);
                stringstream ss(peers_str);
                string peer;

                lock_guard<mutex> lock(state_lock);
                while (getline(ss, peer, ',')) {
                    if (!peer.empty() && peer != SELF_ID) {
                        peer_list.insert(peer);
                        // Initialize degree to 1 for all known peers.
                        // The actual degree is unknown at this point;
                        // 1 ensures every peer has a non-zero weight
                        // in preferential attachment.
                        if (degree_map.find(peer) == degree_map.end())
                            degree_map[peer] = 1;
                    }
                }
                log_peer("[PEER LIST RECEIVED] From seed " + seed.first);
            }
        }
        closesocket(s);
    }
}


// TOPOLOGY CONSTRUCTION: PREFERENTIAL ATTACHMENT (Power-Law)
/*
Implements Barabási–Albert preferential attachment to produce
a power-law degree distribution in the overlay network.

How it works:
  - Each candidate peer is assigned a weight = degree_map[peer]
    (peers with more connections are more likely to be chosen)
  - We perform weighted random selection without replacement
  - We connect to floor(n/2)+1 peers where n = available peers,
    ensuring at least one connection is always made

Why power-law?
  Real-world P2P networks (Gnutella, BitTorrent) exhibit power-law
  degree distributions: a few "hub" nodes have many connections
  while most nodes have few. This makes gossip propagation efficient
  because messages reach hubs quickly and spread from there.
*/
void build_topology() {

    vector<string> available;
    vector<int>    weights;

    {
        lock_guard<mutex> lock(state_lock);
        for (auto& p : peer_list) {
            if (neighbors.find(p) == neighbors.end()) {
                available.push_back(p);
                // Weight = degree + 1 (the +1 ensures new nodes with
                // degree 0 still have a chance of being selected)
                weights.push_back(degree_map.count(p) ? degree_map[p] + 1 : 1);
            }
        }
    }

    if (available.empty()) {
        log_peer("[TOPOLOGY] No peers available yet.");
        return;
    }

    // Connect to at least 1, up to floor(n/2)+1 peers
    int num_to_connect = max(1, (int)available.size() / 2 + 1);
    num_to_connect = min(num_to_connect, (int)available.size());

    random_device rd;
    mt19937 gen(rd());

    for (int i = 0; i < num_to_connect && !available.empty(); i++) {

        // Weighted random selection:
        // discrete_distribution picks index i with probability
        // proportional to weights[i], implementing preferential attachment
        discrete_distribution<int> dist(weights.begin(), weights.end());
        int idx = dist(gen);

        string peer = available[idx];

        // Remove selected peer so we don't pick it again
        available.erase(available.begin() + idx);
        weights.erase(weights.begin() + idx);

        string ip   = peer.substr(0, peer.find(":"));
        int    port = stoi(peer.substr(peer.find(":") + 1));

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            string msg = "TYPE:CONNECT_REQ;PEER:" + SELF_ID;
            send_json(s, msg);

            {
                lock_guard<mutex> lock(state_lock);
                neighbors.insert(peer);
                last_seen[peer] = time(nullptr);
                // Increment degree for the chosen peer so future
                // peers are even more likely to connect to popular nodes
                degree_map[peer]++;
                degree_map[SELF_ID]++;
            }

            log_peer("[TOPOLOGY] Connected to " + peer);
        }

        closesocket(s);
    }
}


// DEAD NODE REPORTING: SEND TO ALL SEEDS
/*
After peer-level consensus confirms a node is dead, we send
the standard dead-node report format to every seed:

  Dead Node:<IP>:<PORT>:<timestamp>:<reporter_IP>

Seeds will exchange these reports and remove the peer from
their Peer Lists only after reaching their own majority consensus.
*/
void report_dead_to_seeds(const string& dead_peer) {

    auto seeds = load_seeds();
    long long ts = time(nullptr);

    // Format as specified in assignment:
    // Dead Node:<DeadNode.IP>:<DeadNode.Port>:<self.timestamp>:<self.IP>
    string dead_ip   = dead_peer.substr(0, dead_peer.find(":"));
    string dead_port = dead_peer.substr(dead_peer.find(":") + 1);
    string report    = "Dead Node:" + dead_ip + ":" + dead_port +
                       ":" + to_string(ts) + ":" + HOST;

    log_peer("[DEAD NODE REPORT] Sending to seeds: " + dead_peer);

    for (auto& seed : seeds) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(seed.second);
        inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            send_json(s, report);
        }
        closesocket(s);
    }
}


// PEER-LEVEL CONSENSUS: SUSPICION BROADCAST + VOTE HANDLING
/*
When we suspect a peer is dead (after PING_FAIL_THRESHOLD failures),
we broadcast a SUSPICION message to all our other neighbors asking
them to independently verify.

Message format: "TYPE:SUSPICION;TARGET:<IP:PORT>;REPORTER:<IP:PORT>"

Each neighbor that receives this will check their own ping_failures
for the target. If they also consider it dead, they send back a
SUSPICION_CONFIRM message.

Once a majority of neighbors confirm, we escalate to seeds.
*/
void broadcast_suspicion(const string& suspected_peer) {

    vector<string> targets;
    {
        lock_guard<mutex> lock(state_lock);
        for (auto& n : neighbors)
            if (n != suspected_peer)
                targets.push_back(n);
    }

    log_peer("[SUSPICION] Broadcasting suspicion about " + suspected_peer);

    string msg = "TYPE:SUSPICION;TARGET:" + suspected_peer + ";REPORTER:" + SELF_ID;

    for (auto& target : targets) {
        string ip   = target.substr(0, target.find(":"));
        int    port = stoi(target.substr(target.find(":") + 1));

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0)
            send_json(s, msg);

        closesocket(s);
    }
}


/*
Check if peer-level consensus has been reached for a suspected dead node.
Consensus = majority of current neighbors agree it is dead.
If yes: remove from neighbors, report to seeds, clean up state.
*/
void check_suspicion_consensus(const string& suspected_peer) {

    lock_guard<mutex> lock(state_lock);

    int votes    = suspicion_votes[suspected_peer].size();
    int required = max(1, (int)neighbors.size() / 2 + 1);

    if (votes >= required) {
        // Peer-level consensus reached — this node is confirmed dead

        log_peer("[DEAD NODE CONFIRMED] Peer-level consensus on: " +
                 suspected_peer + " | Votes: " + to_string(votes) +
                 "/" + to_string(neighbors.size()));

        // Remove from local neighbor set
        neighbors.erase(suspected_peer);
        last_seen.erase(suspected_peer);
        ping_failures.erase(suspected_peer);
        suspicion_votes.erase(suspected_peer);
        degree_map.erase(suspected_peer);
        peer_list.erase(suspected_peer);

        // Escalate to seeds (call outside lock to avoid deadlock)
        // We spawn a thread so we don't hold the lock during network I/O
        thread([suspected_peer]() {
            report_dead_to_seeds(suspected_peer);
        }).detach();
    }
}


// LIVENESS DETECTION: PING THREAD
/*
Runs in a background thread, pinging each neighbor every 10 seconds.

Uses system-level ping (Windows: ping -n 1 -w 1000 <IP>).
Return value 0 = host reachable, non-zero = unreachable.

Failure handling (two-stage):
  Stage 1 — Silent accumulation:
    Each failure increments ping_failures[neighbor].
    We do NOT react until PING_FAIL_THRESHOLD consecutive failures.

  Stage 2 — Suspicion phase (peer-level consensus):
    Once failures >= PING_FAIL_THRESHOLD, we broadcast a SUSPICION
    message to all other neighbors. We do NOT yet report to seeds.
    We only broadcast suspicion once (tracked via suspicion_votes
    already having an entry for this peer).

  Stage 3 — Confirmed dead (seed reporting):
    Once a majority of neighbors send back SUSPICION_CONFIRM,
    check_suspicion_consensus() removes the peer and reports to seeds.

On a successful ping: reset ping_failures and last_seen.
*/
void liveness_check_thread() {

    // Wait for topology to be established before starting pings
    this_thread::sleep_for(chrono::seconds(10));

    while (true) {

        vector<string> current_neighbors;
        {
            lock_guard<mutex> lock(state_lock);
            current_neighbors.assign(neighbors.begin(), neighbors.end());
        }

        for (auto& peer : current_neighbors) {

            string ip = peer.substr(0, peer.find(":"));

            // Windows ping: send 1 packet, wait up to 1000ms
            // Redirect output to NUL so nothing prints to console
            string cmd    = "ping -n 1 -w 1000 " + ip + " > NUL 2>&1";
            int    result = system(cmd.c_str());

            if (result == 0) {
                // Ping succeeded — reset failure counter
                lock_guard<mutex> lock(state_lock);
                ping_failures[peer] = 0;
                last_seen[peer]     = time(nullptr);

            } else {
                // Ping failed — increment failure counter
                int failures = 0;
                {
                    lock_guard<mutex> lock(state_lock);
                    ping_failures[peer]++;
                    failures = ping_failures[peer];
                }

                if (failures == PING_FAIL_THRESHOLD) {
                    // Threshold crossed — enter suspicion phase.
                    // We only broadcast once (when failures == threshold exactly,
                    // not on every subsequent failure)
                    bool already_suspected = false;
                    {
                        lock_guard<mutex> lock(state_lock);
                        // Add ourselves as a suspicion voter
                        suspicion_votes[peer].insert(SELF_ID);
                        already_suspected = (suspicion_votes[peer].size() > 1);
                    }

                    if (!already_suspected) {
                        // We're the first to suspect — broadcast to neighbors
                        broadcast_suspicion(peer);
                        // Also check if we alone constitute a majority
                        // (e.g., if we only have 1 neighbor total)
                        check_suspicion_consensus(peer);
                    }
                }
            }
        }

        // Ping every 10 seconds
        this_thread::sleep_for(chrono::seconds(10));
    }
}


// GOSSIP GENERATOR
/*
Generates 10 gossip messages, one every 5 seconds.
Message format (as specified in assignment):
  <timestamp>:<IP>:<PORT>:<MsgNumber>

Each message is hashed and stored in message_list before sending
to prevent forwarding our own messages back if they loop around.
*/
void gossip_generator() {

    // Wait for neighbor connections to be established
    this_thread::sleep_for(chrono::seconds(5));

    for (int i = 0; i < 10; i++) {

        long long timestamp = time(nullptr);

        // Format: timestamp:IP:PORT:MsgNumber
        string msg      = to_string(timestamp) + ":" + SELF_ID + ":" + to_string(i);
        string msg_hash = hash_message(msg);

        vector<string> targets;
        {
            lock_guard<mutex> lock(state_lock);
            message_list.insert(msg_hash);
            targets.assign(neighbors.begin(), neighbors.end());
        }

        log_peer("[GOSSIP GENERATED] " + msg);

        // Forward to all current neighbors
        for (auto& target : targets) {
            string ip   = target.substr(0, target.find(":"));
            int    port = stoi(target.substr(target.find(":") + 1));

            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            if (s == INVALID_SOCKET) continue;

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                string send_msg = "TYPE:GOSSIP;MSG:" + msg + ";SENDER:" + SELF_ID;
                send_json(s, send_msg);
            }
            closesocket(s);
        }

        this_thread::sleep_for(chrono::seconds(5));
    }
}


// PEER SERVER: HANDLE INCOMING MESSAGES
/*
Handles one incoming connection per call (short-lived connections).
Message types handled:

  TYPE:CONNECT_REQ   — A new peer wants to add us as a neighbor
  TYPE:GOSSIP        — A gossip message to forward if new
  TYPE:SUSPICION     — A neighbor suspects a peer is dead; we vote
  TYPE:SUSPICION_CONFIRM — A neighbor confirms our suspicion
*/
void handle_peer_client(SOCKET conn) {

    string message = recv_json(conn);

    if (message.empty()) {
        closesocket(conn);
        return;
    }

    // CONNECT REQUEST: Another peer is establishing a topology link
    if (message.find("TYPE:CONNECT_REQ") != string::npos) {

        string peer = message.substr(message.find("PEER:") + 5);
        {
            lock_guard<mutex> lock(state_lock);
            neighbors.insert(peer);
            last_seen[peer] = time(nullptr);
            ping_failures[peer] = 0;
            if (degree_map.find(peer) == degree_map.end())
                degree_map[peer] = 1;
            degree_map[SELF_ID]++;
        }
        log_peer("[NETWORK] Bidirectional link with " + peer);
    }

    // GOSSIP: Forward to all neighbors except the sender (flood fill
    // with deduplication via message_list hash check)
    else if (message.find("TYPE:GOSSIP") != string::npos) {

        size_t mpos = message.find("MSG:");
        size_t spos = message.find(";SENDER:");

        string msg    = message.substr(mpos + 4, spos - (mpos + 4));
        string sender = message.substr(spos + 8);

        string msg_hash = hash_message(msg);
        bool   is_new   = false;

        {
            lock_guard<mutex> lock(state_lock);
            if (message_list.find(msg_hash) == message_list.end()) {
                message_list.insert(msg_hash);
                is_new = true;
            }
        }

        if (is_new) {
            log_peer("[GOSSIP RECEIVED] " + msg + " | From: " + sender);

            // Forward to all neighbors except the one who sent it
            vector<string> targets;
            {
                lock_guard<mutex> lock(state_lock);
                for (auto& n : neighbors)
                    if (n != sender)
                        targets.push_back(n);
            }

            for (auto& target : targets) {
                string ip   = target.substr(0, target.find(":"));
                int    port = stoi(target.substr(target.find(":") + 1));

                SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
                if (s == INVALID_SOCKET) continue;

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(port);
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                    string fwd = "TYPE:GOSSIP;MSG:" + msg + ";SENDER:" + SELF_ID;
                    send_json(s, fwd);
                }
                closesocket(s);
            }
        }
    }

    // SUSPICION: A neighbor is asking "is TARGET dead for you too?"
    //
    // We check our own ping_failures for this target.
    // If we also consider it unresponsive (failures >= threshold),
    // we send back a SUSPICION_CONFIRM to the reporter.
    // We also add our vote to suspicion_votes regardless, in case
    // we haven't pinged them yet but our reporter has seen failures.
    else if (message.find("TYPE:SUSPICION") != string::npos) {

        size_t t1 = message.find("TARGET:");
        size_t t2 = message.find(";REPORTER:");

        if (t1 != string::npos && t2 != string::npos) {
            string target   = message.substr(t1 + 7, t2 - (t1 + 7));
            string reporter = message.substr(t2 + 10);

            bool we_also_suspect = false;
            {
                lock_guard<mutex> lock(state_lock);
                // We independently suspect if we've seen ping failures too
                if (ping_failures.count(target) &&
                    ping_failures[target] >= PING_FAIL_THRESHOLD) {
                    we_also_suspect = true;
                }
            }

            if (we_also_suspect) {
                // Send SUSPICION_CONFIRM back to the reporter
                string ip   = reporter.substr(0, reporter.find(":"));
                int    port = stoi(reporter.substr(reporter.find(":") + 1));

                SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
                if (s == INVALID_SOCKET) {
                    closesocket(conn);
                    return;
                }

                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(port);
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                    string confirm = "TYPE:SUSPICION_CONFIRM;TARGET:" + target +
                                     ";VOTER:" + SELF_ID;
                    send_json(s, confirm);
                }
                closesocket(s);
            }
        }
    }

    // SUSPICION_CONFIRM: A neighbor is confirming our suspicion.
    // Record their vote and check if we've reached majority consensus.
    else if (message.find("TYPE:SUSPICION_CONFIRM") != string::npos) {

        size_t t1 = message.find("TARGET:");
        size_t t2 = message.find(";VOTER:");

        if (t1 != string::npos && t2 != string::npos) {
            string target = message.substr(t1 + 7, t2 - (t1 + 7));
            string voter  = message.substr(t2 + 7);

            {
                lock_guard<mutex> lock(state_lock);
                suspicion_votes[target].insert(voter);
            }

            log_peer("[SUSPICION VOTE] " + voter + " confirms " + target + " is dead");

            // Check if majority consensus now reached
            check_suspicion_consensus(target);
        }
    }

    closesocket(conn);
}



// PEER SERVER THREAD: ACCEPT LOOP
void peer_server_thread() {

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);

    // Allow quick restart without "port already in use" error
    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    inet_pton(AF_INET, HOST.c_str(), &addr.sin_addr);

    bind(server, (sockaddr*)&addr, sizeof(addr));
    listen(server, SOMAXCONN);

    log_peer("[PEER STARTED] Listening on port " + to_string(PORT));

    while (true) {
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        thread(handle_peer_client, client).detach();
    }
}


/*
ENTRY POINT
*/

int main(int argc, char* argv[]) {

    if (argc < 3) {
        cout << "Usage: peer <IP> <PORT>\n";
        cout << "Example: .\\peer.exe 127.0.0.1 6001\n";
        return 1;
    }

    HOST    = argv[1];
    PORT    = stoi(argv[2]);
    SELF_ID = HOST + ":" + to_string(PORT);

    if (!initialize_winsock()) {
        cout << "Winsock failed\n";
        return 1;
    }

    // Open log file in append mode
    log_file.open("outputfile.txt", ios::app);

    // Start the server socket in background (must be before register
    // so we can receive CONNECT_REQ from other peers immediately)
    thread(peer_server_thread).detach();

    // Register with seeds and collect peer list
    register_and_get_peers();

    // Build overlay topology using preferential attachment
    build_topology();

    // Start gossip generation in background
    thread(gossip_generator).detach();

    // Start liveness detection in background
    // (begins pinging after 10s to let topology stabilize first)
    thread(liveness_check_thread).detach();

    // Keep main thread alive
    while (true)
        this_thread::sleep_for(chrono::seconds(1));

    cleanup_winsock();
    return 0;
}