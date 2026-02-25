import socket
import threading
import sys
import time
import random
from common import *

# Global configuration
HOST = sys.argv[1]
PORT = int(sys.argv[2])
SELF_ID = f"{HOST}:{PORT}"

# Thread-safe data structures
peer_list = set()
neighbors = set()      # Active connections (IP:PORT format)
message_list = set()   # ML for gossip hashes
suspicion_votes = {}   # Tracks how many neighbors suspect a node is dead
last_seen = {}         # Tracks the last successful ping time

state_lock = threading.Lock()
log_file = None  # Will be initialized in main

def log_peer(message):
    # Include node ID in log message for identification
    log_message = f"[{SELF_ID}] {message}"
    print(log_message)
    if log_file:
        try:
            log_file.write(log_message + "\n")
            log_file.flush()
        except:
            pass

def load_seeds():
    """
    Load seed node addresses from config file.
    Tries config.txt first, then config.csv if config.txt doesn't exist.
    Supports both formats as per assignment requirements.
    """
    seeds = []
    config_files = ["config.txt", "config.csv"]
    
    for config_file in config_files:
        try:
            with open(config_file) as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):  # Skip comments
                        # Handle CSV format (if comma-separated) or colon-separated
                        if "," in line:
                            ip, port = line.split(",")
                        else:
                            ip, port = line.split(":")
                        seeds.append((ip.strip(), int(port.strip())))
                if seeds:  # If we found seeds, break
                    break
        except FileNotFoundError:
            continue
        except Exception as e:
            print(f"Error loading {config_file}: {e}")
    
    if not seeds:
        print("Warning: No seeds found in config.txt or config.csv")
    
    return seeds

def register_and_get_peers():
    seeds = load_seeds()
    if not seeds:
        log_peer("[ERROR] No seeds available in config.txt or config.csv")
        return

    req_seeds = len(seeds) // 2 + 1
    chosen_seeds = random.sample(seeds, min(req_seeds, len(seeds)))
    log_peer(f"[REGISTRATION] Attempting to register with {req_seeds} out of {len(seeds)} seeds")

    # 1. Register
    for seed_ip, seed_port in chosen_seeds:
        retries = 3
        while retries > 0:
            try:
                s = socket.socket()
                s.settimeout(3)
                s.connect((seed_ip, seed_port))
                send_json(s, {"type": "REGISTER", "peer": SELF_ID})
                s.close()
                log_peer(f"[REGISTRATION] Sent registration to {seed_ip}:{seed_port}")
                break
            except Exception as e:
                retries -= 1
                if retries == 0:
                    log_peer(f"[REGISTRATION] Failed to register with {seed_ip}:{seed_port}")
                else:
                    time.sleep(0.5)

    time.sleep(2) # Wait for seeds to reach consensus

    # 2. Retrieve Peer Lists
    for seed_ip, seed_port in seeds:
        retries = 3
        while retries > 0:
            try:
                s = socket.socket()
                s.settimeout(3)
                s.connect((seed_ip, seed_port))
                send_json(s, {"type": "GET_PEERS"})
                res = recv_json(s)
                if res and res.get("type") == "PEER_LIST":
                    with state_lock:
                        for p in res.get("peers", []):
                            if p != SELF_ID:  # Don't add self
                                peer_list.add(p)
                    log_peer(f"[PEER LIST RECEIVED] From {seed_ip}:{seed_port}: {res.get('peers')}")
                s.close()
                break
            except Exception as e:
                retries -= 1
                if retries == 0:
                    log_peer(f"[WARNING] Could not fetch peer list from seed {seed_ip}:{seed_port}")
                else:
                    time.sleep(0.5)

def build_topology():
    """
    Implements Barabasi-Albert preferential attachment for a power-law degree distribution.
    Peers query available nodes for their current degree, and randomly select neighbors 
    weighted by those degrees.
    """
    with state_lock:
        available = list(peer_list - {SELF_ID} - neighbors)
    
    if not available:
        log_peer("[TOPOLOGY] No other peers available yet")
        return

    num_to_connect = max(1, min(3, len(available) // 2 + 1))
    
    # 1. Query the degrees of all available peers
    peer_degrees = {}
    for peer in available:
        ip, port_str = peer.split(":")
        try:
            s = socket.socket()
            s.settimeout(2)
            s.connect((ip, int(port_str)))
            send_json(s, {"type": "GET_DEGREE"})
            res = recv_json(s)
            if res and res.get("type") == "DEGREE_REPLY":
                peer_degrees[peer] = res.get("degree", 0)
            s.close()
        except:
            peer_degrees[peer] = 0 # Default to 0 if we can't reach them

    # 2. Calculate probabilities (Preferential Attachment)
    # Give a base weight of 1 so new nodes (degree 0) still have a chance to be picked
    weights = [peer_degrees[peer] + 1 for peer in available]
    
    # 3. Select neighbors using the calculated weights
    selected = []
    available_copy = available.copy()
    weights_copy = weights.copy()
    
    for _ in range(min(num_to_connect, len(available_copy))):
        if not available_copy: break
        # random.choices picks an element based on the provided weights
        chosen = random.choices(available_copy, weights=weights_copy, k=1)[0]
        selected.append(chosen)
        
        # Remove chosen so we don't connect to the same node twice
        idx = available_copy.index(chosen)
        available_copy.pop(idx)
        weights_copy.pop(idx)

    # 4. Connect to the selected peers bidirectionally
    for peer in selected:
        ip, port_str = peer.split(":")
        retries = 2
        while retries > 0:
            try:
                s = socket.socket()
                s.settimeout(3)
                s.connect((ip, int(port_str)))
                send_json(s, {"type": "CONNECT_REQ", "peer": SELF_ID})
                s.close()
                
                with state_lock:
                    neighbors.add(peer)
                    last_seen[peer] = time.time()
                log_peer(f"[TOPOLOGY] Connected to {peer} (Degree was {peer_degrees[peer]})")
                break
            except Exception as e:
                retries -= 1
                if retries == 0:
                    log_peer(f"[TOPOLOGY] Failed to connect to {peer}")
                else:
                    time.sleep(0.5)
    
    with state_lock:
        log_peer(f"[TOPOLOGY] Neighbors established: {neighbors}")

def handle_peer_client(conn, addr):
    try:
        data = recv_json(conn)
        if not data: 
            conn.close()
            return

        msg_type = data.get("type")

        if msg_type == "CONNECT_REQ":
            # Bidirectional connection established
            sender = data.get("peer")
            if sender and sender != SELF_ID:
                with state_lock:
                    neighbors.add(sender)
                    last_seen[sender] = time.time()
                log_peer(f"[NETWORK] Bidirectional link established with {sender}")

        elif msg_type == "GOSSIP":
            msg_str = data.get("message")
            sender = data.get("sender")
            
            if not msg_str:
                conn.close()
                return

            msg_hash = hash_message(msg_str)
            sender_addr = sender if sender else (f"{addr[0]}:{addr[1]}" if addr else None)

            with state_lock:
                is_new = msg_hash not in message_list
                if is_new:
                    message_list.add(msg_hash)

            if is_new:
                # Log only FIRST-TIME gossip as required
                # Extract timestamp from message for logging
                try:
                    parts = msg_str.split(":")
                    timestamp = parts[0] if len(parts) > 0 else "unknown"
                    log_peer(f"[GOSSIP RECEIVED] {msg_str} | From: {sender_addr} | Time: {timestamp}")
                except:
                    log_peer(f"[GOSSIP RECEIVED] {msg_str} | From: {sender_addr}")
                
                # Forward to all neighbors EXCEPT sender
                with state_lock:
                    targets = list(neighbors - {sender_addr} if sender_addr else neighbors)
                
                for target in targets:
                    ip, port = target.split(":")
                    retries = 2
                    while retries > 0:
                        try:
                            s = socket.socket()
                            s.settimeout(3)
                            s.connect((ip, int(port)))
                            send_json(s, {"type": "GOSSIP", "message": msg_str, "sender": SELF_ID})
                            s.close()
                            break
                        except:
                            retries -= 1
                            if retries == 0:
                                pass  # Connection failed, might trigger suspicion later

        elif msg_type == "GET_DEGREE":
            # Respond to degree queries for power-law topology construction
            with state_lock:
                degree = len(neighbors)
            send_json(conn, {"type": "DEGREE_REPLY", "degree": degree})
            conn.close()
            return

        elif msg_type == "SUSPECT_DEAD":
            # Peer-Level Consensus logic
            suspect = data.get("suspect")
            reporter = data.get("reporter")
            
            if not suspect or not reporter:
                conn.close()
                return
            
            log_peer(f"[PEER CONSENSUS] Received suspicion from {reporter} that {suspect} is dead")
            
            with state_lock:
                if suspect not in suspicion_votes:
                    suspicion_votes[suspect] = set()
                suspicion_votes[suspect].add(reporter)
                
                # If we also think it's dead (or haven't seen it), we add our vote
                if suspect in neighbors:
                    time_since_last = time.time() - last_seen.get(suspect, 0)
                    if time_since_last > 10:
                        suspicion_votes[suspect].add(SELF_ID)
                        log_peer(f"[PEER CONSENSUS] Added our vote for {suspect} (last seen {time_since_last:.1f}s ago)")

                # Check majority: need ⌊n/2⌋ + 1 of neighbors to agree
                num_neighbors = len(neighbors)
                required_votes = max(1, num_neighbors // 2 + 1) if num_neighbors > 0 else 1
                current_votes = len(suspicion_votes[suspect])
                
                log_peer(f"[PEER CONSENSUS] {suspect}: {current_votes}/{required_votes} votes from {num_neighbors} neighbors")

                # Consensus reached! Report to seeds.
                if current_votes >= required_votes:
                    log_peer(f"[PEER CONSENSUS] {suspect} confirmed dead by majority vote ({current_votes}/{num_neighbors} neighbors)")
                    timestamp = int(time.time())
                    target_ip, target_port = suspect.split(":")
                    log_peer(f"[DEAD NODE REPORT] Dead Node:{target_ip}:{target_port}:{timestamp}:{HOST}")
                    
                    # Remove from our neighbor list
                    if suspect in neighbors:
                        neighbors.remove(suspect)
                    if suspect in last_seen:
                        del last_seen[suspect]
                    if suspect in suspicion_votes:
                        del suspicion_votes[suspect]
                    
                    # Report to seeds
                    report_dead_node_to_seeds(suspect)

    except Exception as e:
        pass
    finally:
        conn.close()

def report_dead_node_to_seeds(dead_peer):
    """
    Report dead node to all seeds in the exact format required:
    Dead Node:<DeadNode.IP>:<DeadNode.Port>:<self.timestamp>:<self.IP>
    """
    dead_ip, dead_port = dead_peer.split(":")
    timestamp = int(time.time())
    report_str = f"Dead Node:{dead_ip}:{dead_port}:{timestamp}:{HOST}"
    
    seeds = load_seeds()
    for seed_ip, seed_port in seeds:
        retries = 2
        while retries > 0:
            try:
                s = socket.socket()
                s.settimeout(3)
                s.connect((seed_ip, seed_port))
                s.sendall(report_str.encode('utf-8'))
                s.close()
                break
            except:
                retries -= 1
                if retries == 0:
                    pass  # Seed might be down, continue to next

def peer_server_thread():
    server = socket.socket()
    server.bind((HOST, PORT))
    server.listen()
    log_peer(f"[PEER STARTED] Listening on {HOST}:{PORT}")
    while True:
        try:
            conn, addr = server.accept()
            threading.Thread(target=handle_peer_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            log_peer(f"[ERROR] Error accepting connection: {e}")

def gossip_generator():
    time.sleep(5) # Wait for topology to settle
    for msg_num in range(10):  # Generate 10 messages (0-9)
        timestamp = int(time.time())
        # Format: <timestamp>:<self.IP>:<self.Msg#> (3 fields separated by colons)
        # Using SELF_ID which is "IP:PORT" as the identifier
        msg_str = f"{timestamp}:{SELF_ID}:{msg_num}"
        msg_hash = hash_message(msg_str)
        
        with state_lock:
            message_list.add(msg_hash)
            targets = list(neighbors)

        log_peer(f"[GOSSIP GENERATED] {msg_str}")
        
        for target in targets:
            ip, port = target.split(":")
            retries = 2
            while retries > 0:
                try:
                    s = socket.socket()
                    s.settimeout(3)
                    s.connect((ip, int(port)))
                    send_json(s, {"type": "GOSSIP", "message": msg_str, "sender": SELF_ID})
                    s.close()
                    break
                except:
                    retries -= 1
                    if retries == 0:
                        pass  # Connection failed, might trigger suspicion later
        time.sleep(5)

def liveness_monitor():
    """
    Periodically checks if neighbors are alive using socket connection attempts.
    If a connection fails, initiates local suspicion phase.
    
    Note: The assignment says "system-level ping", but ICMP pings do not check ports.
    If you test this on 127.0.0.1, an ICMP ping will always return true even if the node is dead.
    This script uses a socket connection attempt to verify the specific port is alive.
    """
    while True:
        time.sleep(5)
        with state_lock:
            current_neighbors = list(neighbors)
            
        for peer in current_neighbors:
            ip, port = peer.split(":")
            is_alive = False
            try:
                # System ping check substitute for specific port
                # This verifies the peer service is actually running on the port
                s = socket.socket()
                s.settimeout(2)
                s.connect((ip, int(port)))
                s.close()
                is_alive = True
                with state_lock:
                    last_seen[peer] = time.time()
            except:
                is_alive = False

            if not is_alive:
                with state_lock:
                    time_since_last = time.time() - last_seen.get(peer, 0)
                
                # Local suspicion phase trigger
                if time_since_last > 10: 
                    log_peer(f"[PING FAILED] Neighbor {peer} is not responding to system-level ping")
                    log_peer(f"[LOCAL SUSPICION] {peer} missed pings. Initiating peer consensus.")
                    
                    with state_lock:
                        if peer not in suspicion_votes:
                            suspicion_votes[peer] = set()
                        suspicion_votes[peer].add(SELF_ID)
                        other_neighbors = list(neighbors - {peer})
                        
                    for neighbor in other_neighbors:
                        n_ip, n_port = neighbor.split(":")
                        retries = 2
                        while retries > 0:
                            try:
                                s = socket.socket()
                                s.settimeout(3)
                                s.connect((n_ip, int(n_port)))
                                send_json(s, {"type": "SUSPECT_DEAD", "suspect": peer, "reporter": SELF_ID})
                                s.close()
                                break
                            except:
                                retries -= 1
                                if retries == 0:
                                    pass  # Neighbor might be down too

if __name__ == "__main__":
    try:
        # Initialize log file - all nodes write to the same file
        log_filename = "outputfile.txt"
        log_file = open(log_filename, "a")
        log_peer(f"[PEER STARTED] {HOST}:{PORT}")
        
        threading.Thread(target=peer_server_thread, daemon=True).start()
        
        register_and_get_peers()
        build_topology()
        
        threading.Thread(target=gossip_generator, daemon=True).start()
        threading.Thread(target=liveness_monitor, daemon=True).start()
        
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            log_peer("[SHUTDOWN] Peer shutting down")
    except Exception as e:
        log_peer(f"[ERROR] Fatal error: {e}")
        if log_file:
            log_file.close()
        sys.exit(1)
    finally:
        if log_file:
            log_file.close()
