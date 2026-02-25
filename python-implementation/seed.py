import socket
import threading
import sys
import json
import time
from common import *

# Global storage for the network state
peer_list = set()  # Final list of verified peers
votes = {}  # Dictionary to track how many seeds have "voted" for a peer
# Format:
# {
#   "peerIP:PORT": set(seed_ports_that_voted)
# }
lock = threading.Lock() # Prevents data corruption when multiple threads edit votes/peer_list

dead_reports = {}
# Format:
# {
#   "IP:PORT": set(seed_ports_that_reported)
# }

def load_seeds():
    """
    Load seed node addresses from config file.
    Tries config.txt first, then config.csv if config.txt doesn't exist.
    Supports both formats as per assignment requirements.
    
    Returns:
        List of tuples (IP, port) for all seed nodes
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

SEEDS = load_seeds()

# Command-line arguments: e.g., 'python seed.py 127.0.0.1 5000'
HOST = sys.argv[1]
PORT = int(sys.argv[2])
log_file = None  # Will be initialized in main

def log_seed(message):
    """Log message to both console and file"""
    # Include node ID in log message for identification
    seed_id = f"{HOST}:{PORT}"
    log_message = f"[{seed_id}] {message}"
    print(log_message)
    if log_file:
        try:
            log_file.write(log_message + "\n")
            log_file.flush()
        except:
            pass

def broadcast_to_seeds(message):
    """
    Broadcast a message to all other seed nodes for consensus.
    When this seed receives a registration or dead node report, it tells ALL OTHER seeds.
    This creates distributed consensus across all seeds.
    
    Error handling:
        - If a seed is down (network partition), it's skipped
        - Retries are attempted for transient failures
        - System continues to function as long as majority of seeds are available
    """
    for ip, port in SEEDS:
        if port == PORT: # Don't send the message to yourself
            continue
        retries = 3
        while retries > 0:
            try:
                s = socket.socket() # socket creation
                s.settimeout(3) # prevents hanging if seed is down
                s.connect((ip, port))
                send_json(s, message)
                s.close()
                break
            except Exception as e:
                retries -= 1
                if retries == 0:
                    # If a seed is down, just skip it (network partition tolerance)
                    pass
                else:
                    time.sleep(0.5)

def handle_client(conn, addr):
    """The main logic for processing incoming network requests.
    Handles incoming peer and seed messages.
    Defensive parsing ensures resilience against malformed input.
    Handles both JSON messages and string-format dead node reports.
    """
    # Receive raw data to check message format
    try:
        raw_data = conn.recv(4096)
        if not raw_data:
            conn.close()
            return
        
        message_str = raw_data.decode('utf-8').strip()
        
        # Check if it's a dead node report string format: "Dead Node:..."
        if message_str.startswith("Dead Node:"):
            # Parse the dead node report format: Dead Node:<DeadNode.IP>:<DeadNode.Port>:<timestamp>:<reporter.IP>
            parts = message_str.split(":")
            if len(parts) >= 5:
                dead_ip = parts[1]
                dead_port = parts[2]
                timestamp = parts[3]
                reporter_ip = parts[4]
                target = f"{dead_ip}:{dead_port}"
                
                # Log the exact format as required by assignment
                log_seed(f"Dead Node:{dead_ip}:{dead_port}:{timestamp}:{reporter_ip}")
                
                with lock:
                    if target not in dead_reports:
                        dead_reports[target] = set()

                    if PORT not in dead_reports[target]:
                        dead_reports[target].add(PORT)
                        log_seed(f"[DEAD NODE PROPOSAL] Initiating consensus for removal of {target}")

                broadcast_to_seeds({
                    "type": "SEED_DEAD_VOTE",
                    "target": target,
                    "seed": PORT
                })
            conn.close()
            return
        
        # Otherwise, parse as JSON
        data = json.loads(message_str)
    except (json.JSONDecodeError, UnicodeDecodeError, Exception) as e:
        # If parsing fails, close connection
        conn.close()
        return

    msg_type = data.get("type")

    # A new peer wants to join the network
    if msg_type == "REGISTER":
        peer = data.get("peer")
        if not peer:
            conn.close()
            return
        
        log_seed(f"[REGISTRATION PROPOSAL] Received registration request from {peer}")
        
        with lock: # Thread-safe: ensure only one thread modifies 'votes' at a time
            if peer not in votes: # Initialize the vote set for this peer if it doesn't exist
                votes[peer] = set()

            votes[peer].add(PORT)  # Add yourself 
        
        # Tell other seeds: "Hey, I saw this peer, you should vote for them too!"
        # This is how we achieve distributed consensus: if a majority of seeds report seeing this peer, we consider it verified and add it to the peer list.
        broadcast_to_seeds({
            "type": "VOTE",
            "peer": peer,
            "seed": PORT
        })

    # Another seed is telling us they saw a peer
    elif msg_type == "VOTE":
        peer = data.get("peer")
        reporting_seed = data.get("seed") #data.get - returns None if "seed" key is not present, preventing KeyError

        if not peer or reporting_seed is None:
            conn.close()
            return
        
        with lock:
            if peer not in votes:
                votes[peer] = set() #creates an empty set to track votes for this peer

            votes[peer].add(reporting_seed) # Add the reporting seed's port to the vote set for this peer
            current_votes = len(votes[peer])
            required_votes = len(SEEDS) // 2 + 1

            if current_votes >= required_votes: #if majority of seeds have voted for this peer
                if peer not in peer_list:
                    peer_list.add(peer)
                    log_seed(f"[CONSENSUS OUTCOME] Peer added: {peer} | Votes: {current_votes}/{len(SEEDS)}")
                else:
                    log_seed(f"[CONSENSUS UPDATE] Peer {peer} already in list | Votes: {current_votes}/{len(SEEDS)}")
            else:
                log_seed(f"[CONSENSUS PROGRESS] Peer {peer} | Votes: {current_votes}/{required_votes} (need {required_votes})")

    # A peer is asking "Who else is online?"
    elif msg_type == "GET_PEERS":
        with lock:
            current_peers = list(peer_list) # convert set to list for JSON serialization

        send_json(conn, {
            "type": "PEER_LIST",
            "peers": current_peers
        })
        # this version of GET_PEERS prevents race condition during removal 
        # race condition is possible if a seed is in the process of removing a peer
        # while another seed is sending the peer list (could lead to inconsistent views of the network)
        
    # DEAD_REPORT is now handled above in the string format check
    # This JSON format handler is kept for backward compatibility but should not be reached
    elif msg_type == "DEAD_REPORT":
        target = data.get("target")
        if not target:
            conn.close()
            return
        # This should not happen as dead reports are now sent as strings
        # But handle it gracefully if it does
        log_seed(f"[DEAD NODE REPORT RECEIVED (JSON)] {target}")

        with lock:
            if target not in dead_reports:
                dead_reports[target] = set()

            if PORT not in dead_reports[target]:
                dead_reports[target].add(PORT)
                log_seed(f"[DEAD NODE PROPOSAL] Initiating consensus for removal of {target}")

        broadcast_to_seeds({
            "type": "SEED_DEAD_VOTE",
            "target": target,
            "seed": PORT
        })

    elif msg_type == "SEED_DEAD_VOTE":
        """
        Seed-level consensus for dead node removal.
        After peer-level consensus confirms a node is dead, seeds vote on removal.
        A peer is only removed from all seed PLs after majority (⌊n/2⌋ + 1) of seeds agree.
        
        This prevents:
        - Single seed from unilaterally removing peers
        - Network partitions from causing inconsistent state
        - Malicious seeds from removing legitimate peers
        """
        target = data.get("target")
        reporting_seed = data.get("seed")

        if not target or reporting_seed is None:
            conn.close()
            return

        with lock:
            if target not in dead_reports:
                dead_reports[target] = set()

            if reporting_seed not in dead_reports[target]:
                dead_reports[target].add(reporting_seed)
                log_seed(f"[SEED DEAD VOTE] Received vote from seed {reporting_seed} for removal of {target}")
            
            current_votes = len(dead_reports[target])
            required_votes = len(SEEDS) // 2 + 1

            if current_votes >= required_votes:
                # Majority consensus reached - remove peer from all seed PLs
                if target in peer_list:
                    peer_list.remove(target)
                    log_seed(f"[SEED CONSENSUS OUTCOME] Removed dead peer: {target} | Votes: {current_votes}/{len(SEEDS)}")
                else:
                    log_seed(f"[SEED CONSENSUS UPDATE] Dead peer {target} not in list | Votes: {current_votes}/{len(SEEDS)}")

                # Cleanup to prevent reprocessing
                if target in dead_reports:
                    del dead_reports[target]
            else:
                log_seed(f"[DEAD NODE CONSENSUS PROGRESS] {target} | Votes: {current_votes}/{required_votes} (need {required_votes})")

    conn.close()

def start_server():
    """Starts the TCP server to listen for peer and seed connections."""
    try:
        server = socket.socket()
        server.bind((HOST, PORT))
        server.listen()
        log_seed(f"[SEED STARTED] Running at {HOST}:{PORT}")

        while True:
            try:
                # Every time a new connection arrives, spin up a new thread to handle it
                conn, addr = server.accept()
                threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
            except Exception as e:
                log_seed(f"[ERROR] Error accepting connection: {e}")
    except Exception as e:
        log_seed(f"[FATAL ERROR] Server failed: {e}")
        if log_file:
            log_file.close()
        sys.exit(1)

if __name__ == "__main__":
    try:
        # Initialize log file - all nodes write to the same file
        log_filename = "outputfile.txt"
        log_file = open(log_filename, "a")
        start_server()
    except KeyboardInterrupt:
        log_seed("[SHUTDOWN] Seed shutting down")
        if log_file:
            log_file.close()
    except Exception as e:
        print(f"[FATAL ERROR] {e}")
        if log_file:
            log_file.close()
        sys.exit(1)