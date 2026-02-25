import subprocess
import time
import sys
import os

# Windows flag to launch the process in a new terminal window
CREATE_NEW_CONSOLE = 0x00000010

processes = {}

def compile_code():
    print("[*] Cleaning up old files...")
    if os.path.exists("outputfile.txt"):
        os.remove("outputfile.txt")
        
    print("[*] Compiling seed.cpp...")
    seed_cmd = "g++ seed.cpp common.cpp -lws2_32 -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -o seed.exe"
    if subprocess.run(seed_cmd, shell=True).returncode != 0:
        print("[!] Failed to compile seed.cpp")
        sys.exit(1)

    print("[*] Compiling peer.cpp...")
    peer_cmd = "g++ peer.cpp common.cpp -lws2_32 -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -o peer.exe"
    if subprocess.run(peer_cmd, shell=True).returncode != 0:
        print("[!] Failed to compile peer.cpp")
        sys.exit(1)
    print("[*] Compilation successful!\n")

def start_node(node_type, ip, port):
    name = f"{node_type}_{port}"
    exe = "seed.exe" if node_type == "seed" else "peer.exe"
    cmd = f"{exe} {ip} {port}"
    
    print(f"[*] Starting {name}...")
    # Launch in a new console window so you can see its specific output
    p = subprocess.Popen(cmd, creationflags=CREATE_NEW_CONSOLE)
    processes[name] = p

def shutdown_all():
    print("\n[*] Shutting down all nodes...")
    for name, p in processes.items():
        if p.poll() is None:  # If process is still running
            p.terminate()
            print(f"[-] Terminated {name}")
    print("[*] Cleanup complete. Exiting.")
    sys.exit(0)

def main():
    compile_code()

    # 1. Start the 3 Seeds
    print("--- STARTING SEEDS ---")
    start_node("seed", "127.0.0.1", "5001")
    start_node("seed", "127.0.0.1", "5002")
    start_node("seed", "127.0.0.1", "5003")

    time.sleep(2) # Give seeds a moment to bind to their ports

    # 2. Start the 3 Peers
    print("\n--- STARTING PEERS ---")
    start_node("peer", "127.0.0.1", "6001")
    time.sleep(1) # Stagger starts slightly to allow topology to build gracefully
    start_node("peer", "127.0.0.1", "6002")
    time.sleep(1)
    start_node("peer", "127.0.0.1", "6003")

    print("\n=======================================================")
    print(" NETWORK RUNNING. Interactive Command Menu:")
    print("  - Type 'list' to see all running nodes.")
    print("  - Type 'kill <node_name>' (e.g., 'kill peer_6001') to test fault tolerance.")
    print("  - Type 'exit' to shut down everything and close the script.")
    print("=======================================================\n")

    try:
        while True:
            cmd = input("Command> ").strip().lower()
            if not cmd:
                continue
                
            if cmd == "exit" or cmd == "quit":
                break
                
            elif cmd == "list":
                print("Running nodes:")
                for name, p in processes.items():
                    status = "Running" if p.poll() is None else "Dead"
                    print(f"  - {name} [{status}]")
                    
            elif cmd.startswith("kill "):
                parts = cmd.split(" ")
                if len(parts) == 2:
                    target = parts[1]
                    if target in processes:
                        p = processes[target]
                        if p.poll() is None:
                            p.terminate()
                            print(f"[+] Successfully killed {target}. Watch other terminals for consensus!")
                        else:
                            print(f"[!] {target} is already dead.")
                    else:
                        print(f"[!] Node '{target}' not found. Use 'list' to see valid names.")
                else:
                    print("[!] Usage: kill <node_name>")
            else:
                print("[!] Unknown command.")
                
    except KeyboardInterrupt:
        pass
    finally:
        shutdown_all()

if __name__ == "__main__":
    main()