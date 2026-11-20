# Common Lisp (SBCL), no framework: io_uring, hand-written HTTP/JWT/JSON, SQLite

One Lisp thread runs everything: an io_uring event loop over raw syscalls (no liburing), an HTTP/1.1 parser, a JSON
writer and parser, HS256 verification, and SQLite (the pinned, unmodified amalgamation). All socket
buffers, response arenas and scratch space live in foreign memory and are read and written with
integer addresses (`a-ref-8`). Nothing outside the SBCL core is used: no Quicklisp, no contribs (no `sb-bsd-sockets`),
only `sb-alien`, `sb-sys`, `sb-ext`, `sb-kernel` and `sb-unix`.

| | |
|---|---|
| Language | Common Lisp, SBCL **2.6.10** (official `sbcl-2.6.10-x86-64-linux-binary.tar.bz2`, installed to `/usr/local` by `install.sh`) |
| SQLite | **3.54.1** amalgamation, `sqlite-amalgamation-3540100.zip`, sha256 `0a9c8e34...6ad1f9` checked by `build.sh`, compiled unmodified with gcc `-O3 -march=native` |
| Nginx or direct | **Direct, no Nginx**: the server listens on `HOST:PORT` itself (`0.0.0.0:80` on the benchmark box) |
| Dependencies | none beyond `build-essential curl unzip bzip2 ca-certificates` from apt |

Built by Anthony Taylor directing AI coding agents.

## Where the pinned hashes come from

`install.sh` checks two hashes of the SBCL tarball:

- `96e26b0b...eba726` for the `.tar.bz2` as served by SourceForge (computed when the entry was written);
- `011f411d...5c663d0` for the decompressed `.tar`, which is the value SBCL publishes in its
  PGP-signed checksum file `sbcl-2.6.10-crhodes.asc` (signed by Christophe Rhodes, key
  `D6839CA0A67F74D9DFB70922EBD595A9100D63CD`; verified with `gpg --decrypt` before pinning).

## Build

`build.sh` (normal user, `SBCL=/path/to/sbcl` overrides `/usr/local/bin/sbcl`; it refuses anything but 2.6.10):

1. fetches and sha256-checks the SQLite amalgamation into `vendor/`;
2. compiles `csrc/shim.c`, which `#include`s the unmodified `sqlite3.c`, into `build/shim.so` with
   `gcc -O3 -march=native -fno-plt -fPIC -shared` and these defines:
   `SQLITE_THREADSAFE=2 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 SQLITE_DQS=0
   SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_MAX_EXPR_DEPTH=0 SQLITE_OMIT_DEPRECATED
   SQLITE_OMIT_PROGRESS_CALLBACK SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_LOAD_EXTENSION
   SQLITE_OMIT_AUTOINIT SQLITE_OMIT_UTF16 SQLITE_OMIT_TRACE SQLITE_OMIT_COMPLETE
   SQLITE_OMIT_GET_TABLE SQLITE_OMIT_TCL_VARIABLE SQLITE_USE_ALLOCA HAVE_FDATASYNC=1 HAVE_USLEEP=1`
   (modeled on `rust-iouring-kensac`'s `build.rs`). `THREADSAFE=2` because every connection is used by
   one thread only: the event loop owns one, and the checkpoint thread planned for milestone 2 will own
   its own, so SQLite needs no mutexes;
3. starts SBCL with `--dynamic-space-size 512MB`, loads `shim.so` by absolute path, compiles
   `src/*.lisp` and runs `save-lisp-and-die "bin/server" :executable t :save-runtime-options t`.
   The saved executable re-opens `shim.so` from the recorded absolute path, so it starts from any
   directory.

`start.sh` execs `bin/server`, which is configured by `SQLITE_PATH`, `JWT_SECRET`, `HOST` and `PORT` only.
Optional variables exist for measuring and testing and are never set on the benchmark box: `STATS=1` (a
counter line on stderr every 5 s), `CONSED_LOG=1` (bytes consed and the request count on SIGUSR1 and on
exit), `TUNE`, `NO_BTREE`, `PERFMAP`, for M8 `PULSE_MS` (write pulse, default 20; 0 commits in every
pass), `CKPT_FRAMES` (4096), `CKPT_FORCE` (65536) and `CKPT_DELAY_MS` (0; delays the checkpoint thread
like a slow disk), and the four M9 switches, all off by default: `WALBUF=1`, `INLINE=1`, `PULSE_MAX_MS=n`
and `QUEUE=newest`.

## Layout

| File | What |
|---|---|
| `csrc/shim.c` | SQLite amalgamation plus `shim_open` (unix-excl VFS, pragmas), `shim_prepare`, the B-tree cursor wrappers, the checkpoint thread and the optional WAL-buffering VFS (`WALBUF=1`) |
| `src/tune.lisp` | every tunable in one place: ring sizes, batch caps, turn-time target, `io_uring_enter` wait |
| `src/ffi.lisp` | libc and io_uring syscalls and `sqlite3_*` entry points as `sb-alien` routines |
| `src/http.lisp` | request parser over foreign memory (split reads, pipelining, `Expect: 100-continue`), fixed responses as precomputed octet vectors, response header writer |
| `src/json.lisp` | JSON writer into the response arena; strict JSON validator that finds named top-level fields without allocating |
| `src/jwt.lisp` | SHA-256, HMAC-SHA256 with the inner/outer pad states computed once at startup, base64url, constant-time signature compare, `alg`/`exp`/`sub`/`username` checks |
| `src/db.lisp` | prepared statements kept for the life of the process; group-commit transaction |
| `src/btree.lisp` | B-tree cursor reads and record decoders for `/feed`, `/posts/:id`, like existence |
| `tools/equiv.sh` | B-tree path vs SQL path response-byte equivalence test |
| `tools/pulse.sh` | write-pulse checks: `pulse_edge.py` (pipelining, closes, bursts) and acknowledged-vs-stored accounting under five settings, driven by `loadgen.c` |
| `src/routes.lisp` | routing, auth-then-id order, validation |
| `src/server.lisp` | io_uring loop and the executable's toplevel |

## Behavior

- **No caching.** Every `GET` steps a prepared statement against SQLite while the request is served;
  `like_count` is computed per request. Every request verifies the token's HS256 signature and `exp`.
- **Durability.** WAL, `synchronous=NORMAL`, `cache_size=-65536`, `busy_timeout=5000`, through SQLite's
  `unix-excl` VFS. Writes are group-committed (rule 6): the writes of one pulse share one transaction, and
  none of their responses is handed to the kernel before its `COMMIT` returns. A failed commit rolls back
  and every write of that pulse answers 500, never a 201. Checkpoints run on a second thread (M8 below).
- **Connections.** HTTP/1.1 keep-alive, idle connections are kept for 300 s (the spec asks for at least
  65 s), `MSG_NOSIGNAL` on every send, up to 65,535 descriptors.
- **Health.** `/health` runs `SELECT 1`; on failure it answers 503 with the degraded body.

## Optimizations (milestone 2)

- **io_uring, raw syscalls** (425/426/427), rings mmapped by hand. `DEFER_TASKRUN | SINGLE_ISSUER`, no SQPOLL.
  Registered sparse direct-descriptor tables (65,535 slots per ring, another ring is added when one fills),
  8 `SO_REUSEPORT` listeners with multishot accept, multishot recv into provided-buffer rings, retry for the
  Ubuntu 6.8.0-136+ buffer-ring `resv` bug. Only 6.8 features are used.
- **Group commit.** The first write of a pass opens a transaction; one `COMMIT` ends the pass, and write
  responses are sent only after it returns. A failed commit rolls back and every write of that pass gets a 500.
  (Since M8 writes commit once per pulse instead of once per pass; see "The write pulse" below.)
- **Queue scheduling**, using only the server's own queue and timings: drain first (completions are copied into a
  per-connection pending buffer and the kernel buffer is returned at once), answer oldest first (a FIFO stamped
  with the read time), and an adaptive answer cap per pass derived from the measured turn time. The caps and the
  wait are named parameters in `src/tune.lisp`.
- **Low-priority helper thread** in C (invisible to SBCL's GC): PASSIVE WAL checkpoints on its own connection,
  `/proc/meminfo` every 200 ms, stats logging. (Until M8 it ran at `SCHED_IDLE` and its checkpoints did
  nothing: see "The write pulse" below. It now runs at nice 19 and does all the checkpointing.)
- **Zero consing on the hot path.** Buffers and arenas are foreign memory addressed by fixnums (a SAP held in a
  variable is heap-allocated); `(speed 3) (safety 0)`, no efficiency notes in the build log. Measured: 0 bytes
  per call for parse+handle of `/feed`, `/posts/:id` and `/health`, and 0 bytes consed over 313,068 requests
  with 0 GCs in the 20,000-user run.
- **Batching at high load (M2b).** With 256 or more connections open, ring 0 always waits in one
  `io_uring_enter` (up to the 1 ms timeout) instead of returning on the first completion
  (`+batch-open+` in `src/tune.lisp`). The arena restarts at its front whenever every send from it has finished.
  JSON strings are copied in runs with one `memmove`, escaping only the bytes that need it. Every `/feed`
  reads its rows live from SQLite (rule 5).
- **B-tree cursor reads (M3, PR #153).** The organiser allowed reading through SQLite's own B-tree.
  `csrc/shim.c` wraps the unmodified amalgamation's cursor API (`sqlite3BtreeCursor`, `TableMoveto`, `IndexMoveto`, `First`,
  `Next`, `Payload`) in a few thin functions; the cursors are opened inside the pass's read
  transaction and closed before its COMMIT. `src/btree.lisp` decodes the records (header varints, serial
  types) with `sap-ref-*` and writes the JSON straight into the arena, consing nothing. Used for
  `GET /feed`, `GET /posts/:id` and the like handler's existence check; writes stay on prepared SQL.
  `/feed` reads the newest 20 posts off `posts_created_at_id_idx`, then the post rows, authors and like counts
  (one range walk of the likes index) live on every request. Nothing is kept between requests except the
  cursors and four root page numbers.
  At startup `td_bt_init` reads `sqlite_schema`, checks the table and index SQL and column order the decoders
  assume and resolves the root pages. On any surprise it logs why and the prepared-SQL path is used (`NO_BTREE=1`
  forces that path). A record that is not in the expected form falls back to SQL for that request.
  `tools/equiv.sh` runs one SQL-path and one B-tree-path server on identical databases and compares the bytes of
  2,000 random `GET /posts/:id` and 200 `GET /feed` (with and without tokens): 2,208 requests, 0 mismatches.
- **Measured, 20,000 users on loopback, server CPU over the hold** (about 313k requests each, back to back,
  no response or result caching anywhere). The M2b body cache for `/feed` was removed (rule 5), so M2b is
  re-measured without it:

  | | cpu_s | us/req | p50 / p95 / p99 ms | errors | RSS |
  | --- | --- | --- | --- | --- | --- |
  | M2b, prepared SQL, no cache | 5.73 | 18.3 | 1.0 / 1.9 / 2.1 | 0 | 56 MB |
  | M3, B-tree cursors | 4.38 | 14.0 | 1.0 / 1.9 / 2.1 | 0 | 57 MB |
  | kensac (reference) | 4.17 | 13.3 | 1.0 / 1.9 / 2.0 | 0 | 56 MB |

  (The earlier 14.4 us/req for M2b was measured with the cache; the honest M2b figure is 18.3.)
  Bytes consed stay flat (12,920,496 before and after the run), 0 GCs, with the cursors on.
  `io_uring_enter` calls fell from about 2.2 per request (M2) to 0.56 (kensac: 0.56).

- **CPU cuts (M3b), 20,000 users on loopback, alternating runs, server user/sys us/req over the hold.**
  Kept: `(speed 3) (safety 0)` in `json/http/routes/db/btree/jwt` (they had been compiled at speed 1), an
  integer-to-decimal writer that counts digits by comparison, and a JSON-escape scan that skips 8 clean bytes
  per step (exact SWAR test for `<32`, `"` and `\`). Both A/B pairs, same hold, 0 errors, 0 GCs, bytes consed flat:

  | build | user | sys | total |
  | --- | --- | --- | --- |
  | M3 (e4923ec), run 1 / run 2 | 5.33 / 6.07 | 8.45 / 8.43 | 13.78 / 14.49 |
  | M3b, run 1 / run 2 | 4.40 / 4.37 | 8.68 / 8.74 | 13.08 / 13.11 |

  Tried and dropped: `IORING_REGISTER_RING_FDS` + `IORING_ENTER_REGISTERED_RING` (sys 9.00 vs 8.7, no gain in
  one run). The remaining gap is kernel-side: sys is about 2/3 of the cost, user is about 4.4 us/req.
  Custom VOPs were not attempted: user time is only about 4.4 us/req, so a VOP could not recover more than a small slice of the 13 us total. The sb-sprof/perf profile for this lane was captured but not analysed in depth.

- **Safety-0 fuzz gate (`tools/fuzz.sh`).** 20,947 malformed requests (truncation and byte flips at every offset of six request
  shapes, bad/negative/huge Content-Length, no-colon and 64 KB headers, bare CRLFs, garbage/over-long JWTs, bad ids and bodies)
  against one server process: same pid, 0 5xx (200: 601, 201: 263, 400: 6459, 401: 6958, 404: 2101, 413: 6), still serving, 42/42 on a fresh database afterwards.
- **Back-to-back 20k loopback, server us/req (2 alternating runs):** e4923ec 14.51 / 13.79, 6f50a67 12.61 / 13.60, kensac 11.94 / 11.13.
  M3b is about 1 us/req (7%) cheaper than M3 but still about 1.6 us/req behind kensac; the gap is user time (about 4.4 vs 2.7 us), sys is similar.

## The Lisp tricks (M3c): what was tried, what shipped

Nothing from this round shipped in the code; the verdicts are below. All numbers are the 1 vCPU VM, 200k users, alternating base (11e396a) / candidate, two pairs each.

| trick | candidate | req/s | p95 ms | p99 ms | err % | CPU % | us/req | RSS MB | MemAvail MB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| micro-opts (digit-pair W-INT, inline short copy, local-position REC-COLS) | base 1 / 2 | 21167 / 21160 | 2.18 / 2.27 | 113 / 130 | 0.001 | 72.7 / 73.2 | 32.6 / 32.9 | 124 / 124 | 921 / 919 |
| | new 1 / 2 | 21151 / 21159 | 2.88 / 2.13 | 280 / 122 | 0.001 | 74.1 / 72.8 | 33.3 / 32.6 | 134 / 125 | 908 / 919 |
| 1. boot-time schema compilation of record readers | base 1 / 2 | 21156 / 21156 | 2.11 / 2.11 | 64 / 94 | 0.001 | 72.7 / 72.9 | 32.7 / 32.8 | 122 / 124 | 924 / 923 |
| | new 1 / 2 | 21159 / 21152 | 2.13 / 2.03 | 145 / 78 | 0.001 | 72.9 / 73.0 | 32.7 / 32.8 | 155 / 147 | 897 / 905 |

- **Micro-opts: dropped.** No pair came out lower on us/req (+0.7, then -0.3). Within run-to-run noise, so they do not meet the "both pairs lower" bar and were reverted.
- **1. Schema-compiled readers: dropped.** The generator worked (layouts `posts=ritt users=rtt feed index=ti` read from the schema, `COMPILE` took about 30 ms, 42/42, equivalence 2208/0 mismatches, fuzz gate OK), but us/req is identical (32.7 vs 32.7 and 32.8 vs 32.8) and RSS grows by about 25 MB because the compiler stays resident. The generic reader's cost is not where the user time goes. Code is in git history (3991813) and reverted.
- **2. Early-run self-tuning: not attempted.** The first trick showed per-record decode is not a lever, and scheduling constants were already tuned in M2b/M3b; there was no budget left to build the sampler.
- **3. One sweep per pass: not attempted.**
- A single 300 s hold at 250k with the micro-opt build (not repeated): 26.4k req/s, p95 127 ms, p99 795 ms, 0 errors, CPU 93%. Base failed that hold on p99 (1.22 s) in the earlier run, but one run is not proof of a gain; the 200k pairs show no CPU difference.

## Status

Milestones 2 and 3 (B-tree cursor reads) are done. Macro-specialized responses and CPUID-dispatched byte
routines are later milestones.

### VM ceiling, before and after M3 (1 vCPU / 2 GB score-mode replica, `peek/probe.sh`, lg, 60 s ramp + 120 s hold)

M3 = e4923ec (B-tree cursor reads, PR #153), measured 2026-11-02; M2 = c001df7 from the earlier peek-m2 run.
sys share = CPU % minus user %.

| build | users | pass | req/s | p95 ms | p99 ms | err % | CPU % | us/req | user % / sys % | RSS MB | MemAvail MB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M2 | 200k | yes | 21,141 | 15.8 | 400 | 0.001 | 85 | 38.3 | n/a | 183 | 861 |
| M2 | 225k | no | 22,666 | 1459 | 1612 | 0.001 | 98 | 40.9 | n/a | 203 | 761 |
| M2 | 250k | no | 22,118 | 2957 | 3139 | 0.001 | 99 | 42.1 | n/a | 239 | 655 |
| M3 | 200k | yes | 21,160 | 2.5 | 130 | 0.001 | 73.8 | 33.2 | 17.8 / 56.0 | 123 | 919 |
| M3 | 225k | yes | 23,800 | 10.1 | 318 | 0.001 | 85.2 | 34.1 | 15.8 / 69.4 | 134 | 829 |
| M3 | 250k | no | 25,197 | 1010 | 1628 | 0.001 | 98.4 | 36.8 | 13.4 / 85.0 | 154 | 722 |

The VM ceiling moved from 200k to 225k users (250k fails; 225k-250k not bisected). The only errors are 24
connection resets per run, as in M2. About 70% of the per-request CPU is kernel time, so further user-side work
has limited reach. 27 us/req, the estimated target for the leaders' 310k, was not reached (34 us/req at 225k).

### Profiling and idle sweep (M4, phase A)

- `PERFMAP=1` (off by default, zero cost when unset) installs a SIGUSR1 handler that writes
  `/tmp/perf-<pid>.map` (`start size name` for every Lisp function). `perf script -F ip,dso` samples in the
  `server` dso resolve against it. At 150k users in the VM, Lisp code is 7.3% of server CPU time (kernel 84%,
  SQLite in `shim.so` 7.9%); kensac's non-SQLite user code is about 3%. SQLite itself is equal in both
  (`sqlite3BtreeTableMoveto` 2.3% each). Top Lisp functions: W-JSON-STRING-BODY 12.5% of Lisp time, BT-FEED 10%,
  REC-COLS 8%, ON-RECV 7%, BT-FEED-ROWS 6%, W-INT 5.5%, PARSE-REQUEST 5%, SWEEP-STEP 4.8%, SHA256-BLOCK 4.6%.
- The idle sweep is paced by the clock (one full cycle of the slot space per 20 s) instead of 128 slots every
  pass; it walks cold connection records. Back-to-back at 200k, same VM: 33.2 -> 32.6 and 33.1 -> 32.6 us/req;
  total CPU ticks over 30 s 2320 -> 2304 (user 338 -> 336). A gain of about 1.5%, inside run noise.
- `kill -TERM` with PROFILE unset: the server exits in 0.005 s idle and 0.056 s under load.

### VM ceiling, M4a (sweep build 5add836; 300 s holds use a copy of probe.sh with HOLD=300)

us/req = CPU % x 1e6 / req/s (probe.sh's own us/req column is wrong for 300 s holds: ignore the 85-86 it prints).

| users | hold | pass | req/s | hold p95 ms | hold p99 ms | err % | CPU % | us/req | RSS MB | MemAvail MB |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 225k | 300 s | yes | 23,802 | 2.2 | 125 | 0 | 82.5 | 34.7 | 152 | 812 |
| 250k | 120 s, 8fd1c8d | yes | 26,457 | 24 | 827 | 0.001 | 92.3 | 34.9 | 140 | 752 |
| 250k | 120 s, sweep | yes | 26,443 | 114 | 757 | 0.001 | 93.2 | 35.2 | 136 | 746 |
| 250k | 120 s, sweep (repeat) | no | 25,669 | 990 | 1518 | 0.001 | 97.9 | 38.1 | 205 | 683 |
| 250k | 300 s, sweep | no | 26,353 | 306 | 1220 | 0 | 92.4 | 35.1 | 213 | 675 |
| 275k | 120 s, sweep | no | 27,036 | 1834 | 2108 | 0.001 | 97.6 | 36.1 | 234 | 562 |

Ceiling: 225k held for 5 minutes. 250k passes 2-minute probes about half the time and fails the 5-minute hold.
CPU runs out first (97-98% at the failing runs with 560-680 MB still available).

## Kernel-side comparison (M5)

Ring design ideas from kensac (Rust) and pavlyhalim (C); no code copied. Counters were taken in the
1 vCPU score-mode VM over a 20 s window of a held 150k-user load (`perf stat -a`, `/proc/stat`,
`/proc/softirqs`, per-thread `/proc/<pid>/task/*/status`), all three servers on the same VM and seed.
Per request means per HTTP request completed in the window (TCP segments in/out are ~2.0 / ~1.93 for
all three, so the network side is identical).

| per request, 150k | Lisp 11e396a | kensac | pavlyhalim |
|---|---|---|---|
| probe us/req (CPU% x 1e6 / rps) | 32.7 | 29.5 | 28.6 |
| user us | 5.8 | 3.3 | 2.5 |
| sys us | 13.9 | 13.3 | 11.9 |
| softirq us | 16.4 | 15.6 | 16.8 |
| syscalls (all) | 0.95 | 0.64 | 0.37 |
| io_uring_enter | 0.43 | 0.10 | 0.11 |
| pwrite64 (WAL) | 0.49 | 0.47 | 0.11 (frames batched into one write) |
| context switches (sched_switch) | 0.060 | 0.075 | 0.173 |

(User, sys and softirq rows are /proc/stat ticks over the window divided by requests; they do not sum
to the probe figure, which is process CPU from the guest's stats. Compare columns between servers.)

The server threads cause nearly all switches; the SBCL finalizer and helper threads are idle
(100 voluntary switches in 20 s, about 0.0002 per request). So the extra nonvoluntary switches seen in M4
were the main thread being preempted, not helper threads.

| design item | ours | kensac | pavlyhalim |
|---|---|---|---|
| setup flags | CQSIZE, SUBMIT_ALL, SINGLE_ISSUER, DEFER_TASKRUN | same four | CQSIZE, CLAMP, SUBMIT_ALL, COOP_TASKRUN (+ others in sys.h) |
| SQPOLL | no | no | no |
| accept | multishot, 8 SO_REUSEPORT listeners | SO_REUSEPORT listeners | multishot, NLISTEN = 8 |
| recv | multishot into provided-buffer ring | multishot, provided-buffer ring | multishot, provided-buffer ring |
| fixed files | sparse table, direct descriptors | sparse table | FILES2 table, IOSQE_FIXED_FILE |
| registered buffers | no | no | no |
| send | one send per response, hard-linked close | one send per response, IO_HARDLINK | MSG_WAITALL send, HARDLINK + CQE_SKIP_SUCCESS |
| socket options | NODELAY (inherited), REUSEPORT | same | same |
| submit/wait | adaptive: wait for a batch under load, else short wait | min_complete scaled to load (see main.rs comments) | see server.c |
| wait policy when completions are pending | **enter(0,0) only if fewer than 256 connections are open** (changed in M5) | batched wait (not read in detail) | batched wait (not read in detail) |
| threads | one ring thread | one ring thread | one ring thread plus a checkpoint thread |
| SQLite WAL | `autocheckpoint=32768`, pwrite per frame pair | `autocheckpoint=0`, own checkpoint | own VFS shim: one pwrite per commit |
| idle sweep | one sweep per pass | never closes idle connections | not measured |

What the counters showed: our ring design already matches both leaders item for item. The gap was
the wait policy. With a completion queue entry always pending under load, the loop called a bare
`io_uring_enter(0,0)` on almost every pass, 0.43 enters per request against 0.10 for the leaders.
Each enter under DEFER_TASKRUN runs the deferred task work for a handful of completions only.

### M5 change: let the batching wait run when many connections are open

Before: `if fifo non-empty or any CQE pending: enter(0,0)`. After: a pending CQE short-circuits the
wait only below 256 open connections (`+batch-open+`), so under load the pass takes the batched wait.
Code in `src/server.lisp`, one condition. Tests 42/42, equivalence 2208 compared 0 mismatches, fuzz gate
20,947 cases 0 5xx.

| VM, 200k users, 120 s hold | base 11e396a | M5 |
|---|---|---|
| pair 1, probe us/req | 33.0 | 30.6 |
| pair 1, CPU% | 73.5 | 68.1 |
| pair 2, probe us/req | 33.3 | 30.4 |
| pair 2, CPU% | 74.1 | 67.7 |
| io_uring_enter per request (pair 1) | 0.349 | 0.058 |
| syscalls per request (pair 1) | 0.867 | 0.497 |
| user us per request (pair 1) | 5.6 | 3.9 |

Both pairs lower; about 8% less CPU per request. Throughput identical (21.1k rps).

300 s hold at 250k on the final build: **fails**. 25.5k rps, CPU 93.7% (36.8 us/req), p99 1.71 s,
0 errors besides 24 resets. The ceiling stays at 225k held; the change buys CPU headroom at 200k, not a
new ceiling. Not run at 225k on the new build.

Tried and dropped: nothing else was built. Counters showed the rest of the ring design already matches
the leaders, and the remaining gap to kensac is user time (5.8 vs 3.3 us/req) and the WAL pwrite count
(pavlyhalim writes one frame buffer per commit; we pwrite 0.49 per request).

## Memory (M6)

Measured in the 1 vCPU / 2 GB score-mode VM with `peek/probe.sh`, 60 s ramp + 120 s hold, accounting taken 95 s
into the run (`/proc/PID/smaps_rollup`, `/proc/meminfo`, `slabtop`, sockstat).

Where memory goes at 250k held connections (default build, before M6):

| item | MB | note |
| --- | --- | --- |
| kernel TCP sockets (slab `TCP`, 2.31 KB each) | 601 | 250,016 objects; the dominant cost, outside the process |
| `sock_inode_cache` | 206 | 0.81 KB per socket |
| `filp` + `dentry` | 60 + 50 | one file per socket |
| process RSS | 162 | 140 anon, 22 file-backed (runtime + shim.so text) |
| MemFree / page cache | 125 / 491 | page cache includes the 64 MB SQLite cache and the DB |

So the kernel holds about 920 MB for 250k sockets (about 3.7 KB per connection) against 162 MB for the process
(about 0.65 KB per connection, including SQLite and the Lisp heap). Memory is not what stops 250k: the failing runs
have 680-730 MB available and sit at 97% CPU.

What was tried (all at 250k, 120 s holds, one run each unless noted; the 250k pass/fail result is noisy, so only
the memory columns are conclusive):

| variant | RSS MB | CPU % | pass |
| --- | --- | --- | --- |
| default (64 MB cache) x3 | 163 / 151 / 207 | 96.5 / 96.6 / 97.7 | 0 of 3 |
| 4 MB cache, no mmap | 118 | 95.0 | yes |
| 4 MB cache + 256 MB mmap | 150 | 92.0 | no |
| mlock of text x3 | 139 / 201 / 187 | 84.8 / 97.4 / 84.2 | 2 of 3 |
| 4 MB cache + mlock x2 | 169 / 155 | 97.8 / 97.9 | 0 of 2 |
| fixed 32 KB SO_SNDBUF/SO_RCVBUF | 150 | 97.1 | no |

Not shipped (reverted after verification): `cache_size` 4 MB, `mlock` of the runtime text and `--dynamic-space-size`
256 MB (commit 28cc102). They were picked from single 120 s runs at 250k, which are noisy, and memory is not what
limits the VM, so they bought nothing measurable. The code stays at the 42aa218 settings (64 MB cache, 512 MB
dynamic space, no mlock). Also not shipped: mmap (more RSS, no gain) and socket buffer caps (no measurable effect:
the kernel already auto-sizes down for idle sockets). Per-connection state in the process was already small
(hundreds of bytes), so there was nothing left to shrink there.

Verification (v6), same lock hold in the score-mode VM, lg, 300 s at 247k, 28cc102 then 42aa218: 28cc102 failed
(p95 861 ms, p99 1,051 ms, CPU 88.8%, 39.75 us/req); 42aa218 also failed on the same day (p95 852 ms, p99 1,172 ms,
CPU 88.8%, 39.42 us/req). 28cc102 was 0.8% worse in us/req and did not pass, so its code was reverted. Both runs were
about 18% slower per request than the 33.3 us/req 42aa218 held at 247k earlier, so the VM was slower in this
session; the 247k pass is not reproduced here. `results/runs.csv` in the rig has both rows.

## Raw-syscall VOP and a self-tuning wait policy (M7)

### Phase A: `io_uring_enter` as one `syscall` instruction

`%raw-syscall6` (src/ffi.lisp) is a `define-vop` that loads rax/rdi/rsi/rdx/r10/r8/r9 and emits `syscall`; the
result is rax, so a failure comes back as `-errno` with no errno global and no foreign-call frame. `ring-enter`
(the only hot syscall) uses it; setup/register stay on libc.

| | libc `syscall()` via sb-alien | VOP |
|---|---|---|
| no-op `io_uring_enter(fd,0,0,0)`, 1M calls, local | 42 ns | 37 ns |

That is about 5 ns per enter. At about 0.5 enters per request in the VM it is under 3 ns per request, below what the
rig can resolve. It ships because it is correct (42/42, fuzz, kill -9 durability, flat bytes and gcs=0 at 60k users
with the controller running) and not slower.

Lisp-to-C crossings per request (a `TD_COUNT=1` build counts every `defalien` call; 25,422 requests, local, low load,
so syscalls per request are higher than in the VM): clock_gettime 10.4, memmove 59.0, SQLite 3.1
(`sqlite3_step` 1.6 + column/changes), B-tree shim 54 (`td_payload` 28.6, `td_seek` 19.5, `td_next` 9.2), malloc/free 1.0,
`io_uring_enter` 2.6. Almost all are plain C calls without a kernel entry. The only syscalls on the hot path are
`io_uring_enter` and (rarely) `mmap`.

### Phase B: the wait policy as parameters, loops generated per point, a controller that picks one

Three knobs, five levels each, 125 points (src/tune.lisp): connections open before always waiting for a batch
(`64..1024`), completions that end the wait (`64..1024`), and the wait timeout (`0.25..4 ms`). Point 62 (256/256/1 ms) is
the base policy from M5. `event-loop-body` (src/server.lisp) is a macro: one hot loop with the three numbers as
literals. The 27 points with every knob at levels 1-3 are compiled into the image at build time. A switch is a store
to `**variant-want**`; the running loop notices at the end of a pass and returns to `event-loop`, which funcalls the
other loop: no consing. Points outside the 27 are `COMPILE`d on a helper thread when the climb asks for one, at most one
per 10 s. Measured: the compile takes 12-13 ms, `gc-max=0us` (no GC ran), so nothing came near the 5 ms pause limit.
Live compile therefore stays in. In the 260k hold no outside point was requested (`compiles=0`).

The controller runs on the loop thread once per 3 s. State lives in a preallocated `(simple-array fixnum (48))` and a
`(simple-array fixnum (125))` of "rest until" seconds: no garbage (flat bytes, gcs=0 with the controller active).

**Inputs, all generic load signals, nothing else:** connections open (`open-total`), CQEs per `io_uring_enter`,
requests per second, process CPU time (`getrusage(RUSAGE_SELF)`), and the longest wait of an answer in the queue /
the longest pass in the window. It never reads a request, path, header, token, address, port, or timing pattern.

**Goal:** lowest CPU us per request, guarded by queue wait (a non-base point whose window shows a wait over 250 ms is
dropped to the base policy and rests 120 s). Hill-climb over the six neighbours of the current point, A/B against the
current point's last window, a 4 % hysteresis, a confirming window before a move is believed, windows whose rate moved
more than 5 % discarded. Not busy (under 256 connections or 4000 req/s) means the base policy and no exploring. Three
windows in a row with wrong-looking signals (no CPU advance, no requests, no enters, cost outside 0.5-1000 us, clock
jump) mean base policy and a 30 s pause. Any error in the controller turns it off and leaves the base policy.
`TUNE=off` disables it, `TUNE=fix:b,m,w` pins a point.

```lisp
(defun ctl-tick-1 ()
  (let* (... (rps  (floor (* dreq 1000000000) dt))
         (cost (if (plusp dreq) (floor (* dcpu 1000) dreq) 0))       ; milli-us of CPU per request
         (busy (and (>= open +ctl-min-conns+) (>= rps +ctl-min-rps+)))
         (sane (and (< dt (* 4 +ctl-tick-s+ 1000000000)) (plusp dcpu) (plusp dreq) (plusp denters)
                    (<= 500 cost 1000000)))
         (steady ...))
    (cond ((not busy) (ctl-fall-back +m-idle+))
          ((= mode +m-paused+) (when (>= **mono-s** (cs +c-pause+)) (ctl-fall-back +m-settle-cur+)))
          ((not sane) (when (>= (incf (cs +c-weird+)) 3) (ctl-fall-back +m-paused+) ...))
          ((and (/= **variant-want** +base-point+) (> wmax-us +ctl-guard-wait-us+))
           (setf (aref **ctl-bad** **variant-want**) (+ **mono-s** +ctl-bad-s+))    ; guard: back to base
           (ctl-fall-back +m-settle-cur+))
          (t ... settle -> measure current -> settle -> measure candidate; a candidate cheaper by 4 %
              moves the climb only if a confirming window agrees))))
```

(The full function is `ctl-tick-1` in src/server.lisp.)

### Results (VM, score mode, lg, `RIG_CPU_QUOTA=none`, base 8028056 vs the M7 build; us/req = CPU % x 1e6 / hold req/s)

| 200k, 60 s ramp + 120 s hold | base | M7 |
|---|---|---|
| pair 1 (base, M7) | 32.72 us (69.2 %) | 32.63 us (69.0 %) |
| pair 2 (M7, base) | 32.59 us (68.9 %) | 32.53 us (68.8 %) |
| 20k users: p99 | 2.22 ms | 2.22 ms |

Both pairs are lower, by 0.3 % and 0.2 %. Run-to-run spread on the same build is about 0.5 %, so this is a tie, not a win.
(An earlier build with a 2 s tick, 2 % hysteresis gave 32.79/32.52 and 32.34/32.58; same story.) p99 at 200k varies
97-345 ms between runs of either build.

300 s hold at 260k: **fails** (p95 1402 ms, p99 1763 ms, 97.9 % CPU, 37.8 us/req), the same failure as the base. 270k was not run.

Controller trace, 260k hold (every 3 s; `want` = loop in use, `cur` = point the climb stands on):

```
t=9   measure-cur  want=62 rps=4881  open=19243  cost=58.5us/req cqe/enter=19      ramp: base policy measured
t=18  settle-cand  want=37(128/256/1000us) ...                                      tries batch-open 128
t=120 settle-cand  want=61(256/256/500us)  rps=25127 open=260000 cost=38.6us
t=123 guard->base  want=61  wait-max=...>250ms                                      guard trips, point rests 120 s
t=132 settle-cand  want=63(256/256/2000us)
t=234 guard->base  want=57(256/128/1000us)
t=240 settle-cand  want=67(256/512/1000us)
t=303 converged    want=62  cur=62 rps=24954 open=260000 cost=39.7us/req cqe/enter=1726   ends on the base policy
```

Over the hold: 24 switches, 14 guard trips, 4 "unsteady" discards, 0 signal-wrong events, 0 compiles, `cur` stayed at 62
whenever the load was full. The per-window cost noise (+-10 % at 260k) is larger than any gain a neighbour
showed, so the controller found nothing better than the base policy and says so.

### What shipped, what was dropped

Shipped: the VOP; the 125-point parameterised policy with 27 precompiled loops; live COMPILE for the outer points (12-13 ms,
no GC); the controller. Not shipped as a win: the controller does not beat the base at 200k (tie within 0.5 %) and does not
rescue 260k. The wait policy is flat near the base at this load, which is the honest result.

## The write pulse, and checkpoints off the event loop (M8)

### What was wrong

The scored run held 247,000 users at 79 % CPU with p95 251 ms, while entries with the same CPU per request
(30 us) hold 300,000 at 92-96 %. The loop was not out of CPU: it was freezing.

- The helper thread's connection never read the database, so it never opened the WAL, and
  `sqlite3_wal_checkpoint_v2` on such a connection returns OK without doing anything. `STATS=1` showed
  `ckpts=0` for whole runs. The thread had never checkpointed.
- So the log grew to the `wal_autocheckpoint=32768` limit, and SQLite ran the checkpoint inside the `COMMIT`
  that crossed it, on the event loop: the whole log copied back and two `fdatasync`s, with every connection
  waiting. `strace -f` on the old build: every `fdatasync` is on the loop's thread, the longest 238 ms.

### What changed

1. **Checkpoints run on the helper thread** (`csrc/shim.c`). Automatic checkpoints are off; a WAL hook counts
   frames. Past 4,096 frames the loop asks the thread for a PASSIVE checkpoint. The log only restarts once it
   is copied completely, and a checkpoint cannot catch the frames written while it ran, so the loop then holds
   its write pulse (reads go on, at most 250 ms) while the thread copies the tail. The loop then ends its read
   transaction, the next one starts on a fully copied log, and the next write restarts the log. Past 65,536
   frames the loop checkpoints by itself, as a last resort. The thread reads the schema once at startup, so
   its checkpoints are real, and runs at nice 19 (a `SCHED_IDLE` thread gets nothing while the loop is busy).
   "Fully copied" is only believed when the loop has not committed since the checkpoint was asked for
   (SQLite reports it against the log as it was when the checkpoint began).
2. **The write pulse** (`src/server.lisp`). A connection whose next request is a POST moves to a write queue.
   At most once per 20 ms, and never during a hold, a pass executes the queued writes after its reads, in one
   transaction with one `COMMIT`; the responses are sent after it returns (rule 6). With nothing queued and the
   pulse due, a write is executed in the pass that reads it. Reads never wait for a pulse, and reads of a pass
   are answered before its writes run, so no read sees a row that is not committed yet.
3. **One read transaction between pulses.** A pass that only read no longer does `BEGIN`, opens four cursors,
   closes them and `COMMIT`s: the transaction and the cursors stay open for the next pass. The loop is the only
   writer and a pass that writes commits, so the snapshot is always the latest commit; every request still
   reads its rows from SQLite while it is served (rule 5).
4. Each ring's descriptor table is sized to the hard `RLIMIT_NOFILE` (65,535 on the benchmark box, unchanged)
   instead of failing to start where the limit is lower.

Not changed: the wait policy and its controller, the B-tree readers, the HTTP/JWT/JSON code. The controller's
queue-wait signal now covers the read queue only (a write waits for its pulse by design).

### Measured (2026-11-20)

**Where:** a 2-vCPU cloud VM (kernel 6.18), loopback, the server pinned to one CPU and the load generator
(`tools/loadgen.c`, the k6 user loop with shorter think times, 15 % likes, 2 % posts) to the other. SBCL 2.6.10
and SQLite 3.54.1 built from the projects' release tags. **Not** the score-mode VM, not 200k+ connections:
the ceiling on the benchmark box is not measured by any of this. "Old" is the previous commit plus item 4.

Traced runs (`strace -f -e fdatasync`), 2,000 connections, about 20,400 req/s and 1,600 writes/s, 40 s:

| | old | M8 |
| --- | --- | --- |
| `fdatasync` calls on the loop thread, longest | 11, 238 ms | 21, 0.8 ms (the WAL header sync of a log restart) |
| `fdatasync` calls on the helper thread | 0 | 61 |
| checkpoints by the helper / by the loop | 0 / every 32,768 frames | 36 / 0 |
| worst read latency in a second | 521, 288, 230 ms (one freeze per ~11 s) | 51 ms |
| reads p99.9 / max | 507 / 521 ms | 27 / 51 ms |
| WAL file at the end | 135 MB | 20 MB |

Untraced, alternating runs, 30 s holds, same load (us/req = process CPU, both threads, per request):

| pair | us/req old | us/req M8 | reads p99.9 old / M8 | reads max old / M8 | writes p50 / p99 / max, old | writes p50 / p99 / max, M8 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 12.03 | 11.55 | 144 / 7.1 ms | 157 / 21 ms | 1.4 / 3.5 / 156 ms | 12.0 / 22.5 / 33 ms |
| 2 | 12.53 | 11.01 | 156 / 4.4 ms | 177 / 7 ms | 1.4 / 3.4 / 177 ms | 12.0 / 22.3 / 28 ms |
| 3 | 12.25 | 11.61 | 131 / 5.7 ms | 151 / 43 ms | 1.4 / 2.9 / 151 ms | 12.0 / 22.4 / 45 ms |

All three pairs are lower on CPU (4 %, 12 %, 5 %). The CPU gain is small; the change is the tail. A write now
waits about 12 ms for its pulse (median), which is the price.

One pair at 6,000 connections, about 76,000 req/s (the server at 76 % of its CPU): old p99 143.5 ms with a
~150 ms freeze every 3-4 s; M8 reads p99 18.6 / p99.9 35.6 / max 39.5 ms, writes p99 43.5 / max 72 ms,
46 holds (longest 44 ms), no loop checkpoint; 10.14 -> 9.82 us/req.

`strace -c -f`, 15 s: `pwrite64` 0.332 -> 0.240 per request (the M8 figure includes the checkpoint thread's
database writes; the old build did none in that window), `io_uring_enter` 0.027 -> 0.035.

The checks (tests, equivalence, fuzz, kill -9, accounting) are listed once, for the final code, at the end
of the M9 section.

At the server's limit (9,000 connections, the closed loop holds the load at what the server can answer),
order old, new, new, old, 30 s holds, the new build with `WALBUF=1 INLINE=1`:

| | req/s | us/req | reads p50 / p95 / p99 |
| --- | --- | --- | --- |
| old, run 1 / run 2 | 70,012 / 70,004 | 11.18 / 11.73 | 47.5 / 90.7 / 212.8 and 50.1 / 86.6 / 228.5 ms |
| new, run 1 / run 2 | 85,296 / 88,443 | 9.23 / 9.44 | 28.5 / 55.1 / 72.7 and 22.4 / 53.8 / 71.7 ms |

The same test a few minutes later with the switches off (the default build; not interleaved with the old one):
86,211 and 84,019 req/s, 9.60 and 9.47 us/req, reads p99 71.6 and 73.1 ms. So the gain at the limit, about a
fifth more requests with a third of the p99, comes from the pulse and the checkpoint thread, not from M9.

### Not done

- Not run on the score-mode VM or at 200k+ users. Whether the ceiling moves, and by how much, needs that run.
- The test rig's CPU figures move about 10 % between identical runs, so effects under a few percent cannot
  be seen on it. The rig that made the earlier sections resolves 0.5 %.

## Four switches, built and left off (M9)

Each was built, passes every check below with the switch on, and showed no gain that the test rig could
measure. By this README's own bar (both pairs lower, or it does not ship as a win) they are off by default.
They are worth one A/B each on the rig, which resolves much smaller differences.

| switch | what it does | measured here (same rig as M8) |
| --- | --- | --- |
| `WALBUF=1` | One write per commit: a VFS over `unix-excl` (public VFS interface) buffers a commit's WAL frames and writes them when the commit frame's page arrives, inside SQLite's own `xWrite`, so the commit is in the kernel before it is published and before `COMMIT` returns. Any other write, and every read, sync, truncate and close of the WAL, writes the buffer first | `pwrite64` 0.240 -> 0.027 per request. CPU: six order-balanced pairs at 21k req/s, mean 14.37 us/req off, 14.68 on, single runs from 12.8 to 17.0: no difference visible |
| `INLINE=1` | A chunk that is exactly one complete read request, on an idle connection with nothing older queued anywhere, is answered from the kernel's receive buffer: no malloc, copy, FIFO entry or free. Everything else (POSTs, split or pipelined requests, a backlog) takes the old path, so order is unchanged | 92 % of requests take it. CPU: three rounds, 13.13 us/req off, 13.65 on: within the noise |
| `PULSE_MAX_MS=n` | The breathing pulse: while reads are still queued after a pass's share, the write pulse stretches, until the oldest queued write has waited n ms | The CPU curve over a fixed pulse is nearly flat past 20 ms (0 ms 14.5 and 14.5, 5 ms 14.2, 10 ms 12.8, 20 ms 13.7 and 14.0, 50 ms 13.7, 100 ms 13.3, 200 ms 12.9 us/req, with write p50 rising to 112 ms), so there is little for a stretch to win. In the freeze tests it triggered 0 to 3 times a run and changed nothing |
| `QUEUE=newest` | Past one pass of read backlog, three of four answers go to the newest request and one to the oldest | Freeze tests (the whole server stopped for 200 or 600 ms every 5 s at 63-70k req/s): reads p99 207 vs 206 ms and 610 vs 609 ms. The backlog drains in about five passes whatever the order, so the order does not matter |

At the server's limit (the table above, same settings, two runs each): no switch 86,211 / 84,019 req/s,
`WALBUF` 87,045 / 83,054, `INLINE` 95,742 / 87,530, both 84,105 / 80,052. All inside the run-to-run spread.

What was not built: the M7 controller was not pointed at the pulse. The pulse width is one variable read at
run time, so changing it needs no recompiled loop, and the flat curve above says a controller would have
nothing to find.

**Checks on the final code, each with the four switches off and on:** `test/run.sh` 42/42 and 42/42;
`tools/equiv.sh` 2,208 compared, 0 mismatches, twice; `tools/fuzz.sh` 20,947 cases, 0 5xx, the status counts
above, same pid, 42/42 afterwards, twice; `tools/pulse.sh` (27 edge checks and exact accounting under five
settings) OK twice; `tools/kill9.py` four runs (690, 465, 841 and 465 acknowledged posts, two with the
switches on, two with `CKPT_FRAMES=64`), 0 missing, `integrity_check` ok. 0 GCs in every run of this
session (283 stats lines). The build log has the same 244 notes and 6 style warnings as before M8.

Ideas from other entries, no code copied: the checkpoint thread with a short write hold so the log can restart
is [rust-iouring-kensac](../rust-iouring-kensac)'s policy; committing writes on a window while a read
transaction carries over between passes is [fsharp-uring-delneg](../fsharp-uring-delneg)'s and
[rust-iouring-rkkautsar](../rust-iouring-rkkautsar)'s; one write per commit through a VFS is
[c-iouring-pavlyhalim](../c-iouring-pavlyhalim)'s and delneg's; answering from the receive buffer is how
kensac and delneg read; newest-first under backlog is delneg's.
