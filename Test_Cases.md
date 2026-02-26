# Test Cases — P2P Gossip Network

Below are systematic tests that validate every requirement of the assignment.
Each test describes the **setup**, **steps**, and **expected outcome**.

---

## Test 1: Seed Registration Consensus (⌊n/2⌋ + 1 quorum)

**Objective:** A new peer is only admitted to the network after a majority of seeds agree.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds (ports 5001-5003) | All seeds print `[SEED STARTED]` |
| 2 | Start 1 peer (port 6001) | Peer sends `TYPE:REGISTER` to ⌊3/2⌋+1 = 2 seeds |
| 3 | Observe seed logs | `[CONSENSUS PROGRESS]` until 2 votes arrive, then `[CONSENSUS OUTCOME] Peer added` |
| 4 | Verify peer list | All seeds include `127.0.0.1:6001` after consensus |

**Pass criteria:** Peer appears in `Current Peer List` on all seeds only **after** quorum (2/3) is reached.

---

## Test 2: Gossip Message Propagation (10 messages, SHA-256 dedup)

**Objective:** Each peer generates 10 gossip messages. Messages propagate to all neighbors. Duplicate messages (same SHA-256 hash) are dropped.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 3 peers | All peers build topology |
| 2 | Wait for gossip generation | Each peer logs `[GOSSIP GENERATED]` exactly 10 times |
| 3 | Observe forwarding | Peers log `[GOSSIP RECEIVED]` for messages from other peers |
| 4 | Send a duplicate message manually | The duplicate is **silently dropped** (no `[GOSSIP RECEIVED]` log for it) |

**Pass criteria:**
- 10 messages per peer.
- Every reachable peer receives each unique message exactly once.
- No duplicate processing in `outputfile.txt`.

---

## Test 3: Liveness Checking (Ping + TCP)

**Objective:** Each peer periodically pings its neighbors. If a neighbor is unreachable for 3 consecutive checks, suspicion is raised.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 3 peers, let topology form | Peers log `Connected Neighbors` |
| 2 | Kill one peer process (e.g. peer on 6002) | Remaining peers increment `ping_failures` |
| 3 | After 3 failed pings | Detecting peer logs `[SUSPICION] Broadcasting suspicion about 127.0.0.1:6002` |

**Pass criteria:** Suspicion is raised **only** after 3 consecutive failures (not fewer).

---

## Test 4: Peer-Level Suspicion Cross-Verification

**Objective:** A single peer's suspicion is not enough. Other peers must independently confirm before a node is declared dead.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 4 peers | Full mesh or partial mesh forms |
| 2 | Kill peer on port 6003 | Multiple peers eventually suspect 6003 |
| 3 | Suspicion broadcasts happen | Peers exchange `TYPE:SUSPICION` and `TYPE:SUSPICION_CONFIRM` messages |
| 4 | Quorum of suspicion votes | Detecting peers log `[DEAD NODE CONFIRMED]` once ⌊n/2⌋+1 neighbors agree |
| 5 | Observe updated neighbor list | Dead peer removed from `Connected Neighbors` |

**Pass criteria:** Dead node removal happens **only** after a majority of neighbors agree (not just one peer's local detection).

---

## Test 5: Dead Node Reporting to Seeds

**Objective:** Once a peer confirms a dead node, it reports to **all** seeds. Seeds then reach their own consensus to remove the dead peer from the global peer list.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Continue from Test 4 | Peer 6003 is dead and confirmed by peer-level consensus |
| 2 | Reporting peer sends `Dead Node:` to all seeds | Seeds log `[WARNING]` with the dead node message |
| 3 | Seeds relay `TYPE:SEED_DEAD_VOTE` to each other | Seeds log `[DEAD NODE CONSENSUS PROGRESS]` |
| 4 | Once seed quorum reached | Seeds log `[SEED CONSENSUS OUTCOME] Removed dead peer: 127.0.0.1:6003` |
| 5 | Verify updated peer list on seeds | `Current Peer List` no longer includes 6003 |

**Pass criteria:** Dead peer is evicted from **all** seeds' peer lists after seed-level quorum is reached.

---

## Test 6: Power-Law / Preferential Attachment Topology

**Objective:** Peers connect to others using preferential attachment (higher-degree nodes are more likely to receive new connections), producing a power-law degree distribution.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 6 peers | Peers register, get peer list, build topology |
| 2 | Examine neighbor lists | Some nodes have significantly more connections than others |
| 3 | Verify `TYPE:GET_DEGREE` queries | During `build_topology()`, peers request actual degrees from other peers |
| 4 | Run multiple times | The first few peers (who were available earliest) tend to accumulate more links |

**Pass criteria:**
- Degree distribution is not uniform.
- `GET_DEGREE` queries are visible in logs.
- Connections are weighted toward higher-degree nodes.

---

## Test 7: Bidirectional Connections

**Objective:** When peer A connects to peer B, both A and B add each other to their neighbor lists.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 2 peers (6001, 6002) | Peer 6001 connects to 6002 |
| 2 | Check 6001 logs | `[TOPOLOGY] Connected to 127.0.0.1:6002` |
| 3 | Check 6002 logs | `[NETWORK] Bidirectional link with 127.0.0.1:6001` |
| 4 | Both neighbor lists | 6001's neighbors include 6002, and 6002's neighbors include 6001 |

**Pass criteria:** Every connection appears in **both** peers' neighbor lists.

---

## Test 8: Seed-to-Seed Periodic Sync

**Objective:** Seeds periodically exchange peer lists to recover from missed votes or transient failures.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds | All seeds running |
| 2 | Register a peer with only 1 seed (bypass normal quorum) | That seed has the peer, others don't |
| 3 | Wait 30 seconds | Sync thread fires: `[SYNC] Periodic peer-list sync broadcasted` |
| 4 | Check other seeds | `[SYNC] Merged peer list from remote seed` appears, peer now visible on all seeds |

**Pass criteria:** Peer lists converge across all seeds within one sync cycle (≤ 30 seconds).

---

## Test 9: Timestamps and Log Levels

**Objective:** All log entries include a timestamp and a severity level for easier debugging.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start seeds and peers normally | All output starts running |
| 2 | Check `outputfile.txt` | Every line matches format: `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] [IP:PORT] message` |
| 3 | Verify levels | Normal operations → `[INFO]`, suspicions/dead nodes → `[WARNING]`, fatal errors → `[ERROR]` |

**Pass criteria:** No log line appears without a timestamp and level tag.

---

## Test 10: Multiple Peer Failures (Stress Test)

**Objective:** The network correctly handles multiple simultaneous peer deaths.

| Step | Action | Expected |
|------|--------|----------|
| 1 | Start 3 seeds + 5 peers | Full network operational |
| 2 | Kill 2 peers simultaneously | Surviving peers detect both failures |
| 3 | Suspicion and consensus runs | Both dead peers are independently confirmed and reported |
| 4 | Seeds remove both peers | `Current Peer List` shows 3 remaining peers on all seeds |

**Pass criteria:** Each dead peer is handled independently; one failure does not block or interfere with detection of the other.

---

## How to Run

```bash
# Compile
g++ seed.cpp common.cpp -lws2_32 -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -o seed.exe
g++ peer.cpp common.cpp -lws2_32 -std=c++17 -static-libgcc -static-libstdc++ -Wl,-Bstatic -lpthread -Wl,-Bdynamic -o peer.exe

# Or use the automation script
python run_all.py
```

All logs are written to a shared **outputfile.txt** — open it or use the `visualizer.html` to inspect results after a run.
