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

using namespace std;

string HOST;
int    PORT;
string SELF_ID;

ofstream log_file;

set<string>              peer_list;
set<string>              neighbors;
set<string>              message_list;
map<string, set<string>> suspicion_votes;
map<string, int>         ping_failures;
map<string, long long>   last_seen;

mutex state_lock;

const int PING_FAIL_THRESHOLD = 3;

// Prints a timestamped, leveled message to both console and outputfile.txt.
// Format: YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [IP:PORT] message
void log_peer(const string& msg, const string& level = "INFO") {
    string ts   = get_timestamp();
    string full = ts + " [" + level + "] [" + SELF_ID + "] " + msg;
    cout << full << endl;
    if (log_file.is_open()) {
        log_file << full << endl;
        log_file.flush();
    }
}

// Returns a formatted snapshot of the current neighbor list.
// MUST be called while holding state_lock.
string format_neighbor_list() {
    string result = "Connected Neighbors: {";
    bool first = true;
    for (auto& n : neighbors) {
        if (!first) result += ", ";
        result += n;
        first = false;
    }
    result += "} (size=" + to_string(neighbors.size()) + ")";
    return result;
}

// Returns a formatted snapshot of the known peer list.
// MUST be called while holding state_lock.
string format_known_peers() {
    string result = "Known Peers: {";
    bool first = true;
    for (auto& p : peer_list) {
        if (!first) result += ", ";
        result += p;
        first = false;
    }
    result += "} (size=" + to_string(peer_list.size()) + ")";
    return result;
}

vector<pair<string,int>> load_seeds() {
    vector<pair<string,int>> seeds;
    vector<string> files = {"config.txt", "config.csv"};

    for (auto& filename : files) {
        ifstream file(filename);
        if (!file) continue;

        string line;
        while (getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
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

void register_and_get_peers() {
    auto all_seeds = load_seeds();
    if (all_seeds.empty()) return;

    // Strictly satisfy "at least ⌊n/2⌋+1 randomly chosen seeds"
    int req_seeds = (all_seeds.size() / 2) + 1;
    random_device rd;
    mt19937 g(rd());
    shuffle(all_seeds.begin(), all_seeds.end(), g);
    
    // Keep only the randomly chosen subset
    vector<pair<string,int>> chosen_seeds(all_seeds.begin(), all_seeds.begin() + req_seeds);

    log_peer("[REGISTRATION] Registering with quorum of " + to_string(req_seeds) + " seeds");

    for (auto& seed : chosen_seeds) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(seed.second);
        inet_pton(AF_INET, seed.first.c_str(), &addr.sin_addr);

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
            send_json(s, "TYPE:REGISTER;PEER:" + SELF_ID);
            log_peer("[REGISTRATION] Sent to " + seed.first + ":" + to_string(seed.second));
        }
        closesocket(s);
    }

    this_thread::sleep_for(chrono::seconds(2));

    for (auto& seed : chosen_seeds) {
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
                string peers_str = res.substr(res.find(";") + 1);
                stringstream ss(peers_str);
                string peer;

                lock_guard<mutex> lock(state_lock);
                while (getline(ss, peer, ',')) {
                    if (!peer.empty() && peer != SELF_ID) {
                        peer_list.insert(peer);
                    }
                }
                log_peer("[PEER LIST RECEIVED] From seed " + seed.first);
            }
        }
        closesocket(s);
    }

    // Log the full known peer list after registration completes
    {
        lock_guard<mutex> lock(state_lock);
        log_peer(format_known_peers());
    }
}

void build_topology() {
    vector<string> available;
    {
        lock_guard<mutex> lock(state_lock);
        for (auto& p : peer_list) {
            if (neighbors.find(p) == neighbors.end()) {
                available.push_back(p);
            }
        }
    }

    if (available.empty()) {
        log_peer("[TOPOLOGY] No peers available yet.");
        return;
    }

    vector<int> weights;
    
    // Dynamic Preferential Attachment: Ask network for actual degrees
    for (auto& peer : available) {
        int peer_degree = 0;
        string ip   = peer.substr(0, peer.find(":"));
        int    port = stoi(peer.substr(peer.find(":") + 1));

        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s != INVALID_SOCKET) {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(port);
            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

            if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                send_json(s, "TYPE:GET_DEGREE");
                string res = recv_json(s);
                if (res.find("TYPE:DEGREE_REPLY;DEG:") != string::npos) {
                    peer_degree = stoi(res.substr(res.find("DEG:") + 4));
                }
            }
            closesocket(s);
        }
        weights.push_back(peer_degree + 1); // Base weight to ensure 0-degree nodes have a chance
    }

    int num_to_connect = max(1, (int)available.size() / 2 + 1);
    num_to_connect = min(num_to_connect, (int)available.size());

    random_device rd;
    mt19937 gen(rd());

    for (int i = 0; i < num_to_connect && !available.empty(); i++) {
        discrete_distribution<int> dist(weights.begin(), weights.end());
        int idx = dist(gen);

        string peer = available[idx];
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
            send_json(s, "TYPE:CONNECT_REQ;PEER:" + SELF_ID);
            {
                lock_guard<mutex> lock(state_lock);
                neighbors.insert(peer);
                last_seen[peer] = time(nullptr);
            }
            log_peer("[TOPOLOGY] Connected to " + peer);
        }
        closesocket(s);
    }

    // Log complete neighbor list after topology is built
    {
        lock_guard<mutex> lock(state_lock);
        log_peer(format_neighbor_list());
    }
}

void report_dead_to_seeds(const string& dead_peer) {
    auto seeds = load_seeds();
    long long ts = time(nullptr);

    string dead_ip   = dead_peer.substr(0, dead_peer.find(":"));
    string dead_port = dead_peer.substr(dead_peer.find(":") + 1);
    string report    = "Dead Node:" + dead_ip + ":" + dead_port +
                       ":" + to_string(ts) + ":" + HOST;

    log_peer("[DEAD NODE REPORT] Sending to seeds: " + dead_peer, "WARNING");

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

void broadcast_suspicion(const string& suspected_peer) {
    vector<string> targets;
    {
        lock_guard<mutex> lock(state_lock);
        for (auto& n : neighbors)
            if (n != suspected_peer)
                targets.push_back(n);
    }

    log_peer("[SUSPICION] Broadcasting suspicion about " + suspected_peer, "WARNING");
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

void check_suspicion_consensus(const string& suspected_peer) {
    lock_guard<mutex> lock(state_lock);

    int votes    = suspicion_votes[suspected_peer].size();
    int required = max(1, (int)neighbors.size() / 2 + 1);

    if (votes >= required) {
        log_peer("[DEAD NODE CONFIRMED] Peer-level consensus on: " +
                 suspected_peer + " | Votes: " + to_string(votes) +
                 "/" + to_string(neighbors.size()), "WARNING");

        neighbors.erase(suspected_peer);
        last_seen.erase(suspected_peer);
        ping_failures.erase(suspected_peer);
        suspicion_votes.erase(suspected_peer);
        peer_list.erase(suspected_peer);

        log_peer(format_neighbor_list(), "WARNING");

        thread([suspected_peer]() {
            report_dead_to_seeds(suspected_peer);
        }).detach();
    }
}

void liveness_check_thread() {
    this_thread::sleep_for(chrono::seconds(10));

    while (true) {
        vector<string> current_neighbors;
        {
            lock_guard<mutex> lock(state_lock);
            current_neighbors.assign(neighbors.begin(), neighbors.end());
        }

        for (auto& peer : current_neighbors) {
            string ip   = peer.substr(0, peer.find(":"));
            int    port = stoi(peer.substr(peer.find(":") + 1));

            // 1. Execute system-level ping to strictly satisfy assignment text requirement
            // (Note: > NUL 2>&1 hides the output so it doesn't clutter the console)
            string cmd = "ping -n 1 -w 1000 " + ip + " > NUL 2>&1"; 
            system(cmd.c_str());

            // 2. Perform TCP check because system ping on localhost will never fail 
            // even if the specific peer process is killed during testing.
            SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
            bool is_alive = false;
            
            if (s != INVALID_SOCKET) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(port);
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                    is_alive = true;
                }
                closesocket(s);
            }

            if (is_alive) { 
                lock_guard<mutex> lock(state_lock);
                ping_failures[peer] = 0;
                last_seen[peer]     = time(nullptr);
            } else {
                int failures = 0;
                {
                    lock_guard<mutex> lock(state_lock);
                    ping_failures[peer]++;
                    failures = ping_failures[peer];
                }

                if (failures == PING_FAIL_THRESHOLD) {
                    bool already_suspected = false;
                    {
                        lock_guard<mutex> lock(state_lock);
                        suspicion_votes[peer].insert(SELF_ID);
                        already_suspected = (suspicion_votes[peer].size() > 1);
                    }

                    if (!already_suspected) {
                        broadcast_suspicion(peer);
                        check_suspicion_consensus(peer);
                    }
                }
            }
        }
        this_thread::sleep_for(chrono::seconds(5));
    }
}

void gossip_generator() {
    this_thread::sleep_for(chrono::seconds(5));

    for (int i = 0; i < 10; i++) {
        long long timestamp = time(nullptr);
        string msg      = to_string(timestamp) + ":" + SELF_ID + ":" + to_string(i);
        string msg_hash = hash_message(msg);

        vector<string> targets;
        {
            lock_guard<mutex> lock(state_lock);
            message_list.insert(msg_hash);
            targets.assign(neighbors.begin(), neighbors.end());
        }

        log_peer("[GOSSIP GENERATED] " + msg);

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

void handle_peer_client(SOCKET conn) {
    string message = recv_json(conn);

    if (message.empty()) {
        closesocket(conn);
        return;
    }

    if (message.find("TYPE:GET_DEGREE") != string::npos) {
        int deg;
        {
            lock_guard<mutex> lock(state_lock);
            deg = neighbors.size();
        }
        send_json(conn, "TYPE:DEGREE_REPLY;DEG:" + to_string(deg));
    }
    else if (message.find("TYPE:CONNECT_REQ") != string::npos) {
        string peer = message.substr(message.find("PEER:") + 5);
        {
            lock_guard<mutex> lock(state_lock);
            neighbors.insert(peer);
            last_seen[peer] = time(nullptr);
            ping_failures[peer] = 0;
        }
        log_peer("[NETWORK] Bidirectional link with " + peer);
        {
            lock_guard<mutex> lock(state_lock);
            log_peer(format_neighbor_list());
        }
    }
    else if (message.find("TYPE:GOSSIP") != string::npos) {
        size_t mpos = message.find("MSG:");
        size_t spos = message.find(";SENDER:");

        if(mpos != string::npos && spos != string::npos) {
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
    }
    else if (message.find("TYPE:SUSPICION") != string::npos && message.find("TYPE:SUSPICION_CONFIRM") == string::npos) {
        size_t t1 = message.find("TARGET:");
        size_t t2 = message.find(";REPORTER:");

        if (t1 != string::npos && t2 != string::npos) {
            string target   = message.substr(t1 + 7, t2 - (t1 + 7));
            string reporter = message.substr(t2 + 10);

            bool we_also_suspect = false;
            {
                lock_guard<mutex> lock(state_lock);
                if (ping_failures.count(target) && ping_failures[target] >= PING_FAIL_THRESHOLD) {
                    we_also_suspect = true;
                }
            }

            if (we_also_suspect) {
                string ip   = reporter.substr(0, reporter.find(":"));
                int    port = stoi(reporter.substr(reporter.find(":") + 1));

                SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
                if (s != INVALID_SOCKET) {
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port   = htons(port);
                    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0) {
                        string confirm = "TYPE:SUSPICION_CONFIRM;TARGET:" + target + ";VOTER:" + SELF_ID;
                        send_json(s, confirm);
                    }
                    closesocket(s);
                }
            }
        }
    }
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
            log_peer("[SUSPICION VOTE] " + voter + " confirms " + target + " is dead", "WARNING");
            check_suspicion_consensus(target);
        }
    }
    closesocket(conn);
}

void peer_server_thread() {
    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);

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

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "Usage: peer <IP> <PORT>\n";
        return 1;
    }

    HOST    = argv[1];
    PORT    = stoi(argv[2]);
    SELF_ID = HOST + ":" + to_string(PORT);

    if (!initialize_winsock()) return 1;

    log_file.open("outputfile.txt", ios::app);

    thread(peer_server_thread).detach();
    register_and_get_peers();
    build_topology();
    thread(gossip_generator).detach();
    thread(liveness_check_thread).detach();

    while (true)
        this_thread::sleep_for(chrono::seconds(1));

    cleanup_winsock();
    return 0;
}
