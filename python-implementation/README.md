# Gossip-Based P2P Network

## Description

This project implements a distributed peer-to-peer (P2P) network supporting reliable message dissemination, robust liveness detection, and consensus-driven membership management. The architecture prevents unilateral or malicious network alterations by strictly requiring multi-level agreement among peers and seed nodes before admitting or removing any node from the overlay network.

## Core Features

- **Seed Nodes**: Coordinate peer registration and removal using a majority voting consensus mechanism while remaining outside the gossip dissemination layer.
- **Peer Nodes**: Self-organize into a robust overlay network, generate periodic gossip messages, and actively monitor neighbor liveness via system-level socket connection attempts.
- **Power-Law Topology**: Network connections are explicitly structured using the Barabási–Albert preferential attachment model, ensuring nodes with higher degrees have a proportionally higher probability of being selected as neighbors.
- **Two-Level Consensus**: Peer registration requires agreement from at least $\lfloor n/2\rfloor+1$ seed nodes. Dead node removal requires a localized peer-level consensus followed by a global seed-level consensus.
- **Gossip Protocol**: Messages are broadcast using a strictly formatted protocol with cryptographic hashing (SHA-256) and Message List (ML) deduplication to eliminate infinite routing loops.

## File Structure

| File | Description |
|------|-------------|
| `seed.py` | The seed node server handling network registration, dead-node reports, and seed-level majority consensus. |
| `peer.py` | The peer node client/server handling topology generation, gossip dissemination, and peer-level fault detection. |
| `common.py` | Shared utilities for socket communication, JSON serialization, and message hashing. |
| `config.txt` | Configuration file storing the IP address and port pairs of the designated seed nodes. |
| `outputfile.txt` | The consolidated execution log of the network, with entries prefixed by node identifiers. |

## Execution Instructions

### Step 1: Setup Configuration

Ensure `config.txt` contains the IP addresses and ports of all seed nodes, formatted strictly as `IP:PORT` on separate lines (e.g., `127.0.0.1:5001`).

### Step 2: Initialize Seed Nodes

Open separate terminals for each seed node. Navigate to the project directory and execute the script with the IP and port as arguments.

```bash
python seed.py 127.0.0.1 5001
python seed.py 127.0.0.1 5002
python seed.py 127.0.0.1 5003
```

### Step 3: Initialize Peer Nodes

Open new terminals for the peer nodes. Execute the peer script with the assigned IP and port.

```bash
python peer.py 127.0.0.1 6001
python peer.py 127.0.0.1 6002
python peer.py 127.0.0.1 6003
```

### Step 4: Test Fault Tolerance

To verify liveness detection and consensus, manually terminate a running peer node using `Ctrl+C`. Observe the `outputfile.txt` to verify the local suspicion trigger, the peer-level consensus, and the final seed-level removal.

## Message Formats

**Gossip Protocol**: Strictly adheres to `<self.timestamp>:<self.IP>:<self.Msg#>`.

**Gossip Example**: `1734567890:127.0.0.1:5` (Note: For local testing, port identifiers may be mapped into the IP field to distinguish localhost nodes).

**Dead Node Report**: Strictly adheres to `Dead Node:<DeadNode.IP>:<DeadNode.Port>:<self.timestamp>:<self.IP>`.

**Dead Node Example**: `Dead Node:127.0.0.1:6002:1734567890:127.0.0.1`.

## Output Logging

All nodes log their required actions to both the console and the shared `outputfile.txt`. File writes utilize append mode with immediate flushing to guarantee thread-safe concurrent writes. Ping operations and duplicate gossip messages are explicitly excluded from the logs per the assignment specifications.

## Security Analysis & Attack Mitigation

### False Suspicion Mitigation

A peer might experience a temporary network partition and falsely assume a neighbor is dead. To prevent unilateral, false network removals, the system implements **Peer-Level Consensus**. When a node misses system pings, it is only flagged for "Local Suspicion." The reporting node must query the suspect's other neighbors. A Dead Node report is only generated and broadcast to the seeds if a strict majority of the suspect's active neighbors agree it is unresponsive.

### Malicious Collusion & Sybil Attack Prevention

A group of malicious peers could theoretically collude to forge a Dead Node report or spam bogus registrations. To mitigate this, the system implements **Seed-Level Consensus**. Registration requests require contacting at least $\lfloor n/2\rfloor+1$ seeds. Furthermore, seeds do not blindly trust dead-node reports from peers. Upon receiving a report, seeds exchange `SEED_DEAD_VOTE` messages. A peer is exclusively added to or purged from the network's global Peer Lists if a majority of the independent seed nodes reach a definitive consensus, neutralizing Sybil-style disruption attempts.
