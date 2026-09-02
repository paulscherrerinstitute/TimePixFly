# TimePixFly (`tpx3app`) — Complete Risk Audit

**Audit date:** 2026-07-29 · **Subject:** `TimePixFly` @ `92f60c0` (branch `dev`) ·
**Scope:** all 37 C++ source/header files (9 563 lines), build scripts, container definitions,
systemd units, replay test server; plus the REST/WS/JSON contract against its Python consumer
(`superxas_bec/devices/timepix/timepix_fly_client/`).

**Method.** Six independent auditors each read a disjoint file set in full and reported findings
with file, line and *verbatim* code quotes. Every finding was then handed to a separate adversarial
verifier instructed to default to *refuted*, which re-read the cited code, checked each quote
character-by-character and searched for counter-evidence (a lock taken elsewhere, an upstream guard,
an invariant that prevents the bug). A completeness critic then swept for uncovered areas. Findings
below survived that process. The most severe claims were additionally re-verified by hand, and the
top one was **reproduced by compiling and executing the code** (see TPX-058).

| | |
|---|---|
| Findings (verified) | **101** — 4 critical, 31 high, 41 medium, 22 low, 3 info |
| Verification outcome | 89 confirmed as written, 12 adjusted (line/severity corrected), **0 refuted** |
| Proof density | 259 code citations, avg 2.6 per finding, all quotes verbatim |
| Independent corroboration | 5 defects found separately by 2–3 auditors (see *Cross-corroboration*) |

---

## 1. Executive summary

`tpx3app` is a capable, performance-focused DAQ backend — lock-free ring buffers, AVX2 decode paths,
CPU pinning, jemalloc — written by essentially one author (385 of 395 commits). The performance
architecture is sound. The **risk is concentrated in everything around the hot path**: the control
plane, error handling, lifecycle transitions and the output stage. Those areas were evidently
written to work on the happy path and have not been hardened against aborts, restarts, second
clients or dead consumers — which is precisely the operating regime of a beamline running
back-to-back scans.

Three structural themes explain most of the 101 findings:

**(a) Single-slot shared state with no ownership discipline.** The WebSocket state channel is one
`static unique_ptr` shared by all handler threads, accessed without its mutex. This is the root of
8 separate findings across 3 auditors, up to and including use-after-free. The same pattern —
one global slot, last writer wins — recurs in the Redis publisher cache and the raw-data listener.

**(b) Errors are dropped rather than propagated.** `TcpWriter` never checks stream state, so a dead
consumer silently discards *every* frame while the scan reports success. `EndFrame` write failures
are swallowed. The `except` state is overwritten by `config` within microseconds, so a REST poller
cannot observe that a scan failed. There is no `/metrics` or `/health` endpoint. **A scan can lose
100 % of its data with every observable signal reporting success.**

**(c) One error poisons all later scans.** Several latched signals and caches are never reset:
`stop_sig` (kills the WS stream permanently after any `?restart`), `writer_finished` (desyncs the
writer handshake forever), the Redis client cache (`reconnect()` without `disconnect()`),
`stop_handlers` (accumulates dangling references to destroyed stack locals across restarts). The
failure mode is *silent degradation requiring a process restart*, which matches the incidents
already observed in integration testing.

**The single most serious defect is a silent data-corruption bug in the default build** (TPX-058):
TOA timestamps are computed in 32-bit arithmetic due to a bit-field promotion, producing garbage for
~87 % of the detector's 26.8 s clock cycle. It is invisible — no crash, no error, plausible-looking
histograms.

### Why these survived until now

The audit found a structural reason, not an individual one. There is **no automated test of the
output path**: all six Julia verification scripts in `test_data/` validate the *raw input* stream;
not one parses `StartFrame`/`XesData`/`EndFrame`. `src/test.cpp` is not wired into any CI. The five
worst data-integrity defects all live in exactly the untested region. Combined with the absence of
any metrics surface, there is no mechanism — automated or operational — by which silent output
corruption would be noticed.

---

## 2. Top risks, ranked by expected harm

Ranked by *probability × consequence for beamline data*, not by raw severity label.

| # | Risk | Finding | Why it ranks here |
|---|---|---|---|
| 1 | **Silent TOA corruption in default build** | TPX-058 | Reproduced by compilation. Affects both container images and any build without `USE_AVX`. Corrupts data with no error signal. |
| 2 | **Silent total data loss on a dead consumer** | TPX-041 | `TcpWriter` never checks stream state; scan reports success with zero frames delivered. |
| 3 | **WebSocket single-slot UAF / stream theft** | TPX-021, TPX-070 | Already caused real scan failures in testing; the crash variant (UAF) has not yet been hit but is live. |
| 4 | **Frames delivered out of period order / relabelled** | TPX-039, TPX-040 | Data-integrity bug in the writer queue; wrong physics silently. |
| 5 | **One aborted scan poisons the session** | TPX-038, TPX-071, TPX-043 | Latched signals never reset ⇒ requires process restart; matches observed incidents. |
| 6 | **Control plane cannot stop a running collection** | TPX-005, TPX-075, TPX-071 | `SIGTERM`/REST stop hang; operator must `SIGKILL`, leaving a stale pid file. |
| 7 | **Unauthenticated control on host network** | TPX-030, TPX-094 | `--network=host` + no auth ⇒ any host process can `?kill=true` mid-scan. |
| 8 | **Malformed pixel map ⇒ heap corruption / OOM** | TPX-045, TPX-061 | Unbounded `energy_point`, `npoints += 1` wraps; reachable via REST. |

---

## 3. Cross-corroboration

Five defects were discovered independently by two or three auditors working from different file sets
and different lenses. Independent convergence is meaningful evidence that a finding is real and that
the failure is reachable from more than one direction:

| Defect | Found by | Max severity |
|---|---|---|
| WebSocket single static slot (theft, UAF, cross-thread destroy, restart latch) | concurrency, network, lifecycle — **8 findings** | critical |
| `await_connection` poll loop ignores stop/restart | concurrency, network, lifecycle | high |
| `stop_handlers` dangling references across restarts | concurrency, network, lifecycle | high |
| `priority_queue<Data*>` ordered by pointer address | concurrency, xes-output | high |
| `set_state()` unguarded WS send can abort the server | network, lifecycle | high |

---

## 4. Independently reproduced: the TOA arithmetic defect

This is presented separately because it is the one finding proven by *execution*, not by reading.

`src/include/decoder.h:208-213` computes the TOA clock; `spidr` is a 16-bit bit-field, which the C++
integral-promotion rules convert to **`int`**, so the shift is evaluated in 32-bit signed arithmetic:

```cpp
inline static u64 getToaClock(TOA event) noexcept
{
    // ftoa is on a 640 MHz clock
    // toa is on a 40 MHz clock
    return ((event.spidr << 18) + (event.ToA << 4)) - event.FToA;
}
```

Compiling the struct and expression verbatim, with the project's own production flags
(`-O3 -ffast-math -DNDEBUG`):

```
decltype(e.spidr << 18) is int? YES -> 32-bit arithmetic (sizeof=4)
spidr=40000: buggy=1895827008  correct=10485761600  *** MISMATCH ***
  spidr= 8191 (t= 3.355s into 26.84s cycle): buggy=          2147221504 correct=          2147221504 ok
  spidr= 8192 (t= 3.355s into 26.84s cycle): buggy=18446744071562067968 correct=          2147483648 BROKEN
  spidr=20000 (t= 8.192s into 26.84s cycle): buggy=           947912704 correct=          5242880000 BROKEN
  spidr=65535 (t=26.843s into 26.84s cycle): buggy=18446744073709289472 correct=         17179607040 BROKEN
```

Timestamps are correct only while `spidr < 8192`, i.e. for the **first 3.36 s of every 26.84 s SPIDR
cycle — 12.5 % of the time**. For the remaining 87.5 % the value overflows `int`, is sign-extended
into `u64`, and yields garbage.

**That the scalar path is the defect (not the intent) is proven by the AVX path**, which computes the
same quantity correctly in 64 bits — `src/include/avx2_decoder.h:53`:

```cpp
const auto spidr = _mm256_slli_epi64(_mm256_and_si256(events, spidr_mask), 18);
```

`_mm256_slli_epi64` is a 64-bit shift. The two decode paths therefore **disagree**, and the author's
intent is unambiguous.

**Which builds are affected.** `compile.sh:32-38` leaves `AVX_FLAGS` empty unless `USE_AVX` is set,
so the scalar path is the **default**; both container images build with `GENERIC=1 ./compile.sh`
(`container/tpx3app-container.docker:19`) and therefore ship the defect. A production binary is
affected unless it was explicitly built with `USE_AVX=1`.

**Fix** (one line):
```cpp
return ((u64{event.spidr} << 18) + (u64{event.ToA} << 4)) - u64{event.FToA};
```
Then audit sibling bit-field expressions for the same promotion trap, and add
`-fsanitize=undefined` to a CI build — UBSan flags this immediately as
*"left shift of 40000 by 18 places cannot be represented in type int"*.

> Note this compounds with TPX-059: even once the arithmetic is fixed, the TOA clock **wraps** at
> 2³⁴ ticks (26.8 s) and the TDC clock at 2³⁶ (107.4 s), and no wraparound handling exists. Any
> acquisition longer than the wrap period, or straddling a wrap, produces underflowed relative times.

---

## 5. Remediation plan

Sequenced by risk-reduction per unit of effort. Items in Phase 0 are small, local, and each removes
a live data-integrity or crash risk.

### Phase 0 — before the next beam time (hours)

1. **TPX-058** — 64-bit cast in `getToaClock`. One line. Highest value in the audit.
2. **TPX-039** — `std::priority_queue<Data*>` → supply a comparator that dereferences and compares
   `period` (the `std::less<xes::Data>` specialization at `xes_data.h:123` is currently dead code).
3. **TPX-041** — check stream state after every `TcpWriter` send; on failure set the error state and
   surface it in `EndFrame.error` instead of discarding frames silently.
4. **TPX-044** — fix the `&&`/`||` inversion in `SetTimeROI` validation.
5. **TPX-001** — initialize `bool ss = false;` in the four `thread_signal.h` wait functions.
6. Rebuild containers and pin the base image; add `USE_AVX` explicitly to the production build recipe
   so the decode path is a deliberate choice rather than a default.

### Phase 1 — structural fixes (days)

7. **WebSocket ownership (8 findings).** Replace the static slot with a mutex-protected registry of
   `shared_ptr<WebSocket>`; `set_state()` broadcasts to all clients and drops dead ones inside a
   try/catch. This closes the UAF, the stream theft that already broke scans, the "failed GET /ws
   kills the active client" path, and the `stop_sig` restart latch in one change.
8. **Error propagation.** Make `except` sticky until explicitly cleared (or add a monotonically
   increasing `error_seq` to `/last-error`), so a REST poller cannot miss a failed scan. Note the
   Python client already had to add a REST poll fallback for exactly this class of problem.
9. **Reset all latched state on scan start** — `writer_finished`, `start_writer`, `stop_sig`,
   `stop_handlers`, the Redis publisher cache. Add `disconnect()` before `connect()` in
   `RedisPublisher::reconnect()`.
10. **Make stop actually stop.** Give the `await_connection` poll loop and the reader receive-timeout
    path an exit on `gvars.stop`/`restart`, and install proper `SIGTERM`/`SIGINT` handling.
11. **Bound all externally supplied values** — `energy_point`, `npoints`, chip/pixel indices — at the
    REST and file parsers, with a clear error rather than an OOB write.

### Phase 2 — make the class of defect impossible (weeks)

12. **An output-side test oracle.** The decisive structural gap: add a harness that runs a known raw
    stream through `tpx3app` and asserts on the emitted JSON (frame ordering, period monotonicity,
    conservation of counts, `EndFrame.error`). Every Phase-0 data-integrity bug would have been
    caught by this and none is currently detectable.
13. **CI**: build both decode paths, run `src/test.cpp` (currently never executed automatically),
    add UBSan/ASan/TSan jobs. TSan will find the `thread_signal` races directly.
14. **Observability**: a `/metrics` endpoint exposing frames written, bytes sent, dropped events,
    writer errors and queue depths — today none of this leaves the process except as log lines.
15. **Protocol versioning**: the JSON frames carry no version field and the pixel-map format has no
    schema id. The known "`except` missing from the Python literal" incident is this failure class;
    without a version there will be another.
16. **Deployment hygiene**: pin container builds to a commit (they currently `git clone` branch HEAD,
    so images are unreproducible), tag releases, move the binary out of `/home/asi/git`, drop
    `--loglevel=debug` from the production unit, add `After=` alongside `BindsTo=` in the systemd
    unit, and add basic sandboxing directives.

---

## 6. Known gaps in this audit

Stated explicitly so the coverage claim is honest:

- **`timepix_fly_backend.py` (the Python BEC device) was not audited.** A spot-check found a
  client-side parallel of the C++ listener issue: it binds `0.0.0.0`, accepts the first peer as the
  data stream, and handles one connection at a time. It deserves its own pass.
- **Julia tooling** (`generate_data/`, `test_data/`, 1 275 lines) was inventoried but not
  line-audited; `generate_map.jl` is the de-facto specification of the pixel-map format.
- **Runtime verification is limited.** Findings are proven from source (plus one compiled repro and
  the live incidents already observed). No fuzzing, sanitizer run, or load test was performed
  against a running `tpx3app` — several concurrency findings are codegen- and timing-dependent by
  nature, and a TSan run would raise confidence further.
- **`container/server_data/` fixtures** (150 MB LFS raw file, `layout-8.json`) were not validated.
- The audit reads the local `dev` checkout; if the deployed beamline binary differs, line numbers
  and applicability must be re-checked against that revision.

---

## 7. All findings

Each finding: claim, verbatim code proof (file:line), impact, and fix. IDs are stable within this
document. `Verification` records the adversarial verifier's verdict.

## Concurrency & data pipeline

*20 findings: 8 high, 6 medium, 6 low*

### TPX-001 · [HIGH] Shutdown-interruptible waits return uninitialized bool when signal already set

**Claim.** single<with_shutdown>::wait_reset/wait and multi<reset_with_shutdown>::wait_reset/wait declare 'bool ss;' and only assign it inside the loop condition's second operand; if the start signal is already set on entry, '!signal' short-circuits and 'ss' is returned uninitialized (UB). A garbage nonzero value makes the reader/analyser/writer thread treat it as shutdown and exit its service loop permanently.

**Proof.**

`src/include/thread_signal.h:175-182`
```cpp
std::unique_lock lck{lock};
bool ss;
{
    while (!signal && !(ss = shutdown.signal))
        cond.wait(lck);
}
signal = false;
return ss;
```
`src/include/thread_signal.h:320-326`
```cpp
auto mask = 1u << thread;
bool ss;
std::unique_lock lck{lock};
while (! (signalbits & mask) && !(ss = shutdown.signal))
    cond.wait(lck);
signalbits ^= mask;
return ss;
```
`src/include/data_handler.h:244, 327`
```cpp
if (start_reader.wait_reset())
    break;
...
if (start_analysis.wait_reset(threadId))
    break;
```
`src/include/xes_data_manager.h:150-151`
```cpp
if (start_writer.wait_reset())
    break;
```

**Impact.** The signal-already-set path is real: in server mode a thread finishing scan N logs (possibly to syslog) before looping back to wait_reset, while main can send the next start first; then the loop body never runs and ss is stack garbage. If it reads nonzero, that thread silently shuts down; the next scan then hangs forever because reader_finished/analysis_finished (multi<send>::wait_reset at thread_signal.h:250-252 has no timeout or shutdown escape) is never signaled - a permanent await() hang requiring process kill. Behavior is compiler/codegen dependent, so it can be 'works in test, hangs at beamline'.

**Fix.** Initialize 'bool ss = false;' in all four functions, and preferably compute it explicitly after the loop: 'while (!signal && !shutdown.signal) cond.wait(lck); ss = shutdown.signal;'. Additionally give multi<send>::wait_reset a shutdown escape or timeout so one dead thread cannot hang await() undiagnosed.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim (thread_signal.h:175-182, 320-326; data_handler.h:244-245, 327-328; xes_data_manager.h:150-151). 'bool ss;' is only assigned in the second operand of '&&'; when signal (or the thread's signalbit) is already set on entry, '!signal' short-circuits, the loop body never runs, and 'return ss;' reads an indeterminate value (UB). Same flaw in all four functions: single<true>::wait_reset/wait and multi<reset_with_shutdown>::wait_reset/wait. The already-set path is reachable: workers send *_finished (data_handler.h:295, 459) and then log before looping back to wait_reset, so a fast </sub>

### TPX-002 · [HIGH] Lost wakeup: single<no_shutdown>::send() sets flag and notifies without holding the waiter's mutex

**Claim.** single<false>::send() writes the atomic flag and calls notify_all() without acquiring the condition-variable mutex, while waiters use an untimed cond.wait(). If send() runs between a waiter's predicate check (under the mutex) and its block in wait(), the notification is lost and the waiter sleeps forever. This signal class implements reader_finished, writer_finished, and the all_shutdown/writer_shutdown signals (whose dependents' condvars are also notified without their mutexes).

**Proof.**

`src/include/thread_signal.h:90-95`
```cpp
inline void send() noexcept
{
    signal = true;
    for (auto* sig : dep)
        sig->notify_all();
}
```
`src/include/thread_signal.h:102-108`
```cpp
inline void wait_reset() noexcept
{
    std::unique_lock lck{lock};
    while (!signal)
        cond.wait(lck);
    signal = false;
}
```
`src/include/data_handler.h:543-544`
```cpp
iobuf::resetter reset(databuf);
reader_finished.wait_reset();
```

**Impact.** DataHandler::await() can hang forever on reader_finished (untimed wait, no timeout fallback) when the reader finishes at nearly the same moment main enters the wait - most likely on short measurements or REST-aborted collections. Same for xes::Manager::await() on writer_finished. For all_shutdown.send() in DataHandler::shutdown()/~Manager(), a lost wakeup means join() never returns and the process hangs at shutdown/restart.

**Fix.** In single<false>::send(), lock the mutex of every signal being notified before setting the flag and notifying (for dep signals, take each dependent's base::lock around the notify; the shutdown flag read in dependents' predicates must be published under their lock). Alternatively, change all untimed cond.wait() calls in this header to wait_for with a re-check loop.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (thread_signal.h:90-95, 102-108; data_handler.h:543-544). single<false>::send() stores the atomic flag and calls notify_all() on itself and dependents without acquiring any of their mutexes, while waiters use untimed cond.wait(). Classic lost wakeup: waiter checks !signal under its lock, notifier sets flag+notifies before the waiter blocks in wait(), notification lost, waiter sleeps until a (non-guaranteed) spurious wakeup. Verified single<no_shutdown> implements reader_finished/all_shutdown (data_handler.h:179,181) and writer_shutdown/writer_finished (xes_data_manager.h:118-11</sub>

### TPX-003 · [HIGH] DataHandler::await() is not exception-safe: socket shutdown throw resets the ring under live analysers and desyncs the whole pipeline

**Claim.** await() constructs iobuf::resetter first, then calls dataStream.shutdown() between the two waits. Poco StreamSocket::shutdown() throws NetException when ::shutdown(2) fails (assumption: e.g. ENOTCONN after the connection died with RST - the realistic SERVAL-abort case). The throw unwinds await(): (a) resetter runs databuf.reset() while analyser threads are still reading jars (data race on next/level/done); (b) analysis_finished.wait_reset() is skipped so the signal stays set and the next cycle's await() returns immediately while analysers still run; (c) main.cpp skips analysis.await(), so modules whose analyser aborted without a final purge never get 'final' set and the writer polls forever, making analysisPtr->shutdown() hang on writerThread.join().

**Proof.**

`src/include/data_handler.h:541-548`
```cpp
inline void await()
{
    iobuf::resetter reset(databuf);
    reader_finished.wait_reset();
    dataStream.shutdown();
    dataStream.close();
    analysis_finished.wait_reset();
}
```
`src/main.cpp:775-779`
```cpp
dataHandler.rawDataStream(dataStream);
analysis.run_async();
dataHandler.run_async();
dataHandler.await();
analysis.await();
```
`src/include/io_buf.h:457-460`
```cpp
inline ~resetter() noexcept
{
    bufs.reset();
}
```
`src/include/xes_data_manager.h:434-436`
```cpp
writer_shutdown.send();
writerThread.join();
```

**Impact.** One thrown Poco exception in await() (abrupt SERVAL disconnect/RST - the same class of event as known incident 2) corrupts the buffer collection under active consumers (crash or 'expected header has no TPX3 id' cascades), leaves signal protocols desynchronized across server-mode cycles, and can hang process shutdown on writerThread.join().

**Fix.** Make await() noexcept in effect: wrap dataStream.shutdown()/close() in try/catch(...) (they are best-effort), always execute both wait_reset() calls, and only reset databuf after analysis_finished. Mirror this in main.cpp by ensuring analysis.await() runs even if dataHandler.await() fails (scope guard).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (data_handler.h:541-548; main.cpp:775-779; io_buf.h:457-460; xes_data_manager.h:434-435, cited range 434-436 covers it). Poco SocketImpl::shutdown() does throw on ::shutdown(2) failure (ENOTCONN after RST is a real Linux behavior). On throw: (a) resetter dtor runs databuf.reset() while analysers still call read_reservation/await_data on the same jars (race on next/done/level, io_buf.h:241-255); (b) analysis_finished (multi<send>) stays set once analysers send, so the next cycle's await() returns from it immediately; (c) main.cpp:786 catches and continues the loop, skipping anal</sub>

### TPX-004 · [HIGH] Reader receive-timeout path checks only stop_collect, ignoring stop/restart and the buffer stop flag

**Claim.** readData()'s TimeoutException handler only tests global::instance->stop_collect. REST /?stop and /?restart set gvars.stop and call stopNow() (databuf stop_flag), and analyser exceptions call stopNow() - none of which this loop observes. With an open-but-idle TCP connection, the reader re-arms 300 ms receives forever, so reader_finished is never sent and await() blocks indefinitely.

**Proof.**

`src/include/data_handler.h:213-218`
```cpp
} catch (Poco::TimeoutException&) {
    if (global::instance->stop_collect) {
        stopNow();
        return 0;
    }
}
```
`src/include/rest_callbacks.h:322-325`
```cpp
auto& gvars = *global::instance;
gvars.stop.store(true);
for (const auto& handler : gvars.stop_handlers)
    handler();
```
`src/include/data_handler.h:513-516`
```cpp
inline void stopNow() noexcept
{
    databuf.stop_now();
}
```

**Impact.** Aborting a stalled acquisition via REST /?stop or /?restart (a normal operator action when a scan hangs) leaves the reader spinning on timeouts; main hangs in await() on reader_finished; the app never reaches shutdown until SERVAL sends data or closes the socket. Same hang if an analyser throws while the stream is quiet.

**Fix.** In the timeout branch, also return 0 when gvars.stop, gvars.restart, or the collection's stop flag is set (add a 'bool stopped() const { return stop_flag.load(); }' accessor to iobuf::collection_t and check it).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (data_handler.h:213-218, 513-516; rest_callbacks.h:322-325). Checked the counter-evidence: write_reservation does test stop_flag (io_buf.h:370), but that check is only reached when readData(buf,size) returns, and on an idle-but-open socket the inner loop re-arms 300 ms receives forever since only stop_collect breaks the TimeoutException branch. /?stop (rest_callbacks.h:320-329) and /?restart (337-347) set gvars.stop and fire stop_handlers (-> databuf.stop_now()), neither of which this loop observes; analyser-exception stopNow() likewise. Main then hangs in await() on reader_fin</sub>

### TPX-005 · [HIGH] await_connection poll loop cannot be exited by REST /?stop or /?restart

**Claim.** The accept-poll loop in Tpx3App::main() breaks on timeout only when gvars.stop_collect is set. gvars.stop / gvars.restart are never checked, so a REST stop/restart issued while waiting for the ASI server to connect loops forever. Additionally, a signal (SIGTERM) interrupting poll yields EINTR, which is converted into a RuntimeException and a spurious 'except' state instead of a clean exit.

**Proof.**

`src/main.cpp:738-749`
```cpp
do {
    ret = poll(fds, 1, timeout);

    if (ret == -1) {
        throw Poco::RuntimeException(std::string{"poll failed - "} + std::strerror(errno));
    } else if (ret == 0) {  // timeout
        if (gvars.stop_collect)
            break;
    } else if (fds[0].revents & POLLIN) {
        break;
    }
} while (true);
```

**Impact.** If SERVAL never connects (misconfigured raw destination, dead server - a routine failure at the beamline), the BEC client cannot stop or restart tpx3app via REST; the state machine sticks in await_connection indefinitely. SIGTERM works only by accident via an exception that records a bogus error and pushes 'except' state to the client.

**Fix.** Change the timeout branch to 'if (gvars.stop_collect || gvars.stop) break;' and handle 'ret == -1 && errno == EINTR' as continue (re-check flags) instead of throwing.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (main.cpp:738-749). The loop breaks only on POLLIN or (timeout && stop_collect); gvars.stop/gvars.restart are never consulted, so REST /?stop//?restart during await_connection cannot exit it (the stop_handlers they fire only set the iobuf stop_flag, irrelevant before a connection exists). Verified sigint_handler (main.cpp:64-69) sets stop/restart, so SIGTERM only escapes via poll's EINTR -> ret==-1 -> RuntimeException 'poll failed - Interrupted system call' -> catch at 786 sets a bogus error and 'except' state; per signal(7), poll is never auto-restarted, and if the signal is de</sub>

### TPX-006 · [HIGH] stop_handlers capture stack locals by reference and accumulate across SIGUSR1 restarts (use-after-free)

**Claim.** Tpx3App::main() registers stop handlers into the process-global gvars.stop_handlers vector capturing the local unique_ptrs by reference. The vector is never cleared, and main() runs again on every restart (SIGUSR1 / REST /?restart loop in ::main). After a restart, the first handler in the vector still references the destroyed previous stack frame; any /?stop, /?restart, or /?stop_collect REST call iterates all handlers and dereferences the dangling pointer.

**Proof.**

`src/main.cpp:674-676`
```cpp
gvars.stop_handlers.emplace_back([&dataHandlerPtr]() {
    dataHandlerPtr->stopNow();
});
```
`src/main.cpp:929-938`
```cpp
do {
    global::instance->stop.store(false);
    global::instance->restart.store(false);
    retval = app.run();
    if (global::instance->restart.load()) {
```
`src/include/rest_callbacks.h:358-361`
```cpp
gvars.stop_collect.store(true);
for (const auto& handler : gvars.stop_handlers)
    handler();
return "OK";
```

**Impact.** After one 'tpx3app restart' (a supported operator command), the very next scan abort via REST crashes the DAQ process (UAF on a destroyed unique_ptr and its DataHandler), killing all subsequent scans until manual restart - the same operational signature as the known replay-server incident.

**Fix.** Clear gvars.stop_handlers at the start of Tpx3App::main() before re-registering, or register once against a stable owner (e.g. store a std::function that reads an atomic pointer set/nulled around the handler lifetime under a mutex).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp:674-676, 929-933 within cited 929-938; rest_callbacks.h:358-361). Grep over src/ confirms stop_handlers is only ever emplace_back'd (main.cpp:665, 674) and iterated (rest_callbacks.h:324, 342, 359) — never cleared or erased; the vector lives in the process-global singleton (global.h:47). The restart loop (main.cpp:929-938) re-invokes app.run() -> Tpx3App::main(), whose locals copyPtr/dataHandlerPtr (659-661) are captured by reference; after the first restart the old handler dangles and every /?stop, /?restart, /?stop_collect iterates all handlers including the stale o</sub>

### TPX-007 · [HIGH] Writer 'ready' queue is std::priority_queue<Data*> ordered by pointer address, not period

**Claim.** ModuleData::ready is a priority_queue of raw pointers, so it uses std::less<Data*> (address comparison). The std::less<xes::Data> specialization comparing periods (proving the intent) does not apply to Data*. When a module has two or more queued histograms, ready.top() pops an arbitrary period; the writer then aggregates histograms of different periods into one output frame and takes the max period as the frame period.

**Proof.**

`src/include/xes_data_manager.h:61`
```cpp
std::priority_queue<Data*> ready;   //!< Histograms ready for writer thread
```
`src/include/xes_data.h:123-124, 131-134`
```cpp
template<>
struct std::less<xes::Data> final {
    inline bool operator()(const xes::Data& lhs, const xes::Data& rhs) const noexcept
    {
        return lhs.period < rhs.period;
    }
```
`src/include/xes_data_manager.h:199-203`
```cpp
if (data != nullptr) {
    if (d->period > data->period)
        data->period = d->period;
    assert(data->period != 0);
    data->addResetRhs(*d);
```

**Impact.** Whenever the writer lags behind save_interval (slow tcp/redis output, network hiccup), XES frames silently mix events from different save periods and frames are emitted out of period order - undetectable data corruption in the time axis of the scan.

**Fix.** Use std::priority_queue<Data*, std::vector<Data*>, decltype(cmp)> with cmp = [](Data* a, Data* b){ return a->period > b->period; } (min-heap on period), and in the aggregation loop only merge histograms whose period equals the current frame's period (peek before pop).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_manager.h:61, 199-203; xes_data.h:123-135). std::priority_queue<Data*> defaults to std::less<Data*> (pointer comparison); the std::less<xes::Data> specialization on periods can never be selected for Data* and is dead code proving intent. Pool elements come from a forward_list (arbitrary heap addresses), so with >=2 queued histograms ready.top() pops an arbitrary period; the writer merges one histogram per module into a single frame and takes max period (lines 200-203), so a lagging writer silently mixes save periods and emits frames out of period order. Counter-eviden</sub>

### TPX-008 · [HIGH] WebSocket state stream: unsynchronized access to shared static 'ws' allows use-after-free between handler threads

**Claim.** All /ws connections share one static unique_ptr<WebSocket> ws. The handler's receive loop reads ws and calls ws->receiveFrame() without holding ws_mutex, while a second /ws connection's handler does ws.reset(new WebSocket(...)) under the mutex - deleting the WebSocket object the first thread is currently blocked inside. The exiting old handler also resets/shuts down whatever ws currently holds (the new client's socket). This is the code basis of the known one-client-slot incident, plus a UAF.

**Proof.**

`src/include/rest_callbacks.h:156-157`
```cpp
std::lock_guard lock(ws_mutex);
ws.reset(new WebSocket(request, response));
```
`src/include/rest_callbacks.h:168-170`
```cpp
while ((ws != nullptr) && !stop_sig) {
    try {
        n = ws->receiveFrame(buffer, sizeof(buffer), flags);
```
`src/include/rest_callbacks.h:195-199`
```cpp
std::lock_guard<std::mutex> lock(ws_mutex);
if (ws != nullptr) {
    ws->shutdown();
    ws.reset(nullptr);
}
```

**Impact.** A second /ws client (monitoring tool, stray browser tab) not only steals the state stream from the BEC client (already caused real scan failures: client never saw 'await_connection'), it can crash tpx3app: the first handler thread executes methods on a deleted WebSocket, and the old handler's cleanup tears down the new client's connection.

**Fix.** Give each handler its own WebSocket local object and keep a mutex-protected registry (vector) of active sockets; StateHandler::set_state broadcasts to all registered sockets (dropping dead ones); handlers deregister only their own socket on exit. Never reset a unique_ptr another thread may be executing inside.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (rest_callbacks.h:156-157, 168-170, 195-199). ws is a static inline unique_ptr shared by all /ws handler threads (line 134). The receive loop reads ws and calls ws->receiveFrame() with no lock, while a second connection's handleRequest does ws.reset(new WebSocket) under ws_mutex — destroying the WebSocket the first thread is blocked inside (1s receive timeout, so it is inside receiveFrame most of the time): UAF. The first handler's cleanup block (195-199) then shuts down and resets whatever ws holds, i.e. the new client's socket. No refcount, per-connection ownership, or genera</sub>

### TPX-009 · [MEDIUM] Data race on global program-state string_view (torn reads by REST threads)

**Claim.** global::state is a plain std::string_view (16 bytes, non-atomic) written by the main thread via set_state (without any lock in non-server mode) and read without synchronization by REST handler threads in /state and /?start. Concurrent read/write of a two-word object is a data race; a torn read can pair one state's pointer with another's length.

**Proof.**

`src/include/global.h:114`
```cpp
std::string_view state{init};                                                //!< program state (TODOD: protect with lock, if necessary)
```
`src/include/rest_callbacks.h:403`
```cpp
oss << R"({"type":"ProgramState","state":")" << global::instance->state << R"("})";
```
`src/include/rest_callbacks.h:219-221`
```cpp
} else {
    global::instance->state = state;
}
```

**Impact.** REST /state polled by the Python client during state transitions can return truncated/garbage state strings (UB), which the pydantic literal on the client side rejects - the same failure signature as known incident 4 ('except' not in the literal), but intermittent and unreproducible.

**Fix.** Since all states are static string constants, store std::atomic<const char*> (release store, acquire load) or guard every read/write of state with ws_mutex/a dedicated state mutex, including the non-server-mode write in set_state.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (global.h:114 including the 'TODOD: protect with lock' comment; rest_callbacks.h:403, 219-221). state is a plain 16-byte string_view. Writes: under ws_mutex in server mode (213-217), with NO lock in non-server mode (219-221). Reads: /state (403) and /?start's 'gvars.state != "config"' (429) take no lock in either mode, and the config callbacks read it under a different mutex (configLock), so every mode has unsynchronized read/write pairs — a data race on a two-word object where a torn read can pair one literal's pointer with another's length. Medium is appropriate: real UB with</sub>

### TPX-010 · [MEDIUM] No backpressure: reader allocates and mlocks unbounded jars when analysers fall behind

**Claim.** next_jar() never blocks: when the free list is empty it heap-allocates a new jar and (with the default pin_data=true used by DataHandler's databuf{numChips}) mlocks it. Sustained producer-faster-than-consumer growth is unbounded; hitting RLIMIT_MEMLOCK makes container_t::pin throw inside the reader thread, aborting the scan.

**Proof.**

`src/include/io_buf.h:295-301`
```cpp
if (!free) {
    // create new container
    jar_list.emplace_back(new jar_t);
    free = jar_list.back().get();
    if (pin_data)
        free->container.pin();
}
```
`src/include/io_buf.h:87-88`
```cpp
if (mlock(data, container_size))
    throw Poco::SystemException("internal - mlock failed", errno);
```
`src/include/data_handler.h:475-476`
```cpp
inline DataHandler(Logger& log, Analysis& analysisObj, unsigned numChips, unsigned long queueSize)
    : dataStream{}, logger{log}, databuf{numChips}, reorderSize{queueSize},
```

**Impact.** High-rate scans where histogramming cannot keep up (or where the writer stalls output) grow pinned memory without bound until 'reader exception: internal - mlock failed' kills the measurement, or the OOM killer takes the process.

**Fix.** Cap the jar count (configurable, e.g. N seconds of buffering) and make the producer wait (interruptible via stop_flag) in write_reservation when the cap is reached; at minimum log jar_list growth so operators can see backpressure.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (io_buf.h:295-301, 87-88; data_handler.h:475-476). next_jar() never blocks: empty free list -> jar_list.emplace_back(new jar_t) + pin(). collection_t's constructor defaults pinned=true (io_buf.h:224) and DataHandler passes databuf{numChips} without overriding, so pin_data is true. Free list is only refilled by return_jar when all consumers finish a jar, so sustained producer>consumer growth is unbounded; jar_list also never shrinks. mlock failure at RLIMIT_MEMLOCK throws Poco::SystemException inside write_reservation on the reader thread -> caught at data_handler.h:276 -> stopN</sub>

### TPX-011 · [MEDIUM] Subreservation restore path never returns the last stored jar (per-reorder jar leak)

**Claim.** During restore, intermediate stored jars are returned when transitioning between pieces, but when the store empties, restored_jar is set to nullptr without buffers.return_jar(). If the parser had crossed out of that jar earlier (its return was skipped via the no_return flag because store.back().jar == reservation.jar), the jar's done count sticks at nthreads-1 and it is never recycled for the rest of the measurement.

**Proof.**

`src/include/subreservation.h:153-163`
```cpp
if (store.empty()) {
    end = reservation.end / event_size;
    content = (AsiRawStreamDecoder::Event*)current.jar->container.data;
    pos = current.pos;
    rest = current.rest;
    consume = current.consume;
    current = {};
    state = CHECK_ID;
    restored_jar = nullptr;
    stored_pkgid = 0;
```
`src/include/subreservation.h:169-170`
```cpp
if (restored_jar && (restored_jar != restore.jar))
    buffers.return_jar(restored_jar);
```
`src/include/subreservation.h:77-78`
```cpp
else
    reservation = buffers.read_reservation(reservation, store.back().jar == reservation.jar);
```

**Impact.** Every reordered-chunk event whose stored data ends in a jar the parser has already left leaks one jar until the end-of-measurement reset, compounding the unbounded-allocation finding: long scans on reorder-prone SERVAL streams steadily grow the (mlocked) pool.

**Fix.** In the store.empty() branch: 'if (restored_jar && restored_jar != current.jar) buffers.return_jar(restored_jar);' before clearing restored_jar (the current.jar comparison avoids double-returning when parsing resumes inside the same jar).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (subreservation.h:153-162 within cited 153-163, 169-170, 77-78). Traced the full store/restore lifecycle: store_data crosses jars with read_reservation(...,true) (line 132) so stored jars are not returned at store time; when the in-order parser later leaves the jar containing the last stored piece, update_reservation passes no_return = (store.back().jar == reservation.jar) = true (77-78), skipping that jar's return too. During restore, intermediate jars are returned on piece transitions (169-170), but the store.empty() branch clears restored_jar without return_jar. If parsing r</sub>

### TPX-012 · [MEDIUM] Packet reordering deeper than one chunk aborts the whole measurement; unmatched stored chunk dropped silently at stream end

**Claim.** The reorder machinery holds exactly one out-of-order chunk (stored_pkgid). A second out-of-order id while one is stored throws, which the analyser catch turns into stopNow() + error - killing the scan. Additionally, if the stream ends while a stored chunk was never matched, update() returns with store non-empty and the stored events are silently discarded with no log.

**Proof.**

`src/include/subreservation.h:208`
```cpp
throw RuntimeException{std::string{"unable to handle reordered chunk, expected id "} + std::to_string(pkgid) + ", but got id " + std::to_string(content[pos].packet_id.count)};
```
`src/include/subreservation.h:292-298`
```cpp
while (pos >= end) {
    update_reservation();   // modifies pos and end
    if (! end) {
        if (rest)
            throw RuntimeException{"premature end of data package"};
        return;
    }
```

**Impact.** SERVAL chunk reordering (real enough that the single-slot store/restore exists) of depth 2 or more fails the entire scan with 'except'; scans ending mid-reorder silently lose the stored chunk's events (undetected data loss for that chip).

**Fix.** Generalize the store to a small map keyed by pkgid (bounded, e.g. reorder window of N chunks) instead of a single stored_pkgid; on end-of-stream with non-empty store, log a warning with the dropped chunk id/size instead of silent discard.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (subreservation.h:208, 292-298). check_id (194-208): on id mismatch with a non-empty store whose stored_pkgid != pkgid, it throws — a single-slot store, so reorder depth >=2 kills the scan via the analyser catch (data_handler.h:437-445: stopNow + set_error -> 'except'). End-of-stream: update() returns at line 297 when end==0 and rest==0 with no check or log of a non-empty store, silently dropping the stored chunk's events for that chip. No other drain path for the store exists (reset() just clears it). Both halves confirmed; medium/robustness appropriate.</sub>

### TPX-013 · [MEDIUM] CopyHandler aborts on any 300 ms lull in the raw stream (no TimeoutException handling)

**Claim.** main.cpp sets a 300 ms receive timeout on the data socket before both branches, but CopyHandler::readBytes (unlike DataHandler::readData) has no Poco::TimeoutException handler; the first 300 ms gap in the stream throws, hits the outer catch, and terminates the copy.

**Proof.**

`src/include/copy_handler.h:51-56`
```cpp
do {
    int numRead = dataStream.receiveBytes(&static_cast<char*>(buf)[numBytes], size - numBytes);
    if (numRead == 0)
        break;
    numBytes += numRead;
} while (numBytes < size);
```
`src/main.cpp:756-757`
```cpp
set_state(global::collect);
dataStream.setReceiveTimeout(gvars.collect_timeout);
```
`src/include/global.h:33`
```cpp
static constexpr unsigned collect_timeout{300000};                     //!< 300ms receive timeout for detector data
```

**Impact.** Stream-to-file capture (the tool used to record reference streams for the replay server) dies with 'reader exception: Timeout' on any brief acquisition pause or slow start, producing truncated raw files.

**Fix.** Mirror DataHandler::readData: catch Poco::TimeoutException inside the read loop and continue unless a stop flag (stop_collect/stop/queue stop) is set.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (copy_handler.h:51-56; main.cpp:756-757; global.h:33). main.cpp sets the 300 ms receive timeout at line 757, before the copy_mode branch at 759, so CopyHandler's socket has it. readBytes has no TimeoutException handler (contrast data_handler.h:213-218); Poco::TimeoutException derives from Poco::Exception and propagates to readData()'s outer catch (copy_handler.h:105-111) -> stopNow + 'reader exception' -> reader thread exits and the capture ends. Any 300 ms gap (slow acquisition start, pause) truncates the raw file. Confirmed medium.</sub>

### TPX-014 · [MEDIUM] CopyHandler never resets its iobuf collection: second copy-mode measurement in server mode uses stale buffer state

**Claim.** DataHandler::await() resets its collection via iobuf::resetter, but CopyHandler::await() only joins threads. After the first measurement, queue keeps the old final_jar, jar levels, consumed free-list state, and possibly stop_flag; the next server-mode iteration reuses it, so write_reservation/read_reservation operate on stale levels (put_data can even see level moving backward, and the writer's await_data(first, 0) immediately returns the old full level, writing stale bytes).

**Proof.**

`src/include/copy_handler.h:219-224`
```cpp
inline void await()
{
    readerThread.join();
    if (! readOnly)
        writerThread.join();
}
```
`src/include/data_handler.h:541-543`
```cpp
inline void await()
{
    iobuf::resetter reset(databuf);
```
`src/main.cpp:759-767`
```cpp
if (copy_mode) {
    auto& copyHandler = *copyPtr;
    Timer timer;

    copyHandler.rawDataStream(dataStream);
    copyHandler.run_async();
    copyHandler.await();
```

**Impact.** Running --stream-to-file together with --server-mode: every measurement after the first produces truncated, duplicated, or empty raw files (writer sees the previous run's final_jar and old fill levels), silently corrupting captured reference data.

**Fix.** Add 'iobuf::resetter reset(queue);' at the top of CopyHandler::await() (or call queue.reset() after the joins), and also reset the read/write statistics counters per run.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (copy_handler.h:219-224; data_handler.h:541-543; main.cpp:759-765 within cited 759-767). CopyHandler is constructed once before the server loop (main.cpp:664) and its queue (collection_t) is never reset — no iobuf::resetter and no queue.reset() call anywhere in copy_handler.h, unlike DataHandler::await(). After run 1, final_jar remains set and jar levels are stale, so in run 2 the writer's read_reservation -> await_data(first, 0) sees the old nonzero level immediately and writes stale bytes, and the producer's second write_reservation hits the stale final_jar/level state (and s</sub>

### TPX-015 · [LOW] global::error_empty() reads last_error without the error mutex

**Claim.** set_error/get_error lock error_lock, but error_empty() reads last_error.empty() unlocked while reader/analyser/writer threads may be concurrently assigning it - a data race (UB) on std::string used to drive the main server loop's state decisions.

**Proof.**

`src/global.cpp:37-40`
```cpp
bool global::error_empty() noexcept
{
    return last_error.empty();
}
```
`src/include/data_handler.h:277-279`
```cpp
} catch (Poco::Exception& ex) {
    stopNow();
    logger << "reader exception: " << ex.displayText() << log_critical;
    global::set_error(std::string{"reader: "} + ex.displayText());
```
`src/main.cpp:682-687`
```cpp
if (! gvars.error_empty()) {
    if (gvars.stop_collect) {   // prevent racing effects on data collection stop
        gvars.set_error();
    } else {
        set_state(global::except);
    }
```

**Impact.** Main can read a torn size during a concurrent worker-thread set_error, missing an error (skipping the 'except' state the client depends on) or spuriously entering it; formally UB.

**Fix.** Lock error_lock inside error_empty() (one-line: 'std::lock_guard lock(error_lock); return last_error.empty();').

<sub>Verification: **adjusted** (severity/lines adjusted by verifier; corrected lines: src/include/data_handler.h: 276-279 (quote starts one line earlier than cited 277-279)). Author confidence: high.</sub>

<sub>Verifier: Quotes verified: global.cpp:37-40 verbatim (no lock, vs set_error/get_error which lock error_lock); main.cpp:682-687 verbatim; the data_handler.h quote actually spans 276-279, not 277-279 (catch line is 276). The data race is formally real (std::string read while workers assign). However the practical window is narrow: main calls error_empty() at loop top / after the loop, when reader+analysers+writer are normally parked behind await(); genuine concurrency requires the desync/exception paths of other findings. empty() reads one aligned size_t, effectively atomic on the target platforms, so a t</sub>

### TPX-016 · [LOW] Debug-build assert(rest > 1) fires on legal single-event packets resuming in CHECK_ID state

**Claim.** search_pkg allows size == 2 (one packet-id word + one event, rest = size - 1 = 1). If such a packet's id word lands at a reservation boundary (or parsing resumes in CHECK_ID after a restore), update() re-enters with state CHECK_ID and asserts rest > 1, which is false for a one-event packet - crashing debug builds on a valid stream.

**Proof.**

`src/include/subreservation.h:310-312`
```cpp
case CHECK_ID:
    assert(content && (pos >= 0) && (rest > 1));
    more = check_id();
```
`src/include/subreservation.h:234-239`
```cpp
#if SERVER_VERSION >= 320
    if (__builtin_expect(size < 2, false))
#else
    if (__builtin_expect(size < 1, false))
#endif
        throw RuntimeException{"encountered bogus chunk size"};
```
`src/include/subreservation.h:243-246`
```cpp
#if SERVER_VERSION >= 320
    rest = size - 1;
    state = CHECK_ID;
    return check_id();
```

**Impact.** Debug/test builds abort on streams containing minimal (single-event) chunks at unlucky buffer alignments, masking real testing of the reorder path; release builds are unaffected.

**Fix.** Relax the assertion to 'rest >= 1' (one event after the id word is legal when size == 2).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (subreservation.h:310-312, 234-239, 243-246). For SERVER_VERSION >= 320, search_pkg accepts size == 2 (only size < 2 throws) and sets rest = size - 1 = 1 with state = CHECK_ID. The assert at 311 only runs when CHECK_ID is re-entered through update()'s switch, which happens when (a) the chunk header is the last word of a reservation (check_id returns true on pos >= end, update_reservation runs, switch re-enters) or (b) restore_data's store.empty() branch resumes at 'current' with state CHECK_ID — in both cases rest can legally be 1, failing 'rest > 1' and aborting debug builds o</sub>

### TPX-017 · [LOW] set_affinity rejects CPUs >= 128 (sizeof(cpu_set_t) used as CPU-index bound)

**Claim.** The bound check compares the CPU index against sizeof(cpu_set_t) (128 bytes) instead of CPU_SETSIZE (1024 bits), so pinning to CPUs 128-1023 returns EINVAL and the affinity is silently skipped (only a log_error).

**Proof.**

`src/include/cpu_mask.h:45-46`
```cpp
if ((size_t)cpu >= sizeof(cpu_set_t))
    return EINVAL;
```

**Impact.** On DAQ servers with more than 128 logical CPUs (dual-socket EPYC class), the configured reader/analyser/writer pinning silently does not apply, degrading throughput isolation without an obvious failure.

**Fix.** Compare against CPU_SETSIZE (or 8 * sizeof(cpu_set_t)): 'if ((size_t)cpu >= CPU_SETSIZE) return EINVAL;'.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (cpu_mask.h:45-46). The guard compares the CPU index against sizeof(cpu_set_t) — 128 bytes on glibc — instead of CPU_SETSIZE (1024 bits), so any configured CPU 128-1023 returns EINVAL before CPU_SET. All call sites (data_handler.h:235-237, 313-315; xes_data_manager.h:141-143; copy_handler.h:73-75, 127-129) only log the error at log_error and continue, so pinning is silently ineffective on >128-logical-CPU hosts. Confirmed low.</sub>

### TPX-018 · [LOW] Per-scan throughput statistics accumulate across server-mode measurements, corrupting logged rates

**Claim.** DataHandler's public counters (toaCount, byteCount, read/analyse times) are accumulated under memberMutex each cycle but never reset between measurements; logOutput divides cumulative counters by the current measurement's wall time.

**Proof.**

`src/include/data_handler.h:287-293`
```cpp
{
    std::lock_guard lock{memberMutex};
    readTime += workTime;
    readSpinTime += spinTime;
    readTotalTime += (workTime + spinTime);
    byteCount += readBytes;
}
```
`src/include/data_handler.h:557-563`
```cpp
const uint64_t ntoa = toaCount;
const uint64_t ntdc = tdcCount;
const u64 readCount = (byteCount / sizeof(u64));
const auto numChips = gvars.layout.chip.size();
const double avgAnalysisWorkTime = analyseWorkTime / numChips;
const double avgAnalysisTime = (analyseWorkTime + analyseSpinTime) / numChips;
logger << "time: " << time << "s tdcs: " << ntdc << " toas: " << ntoa << " at " << (ntoa / time)
```

**Impact.** From the second scan onward, logged event counts and rates are inflated (sums of all previous scans divided by one scan's time), making the primary performance/diagnostic log line useless for spotting real throughput regressions at the beamline. CopyHandler has the same defect.

**Fix.** Zero all statistic members at the start of each cycle (e.g. in run_async() or right after start signals are consumed), or log per-cycle deltas.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (data_handler.h:287-293, 557-563). The public counters (toaCount, tdcCount, byteCount, read*/analyse* times, lines 579-589) are only ever initialized at construction and '+='-accumulated per cycle; grep shows no zeroing anywhere in data_handler.h, and DataHandler is constructed once before the server loop while logOutput divides by the per-scan Timer (main.cpp:771, 781, 784). From scan 2 onward the log line reports cumulative sums over one scan's wall time. CopyHandler has the same pattern (copy_handler.h:244-249), though it logs only once per process run unless combined with s</sub>

### TPX-019 · [LOW] Copy-mode thread names exceed the Linux 15-character limit and are silently dropped

**Claim.** set_thread_name ignores pthread_setname_np's return value; 'tpx3app:cp-reader' and 'tpx3app:cp-writer' are 17 characters, exceeding the Linux 15-char (plus NUL) limit, so the call fails with ERANGE and those threads keep the default name.

**Proof.**

`src/include/thread_naming.h:20-24`
```cpp
inline void set_thread_name(const std::string& name)
{
    auto tid = pthread_self();
    pthread_setname_np(tid, name.c_str());
}
```
`src/include/copy_handler.h:66`
```cpp
set_thread_name("tpx3app:cp-reader");
```
`src/include/copy_handler.h:121`
```cpp
set_thread_name("tpx3app:cp-writer");
```

**Impact.** During hangs (several of which this audit found), ps/htop/gdb show unnamed copy threads, slowing live debugging of stuck DAQ processes.

**Fix.** Truncate the name to 15 chars in set_thread_name (name.substr(0, 15)) and use shorter names ('tpx3:cp-read', 'tpx3:cp-write').

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (thread_naming.h:20-24; copy_handler.h:66, 121). 'tpx3app:cp-reader' and 'tpx3app:cp-writer' are 17 characters; Linux pthread_setname_np fails with ERANGE for names longer than 15 chars + NUL, and the return value is ignored, so the copy threads keep the default name. The other names fit ('tpx3app:reader' 14, 'tpx3app:analyze' 15, 'tpx3app:writer' 14), so only the copy-mode threads are affected. Confirmed low.</sub>

### TPX-020 · [LOW] stop_now() sets the flag without notifying condition variables; aborts rely on 1 s poll ticks

**Claim.** collection_t::stop_now() only stores stop_flag; consumers blocked in await_data notice it solely because the wait uses wait_for(lock, 1s). Combined with the reader's 300 ms receive timeout, every abort path (REST stop_collect, analyser exceptions) carries up to ~1.3 s of avoidable latency per wait point rather than waking waiters immediately.

**Proof.**

`src/include/io_buf.h:260-263`
```cpp
inline void stop_now() noexcept
{
    stop_flag.store(true, std::memory_order_release);
}
```
`src/include/io_buf.h:207-214`
```cpp
std::unique_lock lock{jar->level_lock};
do {
    if (jar->level != level)
        return jar->level;
    if ((jar == final_jar.load(std::memory_order_consume)) || stop_flag.load(std::memory_order_consume))
        return 0;
    jar->level_cond.wait_for(lock, 1s);
} while (true);
```

**Impact.** Not a hang (the timed wait masks it), but scan aborts and error propagation are sluggish, and the 1 s poll is what currently hides the absence of proper wakeups - any future change of wait_for to wait would convert this into a deadlock.

**Fix.** In stop_now(), after setting the flag, iterate jar_list and notify_all() each jar's level_cond under its level_lock (jar_list is stable at that point for the producer; guard with free_lock if needed).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (io_buf.h:260-263, 207-214). stop_now() only stores stop_flag and notifies no one; blocked consumers in await_data re-check the flag only when wait_for(lock, 1s) times out, and the reader adds its own up-to-300 ms receive-timeout tick before stop_collect is seen, so abort paths carry up to ~1.3 s latency per wait point. Not a hang today — the 1 s timed wait is the only thing masking the missing wakeups — and the observation that switching wait_for to wait would deadlock is accurate (same missing-notify pattern as finding 2). Confirmed low/operability.</sub>


## Network & control plane

*17 findings: 1 critical, 6 high, 9 medium, 1 low*

### TPX-021 · [CRITICAL] Single static WebSocket slot: replacement causes use-after-free and cross-thread races, not just stream stealing

**Claim.** All /ws handler threads share one static `ws` unique_ptr. A new /ws connection calls `ws.reset(new WebSocket(...))` under `ws_mutex`, but the previous handler thread dereferences `ws` and blocks inside `ws->receiveFrame()` WITHOUT holding the mutex, so the reset deletes the WebSocket object while another thread is executing a method on it (use-after-free), and afterwards two or more handler threads call receiveFrame/sendFrame concurrently on the same replaced WebSocket object (unsynchronized). This is the code basis of the known incident (each new /ws client silently replaces the previous), but the actual behavior is undefined behavior/possible heap corruption, plus orphaned handler threads that keep spinning on the shared `ws` and permanently occupy Poco HTTPServer worker threads (default pool ~16), eventually starving REST request handling. Additionally, whichever handler exits first runs the shared cleanup and shuts down the CURRENT (other client's) socket.

**Proof.**

`src/include/rest_callbacks.h:134-136`
```cpp
static inline std::unique_ptr<WebSocket> ws;//!< single WebSocket
        static inline std::mutex ws_mutex;          //!< Protect WebSocket
        static inline std::atomic_bool stop_sig;    //!< Stop signal
```
`src/include/rest_callbacks.h:156-157`
```cpp
std::lock_guard lock(ws_mutex);
                    ws.reset(new WebSocket(request, response));
```
`src/include/rest_callbacks.h:168-170`
```cpp
while ((ws != nullptr) && !stop_sig) {
                    try {
                        n = ws->receiveFrame(buffer, sizeof(buffer), flags);
```

**Impact.** Two clients (or one client reconnecting while its old TCP connection is alive) trigger deletion of a WebSocket mid-receiveFrame: crash or memory corruption of the DAQ process mid-scan. Even absent a crash, the state stream is stolen (confirmed live incident: client never saw 'await_connection') and stale handler threads exhaust the HTTP thread pool, making REST control (stop/start) unresponsive.

**Fix.** Make the WebSocket per-connection state owned by the handler instance (non-static member). Keep a mutex-protected registry (e.g. std::vector<std::weak_ptr<WebSocket>> or a client list with per-client send mutex) that set_state() iterates to broadcast state to ALL connected clients; each handler adds itself on connect and removes exactly itself on exit. Never reset/shutdown a socket owned by another handler thread.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All three quotes verbatim at cited lines (rest_callbacks.h 134-136, 156-157, 168-170). The ws_mutex scope ends at line 162; the receive loop (168-188) dereferences the static ws with no lock, so a second /ws connection's ws.reset() at 157 deletes the object while the first handler is inside receiveFrame (UAF), or leaves two handler threads calling receiveFrame/sendFrame on the same replaced object unsynchronized. The unconditional cleanup at 194-200 shuts down whatever ws currently holds, i.e. the OTHER client's socket. Orphaned handlers loop on the shared ws until close/error and occupy Poco </sub>

### TPX-022 · [HIGH] Any failed request to /ws (plain HTTP GET, probe, handshake error) destroys the active client's WebSocket

**Claim.** If `new WebSocket(request, response)` throws (any non-upgrade GET to /ws, e.g. `curl http://host:8452/ws`, a load-balancer health check, or a port scanner), `ws.reset(...)` is never executed, the exception is caught at line 190, and the unconditional cleanup block then shuts down and deletes the static `ws` — which at that moment is the healthy, active client's WebSocket (whose own handler thread is concurrently blocked in receiveFrame on that object, adding a use-after-free).

**Proof.**

`src/include/rest_callbacks.h:190-199`
```cpp
} catch (std::exception& exc) {
                logger << "websocket: error - " << exc.what() << log_warn;
            }

            {
                std::lock_guard<std::mutex> lock(ws_mutex);
                if (ws != nullptr) {
                    ws->shutdown();
                    ws.reset(nullptr);
```
`src/include/rest_callbacks.h:156-158`
```cpp
std::lock_guard lock(ws_mutex);
                    ws.reset(new WebSocket(request, response));
                    logger << "websocket: created" << log_debug;
```

**Impact.** A single stray non-WebSocket request to /ws — trivially generated by monitoring tools or a curl typo — kills the BEC client's state stream mid-scan (same failure signature as the known 'never saw await_connection' incident) and races the legitimate handler thread on the deleted object.

**Fix.** Have the handler track the WebSocket it created in a local (e.g. std::shared_ptr<WebSocket> mine) and in the cleanup block only clear the static slot if it still points to `mine` (compare pointers under the mutex). Better: per-connection ownership as in the multi-client fix.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (190-199, 156-158). ws.reset(new WebSocket(request, response)): the new-expression is evaluated before reset, so a throwing handshake (any non-upgrade GET to /ws throws Poco WebSocketException) leaves ws pointing at the healthy client's socket; the exception is caught at 190 and the unconditional cleanup at 194-200 then shutdown()s and deletes that healthy socket. The factory (line 254) routes every request with URI == "/ws" to StateHandler regardless of method/headers, so a plain curl GET triggers it. The victim's handler thread is concurrently in receiveFrame without the mute</sub>

### TPX-023 · [HIGH] StateHandler::set_state sends WS frame with no exception guard; a dead WS client can terminate the whole application

**Claim.** set_state() calls `ws->sendFrame(...)` with no try/catch, and Tpx3App::main calls set_state at several points OUTSIDE the try block of the server loop (lines 686, 692, 704) and inside the catch blocks (788, 798) and shutdown (810). If the WS client died abruptly (TCP RST, e.g. client host crash with unread data), sendFrame throws Poco::Net::ConnectionResetException; thrown from lines 686/692/704 (or re-thrown from a catch block) it propagates out of Tpx3App::main into Poco Application::run, which logs it and returns — the do-loop in ::main then breaks (restart flag unset) and the process exits, skipping the orderly shutdown path (StateHandler::stop, rest::stop_service, dataHandler shutdown).

**Proof.**

`src/include/rest_callbacks.h:213-217`
```cpp
if (global::instance->state != state) {
                    global::instance->state = state;
                    if (ws == nullptr)
                        return;
                    ws->sendFrame(state.data(), state.size(), WebSocket::FRAME_TEXT);
```
`src/main.cpp:690-692`
```cpp
if (server_mode) {  // wait for start signal
                    using namespace std::chrono_literals;
                    set_state(global::config);
```
`src/main.cpp:703-706`
```cpp
gvars.stop_collect = false;
                set_state(global::setup);

                try {
```

**Impact.** An abruptly disconnected BEC client (network blip, host reboot) can take down the entire tpx3app backend at the next state transition between scans; all subsequent scans fail with connection refused on 8452 until the service is restarted.

**Fix.** Wrap the sendFrame in set_state in try/catch; on any exception log, `ws->shutdown()` best-effort, and `ws.reset()` (drop the dead client) instead of propagating. State bookkeeping (global state assignment) must happen regardless of send success (it already does; keep that ordering).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (rest_callbacks.h 213-217; main.cpp 690-692, 703-706). set_state has no try/catch around sendFrame. Call sites 686, 692, 704 are inside the do-loop but outside the try block starting at 706; 788/798 are inside catch handlers, so a throw there also escapes Tpx3App::main; 810 is outside too. Poco Application::run catches and logs the exception and returns; the do-loop in ::main (929-938) then breaks because restart is unset, exiting without StateHandler::stop/rest::stop_service/dataHandler shutdown (810-816 skipped). One nuance not fatal to the claim: an abrupt RST usually also w</sub>

### TPX-024 · [HIGH] Restart leaves StateHandler::stop_sig latched true: WebSocket state push permanently dead after /?restart or SIGUSR1

**Claim.** StateHandler::stop() sets the static `stop_sig = true` during shutdown of a run (main.cpp:811), but nothing ever resets it. After a restart (REST /?restart or `tpx3app restart` via SIGUSR1, both supported), app.run() re-enters Tpx3App::main and restarts the REST service, but every new /ws handler evaluates `while ((ws != nullptr) && !stop_sig)` with stop_sig still true: it sends the single initial state frame, skips the loop, and immediately shuts the socket. The Python client's wait_for_connection/_ws_send_and_receive then reconnects in a tight loop with no backoff for the connect-succeeds-then-closes case (its 0.5 s sleep is only reached on ConnectionRefusedError).

**Proof.**

`src/include/rest_callbacks.h:227-230`
```cpp
inline static void stop() noexcept
        {
            stop_sig = true;
        }
```
`src/main.cpp:810-812`
```cpp
set_state(global::shutdown);
            StateHandler::stop();
            rest::stop_service(restService.get());
```
`src/main.cpp:929-933`
```cpp
do {
                global::instance->stop.store(false);
                global::instance->restart.store(false);
                retval = app.run();
                if (global::instance->restart.load()) {
```

**Impact.** After any restart, the state stream is permanently unavailable: BEC never receives state pushes (same scan-failure signature as the known WS incident), and the client burns CPU/log volume in a fast connect/close churn against the backend.

**Fix.** Reset the flag at service (re)start: add `StateHandler::reset()` (stop_sig = false) and call it from rest::start_service or at the top of Tpx3App::main; or make stop_sig instance/service-scoped rather than a process-lifetime static.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (rest_callbacks.h 227-230; main.cpp 810-812, 929-933). grep confirms stop_sig appears only at declaration (136), loop condition (168), and stop() (229) — never reset to false. It is a static inline member surviving app.run() re-entry, so after restart every new /ws handler sends the single initial frame at 161, skips the loop, and immediately shuts the socket in the cleanup block. Python client verified: wait_for_connection's 0.5 s sleep (line 232) is only reached after a caught ConnectionRefusedError; a connect-succeeds-then-closes cycle (WebSocketException in _ws_send_and_rec</sub>

### TPX-025 · [HIGH] gvars.stop_handlers holds dangling references after restart and is mutated unsynchronized while REST threads iterate it

**Claim.** Tpx3App::main registers stop handlers as lambdas capturing LOCAL unique_ptrs by reference (`[&dataHandlerPtr]`, `[&copyPtr]`) into the global `gvars.stop_handlers` vector, which is never cleared (verified by grep: only emplace_back and iteration exist). After /?restart or SIGUSR1, main() returns (destroying those locals) and is re-entered, appending a second handler; the next /?stop, /?restart or /?stop_collect iterates ALL entries and invokes the stale lambda, dereferencing a dangling reference to a destroyed stack unique_ptr (UB/crash). The Python client calls stop_collect() on every on_connected and scan teardown, so this fires almost immediately after any restart. Separately, the REST service is started (line 657) BEFORE the handlers are emplaced (lines 665/674), so a /?stop_collect arriving in that window (the client issues one right after first REST contact) iterates the vector concurrently with emplace_back — a data race on std::vector.

**Proof.**

`src/main.cpp:674-676`
```cpp
gvars.stop_handlers.emplace_back([&dataHandlerPtr]() {
                    dataHandlerPtr->stopNow();
                });
```
`src/include/rest_callbacks.h:357-361`
```cpp
auto& gvars = *global::instance;
                gvars.stop_collect.store(true);
                for (const auto& handler : gvars.stop_handlers)
                    handler();
                return "OK";
```
`src/main.cpp:656-657`
```cpp
rest::init_callbacks();
            auto restService = rest::start_service(logger, gvars.controlAddress);
```

**Impact.** After one restart, the first abort/stop from the client dereferences freed stack memory: crash or silent corruption of the DAQ process. The startup-window race can crash the process exactly when the BEC client connects.

**Fix.** Clear gvars.stop_handlers at the top of Tpx3App::main (or use a scope guard that removes the entries this run registered before returning), capture shared_ptrs by value instead of locals by reference, and protect the vector with a mutex (or register handlers before rest::start_service and make the vector immutable afterwards).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp 674-676, 656-657; rest_callbacks.h 357-361). Independently re-verified by grep: stop_handlers is only ever emplace_back'd (main.cpp 665, 674) and iterated (rest_callbacks.h 324, 342, 359) — never cleared. The lambdas capture local unique_ptrs by reference; after restart Tpx3App::main returns (destroying them) and re-enters, appending a second handler, so the next /?stop, /?restart, or /?stop_collect invokes the stale lambda over a dangling reference (UB). The startup window race is also real: REST service starts at 657 before emplace_back at 665/674, and the vector ha</sub>

### TPX-026 · [HIGH] await_connection poll loop ignores gvars.stop/restart: REST stop and SIGTERM can hang the server until SERVAL connects

**Claim.** The accept-wait poll loop breaks only on POLLIN or `gvars.stop_collect`; it never checks `gvars.stop` or `gvars.restart`. REST /?stop and /?restart only set stop/restart and call stop_handlers (which affect the not-yet-running data pipeline, not this loop), so if SERVAL never connects (aborted scan, SERVAL down), the main thread polls forever and the process cannot be stopped via REST. SIGTERM works only by accident: if the kernel delivers it to the main thread, poll returns -1/EINTR — which the code treats as a FATAL error (`throw Poco::RuntimeException{"poll failed - Interrupted system call"}`), recording a spurious last-error and except state; if the signal lands on any HTTP worker thread instead, the poll is not interrupted and the process hangs until SIGKILL. SIGKILL then skips the atexit handler, leaving the /tmp/tpx3app.pid lockfile behind (created with O_CREAT|O_EXCL), so the next tpx3app start is refused with 'lockfile exists ... is another tpx3app already running?'.

**Proof.**

`src/main.cpp:741-749`
```cpp
if (ret == -1) {
                                throw Poco::RuntimeException(std::string{"poll failed - "} + std::strerror(errno));
                            } else if (ret == 0) {  // timeout
                                if (gvars.stop_collect)
                                    break;
                            } else if (fds[0].revents & POLLIN) {
                                break;
                            }
                        } while (true);
```
`src/include/rest_callbacks.h:322-325`
```cpp
auto& gvars = *global::instance;
                gvars.stop.store(true);
                for (const auto& handler : gvars.stop_handlers)
                    handler();
```
`src/main.cpp:87-90`
```cpp
fd = open(lock_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
            if (fd < 0) {
                if (errno == EEXIST) {
                    throw Poco::RuntimeException(std::string{"lockfile exists at "} + lock_file + ", is another tpx3app already running?");
```

**Impact.** Stopping/restarting tpx3app while it waits for the SERVAL data connection (a routine abort scenario, cf. the analogous test_server incident) hangs the process; systemd/operator escalation to SIGKILL then leaves a stale pidfile that blocks all subsequent starts until manually removed — a full beamline outage requiring manual intervention.

**Fix.** In the ret==0 branch also break on `gvars.stop || gvars.restart` (and then `break` out of the server loop rather than `continue`); handle EINTR by continuing the poll loop (`if (ret == -1) { if (errno == EINTR) continue; throw ...; }`).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp 741-749, 87-90; rest_callbacks.h 322-325). The poll loop breaks only on POLLIN or stop_collect; /?stop and /?restart set flags never checked here, and their stop_handlers (dataHandler stopNow) only latch databuf.stop_flag, which nothing in this loop reads — so the main thread polls forever. EINTR path verified: ret==-1 unconditionally throws (spurious error recorded, except state set), though in that case the process does then stop via the gvars.stop check at 693-698, matching 'works by accident'. Minor imprecision: Poco's own HTTP worker threads block SIGTERM (Thread</sub>

### TPX-027 · [HIGH] 'except' state lasts microseconds before being overwritten by 'config', and errors are silently discarded when stop_collect is set: REST pollers cannot observe scan failures

**Claim.** After a scan failure, the catch block sets state 'except', but the very next loop iteration immediately calls set_state(global::config) with no dwell time or acknowledgment gate, so 'except' is visible on GET /state only for microseconds. The Python client's REST poll fallback (1 s interval, added precisely because the single-slot WS stream can be stolen) will therefore essentially never observe 'except'; if the WS push was lost, a failed scan is seen as a clean transition to 'config' and status callbacks waiting on CONFIG resolve as success. Additionally, the loop-top logic unconditionally discards any recorded error when stop_collect is set (`if (gvars.stop_collect) gvars.set_error();`), so a genuine failure that coincides with a client-initiated abort is erased and never surfaced via state or /last-error.

**Proof.**

`src/main.cpp:682-692`
```cpp
if (! gvars.error_empty()) {
                    if (gvars.stop_collect) {   // prevent racing effects on data collection stop
                        gvars.set_error();
                    } else {
                        set_state(global::except);
                    }
                }

                if (server_mode) {  // wait for start signal
                    using namespace std::chrono_literals;
                    set_state(global::config);
```
`/Users/janwyzula/PSI/beamline_plugins/superxas_bec/superxas_bec/devices/timepix/timepix_fly_client/timepix_fly_client.py:267-271`
```cpp
rate guarantees that status callbacks still resolve when WebSocket pushes
        are missed. This runs in the websocket update thread, so callback execution
        stays serialized with regular WS updates. Only stable states can be relied
        upon here (config, await_connection); short transients may be skipped.
```
`src/main.cpp:787-788`
```cpp
gvars.set_error(ex.displayText());
                    set_state(global::except);
```

**Impact.** Combined with the known single-WS-slot loss, a failed scan can be reported to BEC as successful (status resolves on 'config'), producing silent data loss: the scan completes in the control system with no XES data and no error ever surfaced.

**Fix.** Make 'except' a latched state: stay in 'except' (do not enter 'config') until the client acknowledges (e.g., /last-error GET with reset, or an explicit /?clear-error=true), or embed the pending error in the /state response ({"state":"config","last_error":...}). Do not clear errors merely because stop_collect is set — only clear errors that are the known receive-timeout consequence of the stop.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier; corrected lines: timepix_fly_client.py 265-268 (third proof; the two main.cpp quotes at 682-692 and 787-788 are exact)). Author confidence: high.</sub>

<sub>Verifier: Logic fully confirmed: the catch sets except (788), the loop immediately re-enters, and set_state(global::config) at 692 overwrites it with no dwell or acknowledgment gate, so the 1 Hz REST poller essentially never observes 'except'; a WS-push loss then makes a failed scan resolve as a clean transition to config. Also verified: gvars.set_error() with default empty arg at 684 erases a genuine error whenever stop_collect is set. Only correction: the quoted Python docstring text sits at lines 265-268, not 267-271. Severity high (silent data loss) stands.</sub>

### TPX-028 · [MEDIUM] Python client set_pixel_map_from_file is broken twice: wrong model field name ('filename' vs 'file') and JSON-parsing the server's plain-text 'OK' response

**Claim.** (a) When called with a string path, the client constructs `PixelMapFromFile(filename=pixel_map_file)`, but the pydantic model's required field is `file` — pydantic v2 (default extra='ignore') raises ValidationError for the missing `file` field on every such call. (b) Even with a correct dict/model, `_put` is invoked with `put_response_model=PixelMapFromFile`, and `_put` executes `put_response_model(**response.json())`; the tpx3app endpoint returns the plain-text body `OK` (not JSON), so `response.json()` raises JSONDecodeError. Every code path of set_pixel_map_from_file therefore fails even when the server succeeds.

**Proof.**

`/Users/janwyzula/PSI/beamline_plugins/superxas_bec/superxas_bec/devices/timepix/timepix_fly_client/timepix_fly_client.py:495-496`
```cpp
elif isinstance(pixel_map_file, str):
                pixel_map_file = PixelMapFromFile(filename=pixel_map_file)
```
`/Users/janwyzula/PSI/beamline_plugins/superxas_bec/superxas_bec/devices/timepix/timepix_fly_client/timepix_fly_interface.py:98-99`
```cpp
type: str = "PixelMapFromFile"
    file: str
```
`/Users/janwyzula/PSI/beamline_plugins/superxas_bec/superxas_bec/devices/timepix/timepix_fly_client/timepix_fly_client.py:389-391`
```cpp
response.raise_for_status()
        if put_response_model is not None:
            return put_response_model(**response.json())
```
`src/include/rest_callbacks.h:505-506`
```cpp
gvars.pix_map = pmap->to_map();
                return "OK";
```

**Impact.** The pixel-map-from-file configuration path is unusable from BEC: with a string argument it fails client-side before any request; with a dict it applies the map on the server but then raises in the client, so the caller believes configuration failed (and the server-side map silently DID change — state divergence between client belief and backend config).

**Fix.** Use `PixelMapFromFile(file=pixel_map_file)` and pass `put_response_model=None` (the endpoint returns plain 'OK'); alternatively change tpx3app to return a JSON body for PUT responses and align the model.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim (client 495-496, interface 98-99, client 389-391, rest_callbacks.h 505-506). (a) PixelMapFromFile requires field 'file'; passing filename= under pydantic v2 default extra='ignore' raises ValidationError for the missing required field — every string-path call fails client-side. (b) _put is called with put_response_model=PixelMapFromFile and executes put_response_model(**response.json()); the server returns plain-text 'OK' (line 506), so response.json() raises JSONDecodeError after the server has already applied the map — confirmed state divergence. Both defects hold; medium </sub>

### TPX-029 · [MEDIUM] WebSocket opcode dispatch uses bitwise AND instead of masked equality: TEXT and CLOSE frames are answered with PONG, close handshake never completes

**Claim.** `if (flags & WebSocket::FRAME_OP_PING)` tests the raw header byte against 0x09 with bitwise AND: a TEXT frame (opcode 0x01, 0x01&0x09!=0), a CLOSE frame (0x08, 0x08&0x09!=0) and a PONG (0x0A) all enter the PING branch, so the server replies with a PONG to every text and close frame; the CLOSE branch (line 180) and the echo branch (183) are unreachable for real frames. A client performing a clean WebSocket close never receives the mandated close reply — the websockets library then waits out close_timeout (default 10 s) and aborts the TCP connection — and the handler thread lingers occupying the WS slot and a server thread until the TCP teardown propagates.

**Proof.**

`src/include/rest_callbacks.h:176-183`
```cpp
if (flags & WebSocket::FRAME_OP_PING) {
                        // Respond to PING with PONG
                        ws->sendFrame(buffer, n, WebSocket::FRAME_FLAG_FIN | WebSocket::FRAME_OP_PONG);
                        logger << "websocket: ping->pong" << log_debug;
                    } else if (n == 0 || (flags & WebSocket::FRAME_OP_CLOSE)) {
                        logger << "websocket: closed" << log_debug;
                        break; // client closed connection
                    } else if ((n > 0) && (n < buf_sz)) { // echo message for tests
```
`src/include/rest_callbacks.h:184-186`
```cpp
ws->sendFrame(buffer, n, WebSocket::FRAME_TEXT);
                        buffer[n] = 0;
                        logger << "websocket: echo \"" << buffer << '"' << log_info;
```

**Impact.** Client shutdown stalls ~10 s per close; the stale handler occupies the single WS slot longer, widening the window for the slot-stealing/UAF races; the RFC6455 close handshake is violated (interop risk with stricter clients); the echo test feature silently does not work.

**Fix.** Extract the opcode with `int op = flags & WebSocket::FRAME_OP_BITMASK;` and dispatch with equality: `if (op == WebSocket::FRAME_OP_PING) ... else if (n == 0 || op == WebSocket::FRAME_OP_CLOSE) { send close reply via ws->shutdown(); break; } else if (op == WebSocket::FRAME_OP_TEXT) ...`.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (176-183, 184-186). Poco opcodes: PING=0x09, TEXT=0x01, CLOSE=0x08, PONG=0x0A, FIN=0x80. A final TEXT frame has flags 0x81; 0x81&0x09!=0 -> PING branch; CLOSE 0x88&0x09!=0 -> PING branch; PONG likewise. So every text/close frame gets a PONG reply and the equality-based CLOSE/echo branches are unreachable for actual frames (CLOSE is only detected indirectly via n==0 after TCP FIN). A websockets-library client performing a clean close never receives the close reply, waits out close_timeout (default 10 s), then aborts TCP, during which the handler occupies the single WS slot and a</sub>

### TPX-030 · [MEDIUM] No authentication on any control endpoint, including /?kill=true which is an immediate std::exit, and no request size limits

**Claim.** Every state-changing endpoint (stop, restart, stop_collect, start, kill, pixel-map PUT, other-config PUT, pixel-map-from-file PUT — the latter also reads an arbitrary server-side file path supplied by the caller) is reachable with zero authentication or origin checking; /?kill=true calls std::exit(EXIT_FAILURE) directly from the HTTP worker thread (no orderly pipeline shutdown, buffered histogram data lost). The control address is configurable (--control) and in beamline deployment must be network-reachable for BEC, so any host on the control network can kill or reconfigure the DAQ mid-scan. PUT bodies are parsed with no size cap (Poco JSON parser buffers the stream), enabling trivial memory-exhaustion.

**Proof.**

`src/include/rest_callbacks.h:370-374`
```cpp
global::instance->get_callbacks["/?kill"] = [](const std::string& val) -> std::string {
            if (val == "true")
                std::exit(EXIT_FAILURE);
            throw Poco::DataFormatException("only 'true' is accepted as 'kill' value");
        };
```
`src/include/rest_callbacks.h:496-502`
```cpp
global::instance->put_callbacks["/pixel-map-from-file"] = [](Poco::JSON::Object::Ptr obj) -> std::string {
                auto lock = global::configLock();
                auto& gvars = *global::instance;
                if (gvars.state != "config")
                    throw Poco::RuntimeException("not in config state");
                std::string path{obj->getValue<std::string>("file")};
```
`src/main.cpp:448-450`
```cpp
} else if (name == "control") {
                try {
                    gvars.controlAddress = SocketAddress(value);
```

**Impact.** Any process with network reach to port 8452 (misconfigured monitoring, a second experiment's software, a compromised host on the beamline LAN) can terminate the DAQ instantly, silently retarget output_uri, or swap the pixel map between scans — undetectable data corruption or scan failure.

**Fix.** Require a shared-secret header (e.g. X-Auth-Token checked in RestHandler::handleRequest before dispatch), keep the default bind on loopback and document SSH-tunnel/reverse-proxy for remote BEC, remove or gate /?kill behind the token, and enforce a Content-Length cap on PUT bodies.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim; note the second proof's quoted text actually spans rest_callbacks.h 496-501 (line 502 is the following std::ifstream line) — trivial end-bound over-count, start line exact. main.cpp 448-450 exact. Re-verified RestHandler::handleRequest (65-124): dispatch is purely path-based with zero auth/origin checks; /?kill calls std::exit(EXIT_FAILURE) on the HTTP worker thread; /pixel-map-from-file opens an arbitrary caller-supplied server-side path; PUT bodies go straight into Poco's JSON parser with no size cap, and --control makes the bind address arbitrary. Confirmed at medium severi</sub>

### TPX-031 · [MEDIUM] global::state (std::string_view) is written by the main thread and read by REST threads without a common lock: data race / torn reads

**Claim.** StateHandler::set_state writes `global::instance->state` under ws_mutex (server mode), but readers do not take ws_mutex: the /state GET (line 403), the /?start config check (line 429), and the config-PUT state checks (lines 478, 499, 546, under configLock — a different mutex). std::string_view assignment is a non-atomic pointer+size pair, so concurrent read/write is UB; a torn read can pair one state's pointer with another's length (e.g. "init" pointer with "await_connection" length: out-of-bounds read, garbage in the /state JSON, or a wrong state-check outcome). global.h itself flags this: 'TODOD: protect with lock, if necessary'.

**Proof.**

`src/include/global.h:114`
```cpp
std::string_view state{init};                                                //!< program state (TODOD: protect with lock, if necessary)
```
`src/include/rest_callbacks.h:401-404`
```cpp
global::instance->get_callbacks["/state"] = []([[maybe_unused]] const std::string& val) -> std::string {
            std::ostringstream oss;
            oss << R"({"type":"ProgramState","state":")" << global::instance->state << R"("})";
            return oss.str();
```
`src/include/rest_callbacks.h:428-431`
```cpp
auto& gvars = *global::instance;
                    if (gvars.state != "config")
                        throw Poco::RuntimeException("not in config state");
                    gvars.start = true;
```

**Impact.** The Python client's REST poll fallback consumes /state continuously (1 Hz), so the racy read executes constantly during scans: risk of a malformed /state JSON that fails the client's pydantic Literal validation (breaking the fallback exactly when it is needed), or of /?start being accepted/rejected on a stale/torn state.

**Fix.** Replace the string_view with std::atomic<StateEnum> (an enum of the seven states); render the string on demand from the enum. All checks and set_state compare/exchange the atomic; keep ws_mutex only for the WebSocket send.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (global.h 114 including the 'TODOD' comment; rest_callbacks.h 401-404, 428-431). set_state writes state under ws_mutex (server mode); the /state GET and /?start callbacks read it with no lock at all, and the config PUT checks (478, 499, 546) hold configLock — a different mutex — so no reader synchronizes with the writer. std::string_view assignment is a non-atomic pointer+size pair: a formal data race (UB) with plausible tearing (mixed pointer/length across the seven constexpr literals). Grep confirms no other lock guards these reads. Confirmed; medium severity reasonable.</sub>

### TPX-032 · [MEDIUM] TOCTOU between REST commands and main loop: stop_collect can be silently erased, and config PUTs can slip past the state check into a starting scan

**Claim.** (a) Main unconditionally clears stop_collect (`gvars.stop_collect = false;`, line 703) after leaving the start-wait; a /?stop_collect arriving between the client's /?start and main reaching line 703 is lost — the scan proceeds and, if SERVAL never connects (the aborting client won't trigger it), the backend sits in await_connection with the abort already consumed. (b) The config PUT handlers verify `state != "config"` under configLock, but main transitions config->setup (line 704) WITHOUT taking configLock, so a PUT can pass the check concurrently with the transition and then mutate output_uri/save_interval/time_roi/pix_map while processing::init/setup reads them without configLock.

**Proof.**

`src/main.cpp:703-704`
```cpp
gvars.stop_collect = false;
                set_state(global::setup);
```
`src/include/rest_callbacks.h:543-547`
```cpp
global::instance->put_callbacks[rest_config] = [](Poco::JSON::Object::Ptr obj) -> std::string {
                auto lock = global::configLock();
                auto& gvars = *global::instance;
                if (gvars.state != "config")
                    throw Poco::RuntimeException("not in config state");
```
`src/main.cpp:706-709`
```cpp
try {
                    if (!copy_mode) {
                        processing::init();
                        analysisPtr->Reset();
```

**Impact.** A rapid start-then-abort sequence (user cancels a just-triggered scan — routine at a beamline) can strand the backend in await_connection with the abort lost; a config PUT racing a scan start can produce a scan running with a half-applied configuration (e.g. new time_roi with old output_uri) — corrupted/mis-attributed XES data.

**Fix.** Drive the scan lifecycle through one mutex-protected state machine: main takes configLock for the config->setup transition and for clearing stop_collect, and the clear must be conditional (compare-exchange against the value observed when start was accepted, or use a monotonically increasing abort generation counter instead of a bool).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp 703-704, 706-709; rest_callbacks.h 543-547). (a) stop_collect is unconditionally cleared at 703 with no synchronization against the REST setter; I checked the potential mitigation — the /?stop_collect handler also calls dataHandlerPtr->stopNow(), which latches databuf.stop_flag — but that flag is only consulted once collection runs, and it never breaks the await_connection poll (which re-checks the now-cleared gvars.stop_collect), so the finding's exact scenario (abort consumed, backend stranded in await_connection when SERVAL never connects) holds. (b) grep confirms </sub>

### TPX-033 · [MEDIUM] asi::server::checkSession can null the HTTP session; the next SERVAL call dereferences a null unique_ptr (no reconnect logic exists)

**Claim.** checkSession calls `clientSession.reset()` when a response stream has more than 32 bytes of unread trailing data. connect() (the only place that creates the session) is called exactly once at startup (main.cpp:642), so after such a reset every subsequent call goes through `clientSession->sendRequest(request)` on an empty unique_ptr — undefined behavior, in practice a segfault of the main thread. Trigger: any SERVAL 200-response whose body is not fully consumed by the JSON parse/rdbuf (e.g. a SERVAL version appending trailing content, or a proxy adding data).

**Proof.**

`src/include/asi_server.h:110-114`
```cpp
in.read(buf, bufSize);
            if (! in.eof()) {
                logger << "session reset" << log_debug;
                clientSession.reset();
            }
```
`src/include/asi_server.h:69-72`
```cpp
auto request = HTTPRequest{HTTPRequest::HTTP_GET, getUri(requestString)};
                logger << request.getMethod() << " " << request.getURI() << log_debug;
                clientSession->sendRequest(request);
                return clientSession->receiveResponse(response);
```
`src/main.cpp:641-642`
```cpp
auto serval = asi::server(logger);
            serval.connect();
```

**Impact.** One oversized/trailing SERVAL response crashes tpx3app on the next configure_raw_destination (i.e., at the start of the next scan) — hard process death instead of an except state, with no diagnostic for the client.

**Fix.** Replace `clientSession.reset()` with a reconnect: `clientSession.reset(new HTTPClientSession{global::instance->serverAddress});` (matching connect()), or null-check the session at the top of serverGet/serverPut and lazily reconnect.

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (asi_server.h 110-114, 69-72; main.cpp 641-642). connect() is the only place clientSession is created and is called exactly once; after checkSession's clientSession.reset() every serverGet/serverPut dereferences a null unique_ptr (UB, not caught by the surrounding Poco::Exception catch). One reachability caveat consistent with the finding's own medium confidence: Poco::JSON::Parser::parse(istream) and the rdbuf() logging paths generally consume the response to EOF, so the >32-byte-trailing trigger may be rare in practice — but the structural defect (reset with no null-check/rec</sub>

### TPX-034 · [MEDIUM] SERVAL HTTP client has no explicit timeouts, and all SERVAL interrogation runs before the REST service starts

**Claim.** The HTTPClientSession is constructed with no setTimeout/setKeepAlive configuration, so every SERVAL call relies on Poco's default 60 s socket timeout, executed synchronously on the main thread. At startup, four SERVAL round-trips (log_dashboard, log_config, read_info, read_layout) are performed BEFORE rest::start_service opens port 8452; if SERVAL is down or black-holed, tpx3app blocks up to 60 s per call and then exits — the BEC client only ever observes connection refused on 8452 with no way to query state or last-error. Mid-loop, a hung SERVAL blocks the main thread in configure_raw_destination for up to 60 s during which stop/stop_collect flags are set but not acted upon.

**Proof.**

`src/include/asi_server.h:234-237`
```cpp
const auto& gvars = *global::instance;
            logger << "connecting to ASI server at " << gvars.serverAddress.toString() << log_notice;
            clientSession.reset(new HTTPClientSession{gvars.serverAddress});
```
`src/main.cpp:647-657`
```cpp
serval.log_dashboard();
            serval.log_config();
            serval.read_info();

            detector_layout& layout = gvars.layout;
            serval.read_layout(layout);

            // ----------------------- create data pipeline -----------------------

            rest::init_callbacks();
            auto restService = rest::start_service(logger, gvars.controlAddress);
```

**Impact.** SERVAL unavailability at tpx3app launch (common after power cycles: startup ordering) makes tpx3app die silently from BEC's perspective; scans fail with opaque 'connection refused' instead of a diagnosable state. Abort latency during a hung SERVAL is up to 60 s.

**Fix.** Call clientSession->setTimeout(Poco::Timespan(5, 0)) (or separate connect/send/receive timeouts) after creation; start the REST service (with state 'init' and meaningful last-error) BEFORE contacting SERVAL, and convert startup SERVAL failure into the except state instead of process exit in server mode.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (asi_server.h 234-236 — cited range 234-237 merely includes the closing brace line; main.cpp 647-657 exact). grep confirms no setTimeout/setKeepAlive anywhere in src/, so Poco's HTTP_DEFAULT_TIMEOUT (60 s) applies to every synchronous main-thread SERVAL call. Startup ordering verified: log_dashboard/log_config/read_info/read_layout (647-652) all precede rest::start_service (657); a failure there propagates out of Tpx3App::main, Application::run logs it, and the process exits with port 8452 never opened — BEC sees only connection refused. Mid-loop, configure_raw_destination (717</sub>

### TPX-035 · [MEDIUM] Data port 8451 accepts the first connection from ANY peer as the detector stream (single accept per scan, no source validation)

**Claim.** After configure_raw_destination, the code polls and accepts exactly one connection per scan and treats it as SERVAL's raw event stream; senderAddress is only logged, never validated against gvars.serverAddress. Any other process connecting to 8451 during await_connection (port scanner, monitoring probe, misconfigured client) consumes the single accept slot: collect starts on the bogus connection, hits the 300 ms receive timeout / garbage data, and the scan fails with except, while SERVAL's genuine connection is left unaccepted in the backlog.

**Proof.**

`src/main.cpp:754`
```cpp
StreamSocket dataStream = serverSocket->acceptConnection(senderAddress);
```
`src/main.cpp:773`
```cpp
logger << "connection from " << senderAddress.toString() << log_info;
```

**Impact.** Routine network scanning on the beamline LAN can nondeterministically kill scans (same 'single-slot resource' pattern as the WS incident, applied to the data plane); an adversarial peer could inject a fabricated event stream that is histogrammed as real data.

**Fix.** Loop on acceptConnection until senderAddress.host() == gvars.serverAddress.host() (closing non-matching peers), or at minimum reject peers not matching the configured SERVAL host; log rejected peers at warning level.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp 754, 773). Exactly one acceptConnection per scan iteration; senderAddress is only logged (773, and only on the non-copy path, after collect has already begun) — nothing anywhere compares it to gvars.serverAddress or closes non-matching peers. A stray connection during await_connection consumes the accept, state moves to collect on the bogus stream, and the 300 ms receive timeout (global::collect_timeout, dataStream.setReceiveTimeout at 757) fails the scan while SERVAL's real connection sits in the backlog. Confirmed; medium severity fits the trusted-LAN context.</sub>

### TPX-036 · [MEDIUM] Listener rebind pattern: new ServerSocket bound while old still listening; reuse flags applied after bind; SO_REUSEPORT silently enabled on both 8451 and 8452

**Claim.** Each scan iteration executes `serverSocket.reset(new ServerSocket{gvars.clientAddress})`: the new socket is constructed (bind+listen) BEFORE the old one is destroyed, and the subsequent setReuseAddress/setReusePort calls run after bind, where they have no effect on the completed bind. This only works because Poco's two-arg `SocketImpl::bind(address, reuseAddress)` delegates to `bind(address, reuseAddress, reuseAddress)` (verified in Poco 1.13.3 source, the version in the Ubuntu 24.04 container images), i.e. SO_REUSEPORT is silently set before bind on every ServerSocket — including the REST listener on 8452. Consequences: (1) during the reset there are briefly TWO listeners on 8451 and the kernel load-balances SYNs between them, so a SERVAL connect can land in the dying socket's accept queue and be reset; (2) SO_REUSEPORT removes the EADDRINUSE protection, so a second tpx3app instance run by the same user (e.g. started with --pid-file=none, which disables the lockfile) binds the SAME 8452/8451 ports successfully and REST/WS/data connections are randomly distributed between the two instances — silent request stealing, the same failure family as the WS incident. Assumption: Poco >= 1.8 (delegation verified for 1.13.3); on older Poco with reusePort=false the rebind at line 713 would instead fail with EADDRINUSE, wedging every scan after the first in an except loop.

**Proof.**

`src/main.cpp:713-715`
```cpp
serverSocket.reset(new ServerSocket{gvars.clientAddress});
                    serverSocket->setReuseAddress(true);
                    serverSocket->setReusePort(true);
```
`src/include/rest_callbacks.h:273-275`
```cpp
inline RestService(Logger& logger, const SocketAddress& listen_to)
            : server(new RestHandlerFactory(logger), ServerSocket(listen_to), http_params)
        {}
```
`container/tpx3app-container.docker:2-14`
```cpp
FROM ubuntu:24.04 AS builder
...
RUN apt-get update && apt-get install -y \
...
    libpoco-dev \
```

**Impact.** A duplicate tpx3app instance (debug session, stale container, lockfile bypassed) shares the control and data ports without any error: BEC commands and the SERVAL stream randomly hit either instance — extremely hard-to-diagnose intermittent scan failures. The dual-listener window can also drop SERVAL's connect, stranding a scan in await_connection.

**Fix.** Close the old listener before binding the new one (`serverSocket.reset(); serverSocket.reset(new ServerSocket{...});`) or bind once outside the loop and keep the listener for the process lifetime; construct listeners via the explicit-bind path with reusePort=false (ServerSocket() + bind(addr, true, false) + listen()) so a duplicate instance fails fast with EADDRINUSE; delete the ineffective post-bind setReuse* calls.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: medium.</sub>

<sub>Verifier: Mechanics independently verified against actual Poco sources: ServerSocket(const SocketAddress&) calls impl()->bind(address, true) then listen (poco-1.11.0-release ServerSocket.cpp), and SocketImpl::bind(addr, reuseAddress) delegates to bind(addr, reuseAddress, reuseAddress) — i.e. SO_REUSEPORT set pre-bind — in both poco-1.11.0-release and 1.12.5p2 SocketImpl.cpp. serverSocket.reset(new ServerSocket{...}) constructs/binds the new listener before destroying the old (dual-listener window with kernel SYN load-balancing), the post-bind setReuseAddress/setReusePort calls (714-715) cannot affect th</sub>

### TPX-037 · [LOW] PUT Content-Type checked with strict string equality: 'application/json; charset=utf-8' is rejected

**Claim.** handleRequest compares the raw Content-Type header with `!=` against "application/json"; any client that appends parameters (charset, boundary) — e.g. aiohttp, many HTTP libraries and proxies by default — gets a 400 'PUT only allowed with JSON content' despite sending valid JSON. The current Python client (requests with json=) happens to send the bare type, so the fragility is latent until the client library or a proxy changes.

**Proof.**

`src/include/rest_callbacks.h:72-73`
```cpp
if (request.getContentType() != "application/json")
                        throw Poco::DataFormatException{"PUT only allowed with JSON content"};
```

**Impact.** Config PUTs (pixel-map, other-config) break as soon as any intermediary or alternate client adds a charset parameter — a confusing 400 during commissioning.

**Fix.** Parse with Poco::Net::MediaType mt(request.getContentType()) and check `mt.matches("application", "json")` instead of string equality.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (rest_callbacks.h 72-73). Poco's HTTPMessage::getContentType() returns the raw Content-Type header value including any parameters, so 'application/json; charset=utf-8' fails the != comparison and yields a 400 despite valid JSON. The current Python client (requests with json=) sends the bare media type, so the breakage is latent exactly as described. Confirmed; low severity appropriate.</sub>


## XES data path & output writers

*20 findings: 1 critical, 7 high, 7 medium, 5 low*

### TPX-038 · [CRITICAL] Writer lifecycle handshake desyncs permanently after one skipped await(): Reset() races with active writer thread (UAF, cross-scan corruption)

**Claim.** xes::Manager::writer_finished is a latched one-shot signal that Manager::Reset() never clears and Reset() has no guard that the writer thread is idle. If main's flow ever skips analysis.await() for one measurement (any exception thrown between analysis.run_async() and analysis.await(), e.g. dataStream.shutdown() throwing NetException inside dataHandler.await() when the peer sent RST after an abort - the exact abort scenario of known incident #2), the writer thread still sends writer_finished, which stays latched. Every subsequent measurement's await() then consumes the PREVIOUS run's signal and returns immediately while the current writer thread is still writing; the next Reset() then replaces the `writer` unique_ptr and mutates pool/ready/empty/fill with no locks held while the writer thread dereferences them. Assumption stated: the trigger requires an exception escaping between run_async and await in main.cpp; the latch/no-guard defect itself is unconditional in the quoted code.

**Proof.**

`src/include/xes_data_manager.h:410-425`
```cpp
inline void Reset()
{
    const auto& gvars = *global::instance;
    {
        const std::string& uri = gvars.output_uri;
        writer = xes::Writer::from_uri(uri);
```
`src/include/xes_data_manager.h:460`
```cpp
writer_finished.wait_reset();
```
`src/include/thread_signal.h:102-108`
```cpp
inline void wait_reset() noexcept
{
    std::unique_lock lck{lock};
    while (!signal)
        cond.wait(lck);
    signal = false;
}
```
`src/main.cpp:776-779`
```cpp
analysis.run_async();
dataHandler.run_async();
dataHandler.await();
analysis.await();
```
`src/include/data_handler.h:541-547`
```cpp
inline void await()
{
    iobuf::resetter reset(databuf);
    reader_finished.wait_reset();
    dataStream.shutdown();
    dataStream.close();
    analysis_finished.wait_reset();
```

**Impact.** One aborted/errored measurement poisons all later scans: await() returns before the writer finishes, Reset() destroys the Writer object the writer thread is using (use-after-free of the TCP socket / Redis publisher), and ModuleData::reset() clears queues the writer is concurrently popping - crash or silent corruption; with Redis output the still-running writer publishes the old scan's frames under the NEW scan-id after setParam. This mirrors the incident class where a mid-replay abort broke every subsequent scan.

**Fix.** In Manager::Reset(): call writer_finished.reset() and start_writer.reset(), and add a hard guard that the writer thread is idle (e.g. an atomic 'writer_active' flag set/cleared by the writer loop; if active, send stopWriter=true, wake all module CVs, and wait for writer_finished before touching writer/module_data). In main.cpp, wrap dataHandler.await() so that analysis.await() (or an analysis.abort()) is always executed (try/catch or scope guard). Make DataHandler::await() tolerate shutdown()/close() throwing on a dead socket.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All five quotes are verbatim at the cited lines. Manager::Reset() (xes_data_manager.h:410-425) never calls writer_finished.reset()/start_writer.reset() and has no writer-idle guard; single<no_shutdown>::wait_reset (thread_signal.h:102-108) consumes a latched flag. Trigger path verified: main.cpp:776-779 has no try/scope-guard around dataHandler.await(), and data_handler.h:545 calls dataStream.shutdown() (Poco throws NetException on a dead socket) BEFORE analysis_finished.wait_reset(), so an RST-after-abort skips analysis.await() while the writer thread of that run still completes and latches w</sub>

### TPX-039 · [HIGH] ready queue is std::priority_queue<Data*> ordered by pointer address - the std::less<xes::Data> specialization is never used; frames pop out of period order and different periods get merged

**Claim.** ModuleData::ready is declared std::priority_queue<Data*>, so the comparator is std::less<Data*> (raw pointer comparison). The std::less<xes::Data> specialization written for period ordering in xes_data.h applies to Data values, not Data pointers, and is dead code. Whenever a module has more than one histogram queued (writer backlog, e.g. a slow output consumer), pop order is by heap address, i.e. arbitrary relative to period. The writer's per-sweep merge then aggregates whatever was popped from each chip into one frame and labels it with the max period seen, so partial histograms of different periods are summed into a single mislabeled output frame.

**Proof.**

`src/include/xes_data_manager.h:61`
```cpp
std::priority_queue<Data*> ready;   //!< Histograms ready for writer thread
```
`src/include/xes_data.h:123-124`
```cpp
template<>
struct std::less<xes::Data> final {
```
`src/include/xes_data_manager.h:199-203`
```cpp
if (data != nullptr) {
    if (d->period > data->period)
        data->period = d->period;
    assert(data->period != 0);
    data->addResetRhs(*d);
```

**Impact.** Under any writer backlog, XesData frames arrive with non-monotonic periods and with counts from different save intervals mixed into one frame - silent scientific data corruption of the time-vs-energy histograms that is essentially undetectable downstream.

**Fix.** Use an explicit comparator on the pointed-to period: `struct ByPeriodDesc { bool operator()(const Data* a, const Data* b) const noexcept { return a->period > b->period; } }; std::priority_queue<Data*, std::vector<Data*>, ByPeriodDesc> ready;` so top() is the LOWEST period, and in the writer sweep only merge d into data when d->period == data->period (otherwise push it back / defer to the next sweep).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_manager.h:61, xes_data.h:123-124, xes_data_manager.h:199-203). std::priority_queue<Data*> defaults to std::less<Data*> = raw pointer comparison; grep confirms the std::less<xes::Data> specialization is defined at xes_data.h:124 and referenced nowhere else — dead code. ReturnData pushes in monotonically increasing period order per module (save_point only increments), so a FIFO would be correct, but a pointer-ordered max-heap pops in address order whenever a module's backlog exceeds 1. The writer sweep (lines 174-215) pops one entry per module per pass and merges via ad</sub>

### TPX-040 · [HIGH] ReturnData matches fill entries with '>=' and unconditionally relabels the histogram, without invalidating the cache - later-period data published under an earlier period and concurrently written by the analysis thread

**Claim.** ReturnData(threadNo, period) searches mdata.fill for the first entry with d->period >= period, removes it, then sets data->period = period and pushes it to the ready queue. If the chip had no events in bucket `period` but has a partially filled histogram for the NEXT save interval (realistic for a sparse/low-rate chip that was idle for one save_interval), that later-period histogram is stolen and relabeled to the earlier period. The cache-invalidation above it only fires on an exact match (cached.period == period), so mdata.cache still points at the submitted Data object; the next DataForPeriod cache hit returns it and the analysis thread writes into TDSpectra while the writer thread concurrently aggregates and Reset()s the same object.

**Proof.**

`src/include/xes_data_manager.h:365-377`
```cpp
// clean cache
CacheEntry& cached = mdata.cache;
if (cached.period == period)
    cached.period = none;

// find the histogram data beeing filled up
for (auto& d: mdata.fill) {
    if (d->period >= period) {
```
`src/include/xes_data_manager.h:396-401`
```cpp
// add to ready queue
histo_submitted++;
data->period = period;
{
    std::lock_guard lock{mdata.lock_ready};
    mdata.ready.push(data);
```
`src/include/xes_data_manager.h:303-306`
```cpp
// try cache
CacheEntry& cached = mdata.cache;
if (cached.period == period)
    return *cached.data;
```

**Impact.** Events collected for save interval N+1 are published as interval N (wrong time axis), and an unsynchronized read-modify-write race between the analysis thread and the writer thread on the same float array causes lost or double-counted events and torn frame counters.

**Fix.** Use an exact match (d->period == period) in the fill search; when no exact match exists fall through to the existing empty-histogram path (which correctly submits an empty frame for the gap). Additionally invalidate the cache whenever the cached Data pointer equals the object being submitted (compare cached.data == data, not only cached.period == period).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (quote for '365-377' actually spans 365-372, within the cited range; 396-401 and 303-306 exact). Code re-derived: the fill search uses d->period >= period and erases/relabels (data->period = period at line 398). Scenario is realistic given Analysis bucketing (analysis.h:107-115): a chip with zero events below save_point but events filed under the next bucket (ProcessEvent bumps sp before DataForPeriod) leaves only a later-period entry in fill; PurgePeriod then calls ReturnData(sp), which steals and relabels it. Cache invalidation (lines 366-368) fires only on exact period match</sub>

### TPX-041 · [HIGH] TcpWriter never checks stream state: a dead consumer causes silent, total loss of all subsequent frames (scan 'succeeds')

**Claim.** TcpWriter::write/start/stop stream through Poco::Net::SocketStream and never inspect send.fail()/bad() afterwards. When the consumer dies mid-measurement, Poco's SocketStreamBuf throws NetException inside the streambuf, which std::ostream catches internally and converts to badbit (default exception mask is goodbit, so nothing propagates). Every subsequent write() becomes a no-op that still executes data_counter++ and updates last_period, so the writer thread believes it is succeeding. FileWriter, by contrast, checks OutFile.fail() and throws. Assumption stated: standard iostream behavior of swallowing streambuf exceptions and setting badbit with the default exception mask.

**Proof.**

`src/xes_data_writer.cpp:126-136`
```cpp
send << R"({"type":"XesData","period":)" << data.period
     << R"(,"TDSpectra":[)" << TDSpectra[0];
for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
    send << ',' << TDSpectra[i];
send << R"(],"totalEvents":)" << data.Total
     << R"(,"beforeROI":)" << data.BeforeRoi
     << R"(,"afterROI":)" << data.AfterRoi
     << "}\n" << std::flush;
data_counter++;
```
`src/xes_data_writer.cpp:58-59`
```cpp
if (OutFile.fail())
    throw std::ios_base::failure("xes::FileWriter::write failed");
```

**Impact.** If the downstream tcp: consumer aborts mid-scan (the same consumer-abort environment as known incident #2), every remaining XesData frame and the EndFrame are silently discarded; the scan completes with no error (data_counter > 0 so even the 'no event data was collected' check passes). Complete, undetected data loss.

**Fix.** After each write/start/stop, check the stream: `if (!send) throw Poco::WriteFileException("xes::TcpWriter: send failed to " + dest());` (or enable send.exceptions(std::ios::badbit|std::ios::failbit)). This converts consumer death into the existing writer-exception path, which sets global error state and aborts the measurement visibly.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp write body at 126-134 within cited 126-136; FileWriter contrast at 58-59 exact). TcpWriter::write/start/stop (lines 120-199) construct a fresh Poco::Net::SocketStream per call and never inspect fail()/bad(); no exceptions() mask is set. Standard iostream behavior holds: streambuf exceptions (Poco StreamSocket::sendBytes throws NetException on EPIPE/ECONNRESET) are caught by formatted inserters and flush(), which set badbit and rethrow only if badbit is in exceptions() (default goodbit) — so every write silently no-ops after consumer death while data_counter</sub>

### TPX-042 · [HIGH] Blocking TCP send with no timeout can wedge the writer thread forever; await() and Manager destructor use an uninterruptible wait, hanging the whole DAQ

**Claim.** TcpWriter's socket gets no send timeout (constructor only connects), so a stalled-but-alive consumer (full socket buffer) blocks the writer thread indefinitely inside SocketStream flush. writer_finished is then never sent, and Manager::await() blocks in thread_signal::single<no_shutdown>::wait_reset(), which loops on a plain condition variable with no shutdown escape and no timeout. Manager::~Manager()/shutdown() joins writerThread, which is also stuck. There is no external mechanism to set stopWriter during a run (it is only ever set by the writer thread's own exception path), so REST stop cannot recover.

**Proof.**

`src/xes_data_writer.cpp:86-92`
```cpp
inline explicit TcpWriter(const std::string& address)
{
    try {
        dataReceiver.connect(Poco::Net::SocketAddress{address});
```
`src/include/xes_data_manager.h:119`
```cpp
thread_signal::single<thread_signal::no_shutdown> writer_finished;  //!< Reader finished sigal
```
`src/include/xes_data_manager.h:285-290`
```cpp
inline ~Manager()
{
    writer_shutdown.send();
    if (writerThread.joinable())
        writerThread.join();
}
```

**Impact.** A slow or stalled output consumer permanently hangs analysis.await() in the main server loop and later the shutdown path; the beamline DAQ must be killed manually and the in-flight scan is lost.

**Fix.** Set a send timeout on the socket (dataReceiver.setSendTimeout(Poco::Timespan(...)) in the TcpWriter constructor) so a stalled consumer surfaces as a Poco::TimeoutException through the existing writer-exception path; additionally give writer_finished a shutdown-interruptible wait (single<with_shutdown> bound to writer_shutdown) or a wait_for loop that checks a stop flag.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp:86-89 within cited 86-92; xes_data_manager.h:119 and 285-290 exact). Grep confirms no setSendTimeout anywhere in src/ (only receive timeouts at main.cpp:757 and rest_callbacks.h:160), so a stalled consumer with a full socket buffer blocks Poco's blocking sendBytes indefinitely inside writer->write(). writer_finished is single<no_shutdown>, whose wait_reset (thread_signal.h:102-108) has no timeout or shutdown escape, so Manager::await() (line 460) hangs; ~Manager/shutdown() join the stuck thread. Grep confirms stopWriter is assigned true only at xes_data_man</sub>

### TPX-043 · [HIGH] RedisPublisher::reconnect() calls connect() without disconnect(): after a Redis restart, every reconnect attempt throws and the cached broken client makes redis output permanently unusable until process restart

**Claim.** When checkConnection()'s PING fails (e.g. Redis server restarted), RedisWriter's constructor calls publisherCache->reconnect(), which invokes redis_client.connect(host_port) on a client whose streams are still allocated. Poco::Redis::Client::connect() requires a disconnected client (it poco_asserts its input/output streams are null - assumption about Poco internals, valid for shipped Poco versions), so reconnect throws instead of reconnecting. Because the failed RedisPublisher stays in the static cache and hasAddress() still matches, every subsequent Manager::Reset() takes the same reconnect path and throws again.

**Proof.**

`src/xes_data_writer.cpp:411-414`
```cpp
inline void reconnect()
{
    redis_client.connect(host_port);
}
```
`src/xes_data_writer.cpp:446-451`
```cpp
} else {
    if (!publisherCache->checkConnection()) {
        log << "redis writer: reconnecting to " << host_port << log_debug;
        publisherCache->reconnect();
    }
}
```

**Impact.** A single Redis outage or restart bricks the redis:// output path for the lifetime of the tpx3app process: every scan start fails in Reset() until an operator restarts the DAQ application.

**Fix.** In reconnect(), disconnect first: `try { redis_client.disconnect(); } catch (...) {} redis_client.connect(host_port);` - or more simply, in RedisWriter's constructor drop the cached object on a failed ping and construct a fresh RedisPublisher (publisherCache.reset(new RedisPublisher{host_port})).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp:411-414 and 446-451). Poco::Redis::Client::connect() does poco_assert(!_input)/poco_assert(!_output) (active in release builds; throws AssertionViolationException) — after a failed PING the streams are still allocated (only disconnect() nulls them; checkConnection() at 395-406 does not disconnect on failure), so reconnect() throws out of RedisWriter's constructor, i.e. out of Manager::Reset() at scan start. The broken publisher remains in publisherCache (line 417) and hasAddress() (386-389) still matches, so every subsequent Reset with the same URI repeats </sub>

### TPX-044 · [HIGH] SetTimeROI validation uses && instead of ||: zero/negative TRoiStep or TRoiN accepted, producing empty TDSpectra and out-of-bounds TDSpectra[0] access in TCP/Redis writers, or huge wrapped u64 values

**Claim.** TimeRoi::SetTimeROI only throws when BOTH tRoiStep <= 0 AND tRoiN <= 0, contradicting its own message 'TRoiStep and TRoiN must be positive'. TRoiN=0 with a positive step is accepted (TDSpectra sized TRoiN*npoints = 0, then TcpWriter::write and RedisPublisher::write unconditionally read TDSpectra[0] - out-of-bounds on an empty vector, UB). A negative tRoiStep or tRoiN alone is also accepted and, assigned to u64 members, wraps to ~1.8e19 (negative TRoiN additionally makes the Data constructor attempt a multi-exabyte allocation). These values arrive from the REST /other-config call, i.e. straight from the Python client.

**Proof.**

`src/include/time_roi.h:37-42`
```cpp
if ((tRoiStep <= 0) && (tRoiN <= 0))
        throw std::invalid_argument("TRoiStep and TRoiN must be positive");

TRoiStart = tRoiStart;
TRoiStep = tRoiStep;
TRoiN = tRoiN;
```
`src/xes_data_writer.cpp:126-127`
```cpp
send << R"({"type":"XesData","period":)" << data.period
     << R"(,"TDSpectra":[)" << TDSpectra[0];
```
`src/include/xes_data.h:45-49`
```cpp
inline explicit Data()
    : TDSpectra(
        global::instance->time_roi.TRoiN * global::instance->pix_map->npoints
      )
{}
```

**Impact.** A single client-side typo in the ROI config (TRoiN: 0 or a negative value) leads to undefined behavior / crash at the first frame write, or to bad_alloc / OOM-killer at Reset - a hard scan failure with a confusing signature.

**Fix.** Change the condition to `if ((tRoiStep <= 0) || (tRoiN <= 0) || (tRoiStart < 0)) throw std::invalid_argument(...)`, and in TcpWriter/RedisPublisher guard the serialization for elements == 0 (emit an empty array) instead of unconditionally reading TDSpectra[0].

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Core claim confirmed: time_roi.h:37-42 quote is verbatim; the && only throws when BOTH are <= 0, contradicting the message. TRoiN=0 with positive step is accepted, Data() (xes_data.h:45-49) then sizes TDSpectra to 0, and TcpWriter (line 127) and RedisPublisher (line 280) unconditionally read TDSpectra[0] — OOB/UB confirmed. Adjustment on the delivery path of NEGATIVE values: the REST /other-config handler (rest_callbacks.h:556-560) extracts with getValue<decltype(TRoiStart)> = getValue<u64>, and Poco's Dynamic::Var conversion of a negative JSON number to an unsigned type throws RangeException </sub>

### TPX-045 · [HIGH] Pixel-map parsers put no upper bound on energy_point; npoints += 1 wraps to 0 for p = UINT_MAX (from_file even accepts '-1'), leading to heap-corrupting OOB writes or OOM

**Claim.** Both from_json and from_file compute pmap.npoints = max(p, npoints) with no upper bound on p, then do `pmap.npoints += 1`. A map containing p = 4294967295 makes npoints wrap to 0, so every Data's TDSpectra is allocated with size TRoiN*0 = 0 while the mapping still contains energy_point = 4294967295; Analysis::Register then writes TDSpectra[TimePoint*0 + 4294967295] - a wild out-of-bounds float write during collection. from_file additionally parses energy points with istream >> unsigned, which accepts '-1' as 4294967295 without setting failbit (standard strtoull wrap semantics - assumption stated). A merely 'large' p (e.g. 1e8) instead causes a TRoiN*npoints*4-byte allocation per pooled histogram - immediate bad_alloc/OOM at Reset. The map is supplied via REST PUT /pixel-map.

**Proof.**

`src/energy_points.cpp:88-89`
```cpp
auto p = pointList->getElement<unsigned>(k);
pmap.npoints = std::max(p, pmap.npoints);
```
`src/energy_points.cpp:96`
```cpp
pmap.npoints += 1;
```
`src/energy_points.cpp:157-159`
```cpp
part.energy_point = parse<unsigned>(s, posN[2+m]);
part.weight = parse<float>(s, posN[2+numEnergyPoints+m]);
pmap.npoints = std::max(pmap.npoints, part.energy_point);
```
`src/include/analysis.h:40`
```cpp
data.TDSpectra[TimePoint * pix_map.npoints + part.energy_point] += part.weight;
```

**Impact.** A malformed pixel map (one bad index, or a '-1' in a generated .inp file) is accepted at config time and later corrupts the heap mid-scan (crash at a distance) or OOM-kills the DAQ at measurement start.

**Fix.** Validate p against a sane configurable ceiling (e.g. reject p >= 65536) in both parsers, reject npoints overflow explicitly, and reject non-finite or absurd weights. In from_file, read into a signed long and range-check before casting to unsigned to catch '-1'.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim (energy_points.cpp:88-89, 96, 157-159; analysis.h:40). npoints is unsigned (pixel_map.h:40, energy_points.h:55): p=4294967295 makes npoints+=1 wrap to 0, Data allocates TRoiN*0=0 floats, and to_map() keeps energy_point=4294967295 in the mapping (weight!=0 filter only), so Register writes TDSpectra[TimePoint*0 + 4294967295] — wild OOB float write mid-scan. from_file's parse<unsigned> uses istream >> unsigned, which per strtoull semantics accepts '-1' as 4294967295 without failbit — assumption verified as standard behavior. Large p causes per-pooled-Data allocations of TRoiN*</sub>

### TPX-046 · [MEDIUM] scanID is spliced into XesData JSON without escaping (unlike Start/EndFrame which use PrintHandler): malformed or adversarial scan-id corrupts every data frame

**Claim.** RedisPublisher::write builds the XesData frame with raw ostream concatenation and embeds the scan string directly inside a JSON string literal, while start() and stop() serialize scanID through Poco::JSON::PrintHandler (which escapes). Any scan-id containing a quote, backslash, or control character (it is taken verbatim from the output_uri query string, set by the REST client) produces invalid JSON for every XesData frame of the scan; the frames become unparseable downstream while StartFrame/EndFrame still parse.

**Proof.**

`src/xes_data_writer.cpp:283-287`
```cpp
oss << R"(],"totalEvents":)" << data.Total
    << R"(,"beforeROI":)" << data.BeforeRoi
    << R"(,"afterROI":)" << data.AfterRoi
    << R"(,"scanID":")" << scan
    << R"("})";
```
`src/xes_data_writer.cpp:323`
```cpp
json.key("scanID"); json.value(scan);
```

**Impact.** All histogram frames of a scan are silently rejected by the JSON parser on the BEC side while control frames parse - a confusing partial data loss keyed to the scan-id content.

**Fix.** Escape scan when embedding (e.g. serialize it once via Poco::JSON at setParam time and store the escaped form), or validate the scan-id at RedisWriter construction (reject characters outside [A-Za-z0-9_.-]).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp:283-287 and 323). RedisPublisher::write concatenates the raw scan string inside a JSON string literal while start() (line 323) and stop() (line 353) escape via Poco::JSON::PrintHandler. scan is taken from the URI query (line 434) — and Poco::URI::getQuery() returns the DECODED query, so %22 etc. become literal quotes/backslashes — producing invalid JSON for every XesData frame while Start/EndFrame still parse. Confirmed as described; medium severity appropriate.</sub>

### TPX-047 · [MEDIUM] output_uri parsing is fragile: redis path/query sliced with unchecked substr (throws std::out_of_range, mis-slices extra params); tcp://host:port form unsupported

**Claim.** RedisWriter's constructor does getPath().substr(1) and getQuery().substr(8) with no verification: a redis URI without a '?scan-id=' query (or without a /channel path) throws std::out_of_range('basic_string::substr') out of Manager::Reset with no hint about the URI; a query not starting with 'scan-id=' or containing extra parameters ('?scan-id=17&foo=1') silently produces a wrong scan string. For tcp, from_uri passes getPathEtc() to SocketAddress, so the natural 'tcp://host:port' spelling yields an empty string and an obscure failure (acknowledged by the in-code TODO); only 'tcp:host:port' works.

**Proof.**

`src/xes_data_writer.cpp:433-434`
```cpp
const std::string key{address.getPath().substr(1)};     // remove leading char in /key
const std::string scan{address.getQuery().substr(8)};   // remove up to = in scan-id=xxxx
```
`src/xes_data_writer.cpp:528-529`
```cpp
} else if (scheme == "tcp") {
    return std::unique_ptr<Writer>{new TcpWriter{destination.getPathEtc()}}; // TODO: change to getAuthority
```

**Impact.** Slightly off output_uri values (a very likely client-side mistake) fail scan start with cryptic 'basic_string::substr: __pos > size' errors or silently attribute frames to a wrong scan-id.

**Fix.** Parse the query with Poco::URI::getQueryParameters() and look up 'scan-id' explicitly, throwing a descriptive Poco::RuntimeException naming the URI when path/scan-id are missing; for tcp, accept both forms (use getAuthority() when non-empty, else getPathEtc()).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp:433-434 and 528-529). getPath() is "" for redis://host:port (no path) → substr(1) throws std::out_of_range; getQuery() is "" without ?scan-id=... → substr(8) throws; both escape from Manager::Reset with the bare libstdc++ message. A query like '?scan-id=17&foo=1' yields scan="17&foo=1" silently. For tcp, Poco parses tcp://host:port with host:port as authority, so getPathEtc() is empty and TcpWriter gets "" (obscure failure); the in-code TODO at line 529 and the error text at line 531 ('use file:filename or tcp:host:port') corroborate that only the no-slash </sub>

### TPX-048 · [MEDIUM] Static process-lifetime RedisPublisher cache is shared mutable state with no synchronization; setParam retroactively changes scan-id/channel under a potentially active writer

**Claim.** publisherCache is an inline static unique_ptr shared by all RedisWriter instances for the process lifetime. RedisWriter's constructor (run on the control thread inside Manager::Reset) may reset it or call setParam(channel, scan) with no locking, while the writer thread of a not-yet-finished previous measurement can still be calling publisherCache->write()/stop() through its own RedisWriter (reachable via the await/Reset desync reported separately). Poco::Redis::Client is not thread-safe, and because frames read `scan` at publish time, setParam retroactively re-tags any still-in-flight publishing with the new scan's ID.

**Proof.**

`src/xes_data_writer.cpp:417`
```cpp
inline static std::unique_ptr<RedisPublisher> publisherCache{nullptr};  //!< Caches REDIS connection object
```
`src/xes_data_writer.cpp:453-454`
```cpp
log << "redis writer: publishing to channel " << key << " with scan id " << scan << log_debug;
publisherCache->setParam(key, scan);
```

**Impact.** Under lifecycle races: corrupted RESP protocol stream or crash inside Poco::Redis::Client, and frames of scan N published with scan N+1's scanID (stale/shifted scan attribution in Redis).

**Fix.** Make the publisher per-RedisWriter (drop the static cache; connection setup cost per scan is negligible), or protect the cache with a mutex and store channel/scan per-RedisWriter instance, passing them as arguments to write/start/stop instead of mutating shared state.

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (xes_data_writer.cpp:417 and 453-454). publisherCache is inline static with no mutex; RedisWriter's constructor (control thread, inside Manager::Reset) can reset it or mutate channel/scan via setParam while a still-running writer thread from a desynced previous measurement calls publisherCache->write()/stop() through its own RedisWriter. RedisPublisher::write reads the scan member at publish time (line 286), so setParam retroactively re-tags in-flight frames. Correctly scoped: under normal sequencing writer.reset(nullptr) happens before writer_finished.send() (xes_data_manager.</sub>

### TPX-049 · [MEDIUM] Histogram pool grows without bound when the writer lags and never shrinks afterwards

**Claim.** DataForPeriod and ReturnData allocate a new pooled histogram (pool.emplace_front) whenever the empty list is exhausted, with no cap and no backpressure; each Data is TRoiN*npoints floats (tens of MB for realistic configs). ModuleData::reset() re-registers every pool node forever, so the high-water mark persists for the process lifetime.

**Proof.**

`src/include/xes_data_manager.h:332-335`
```cpp
// create a new histogram
// NOTE: this operation MUST NOT change memory location of other data
mdata.pool.emplace_front();
data = &mdata.pool.front();
```
`src/include/xes_data_manager.h:94-100`
```cpp
unsigned size = 0u;
for (auto& data : pool) {
    data.Init();    // Pick up ROI changes from global config vars
    data.Reset();
    empty.push_back(&data);
    size++;
}
```

**Impact.** A slow output consumer (or the blocking-send stall reported separately) makes memory grow linearly with scan duration across all chips; long scans can drive the DAQ into swap or the OOM killer, and the memory is never returned between scans.

**Fix.** Cap the per-module pool (e.g. max_xes_data_pool_size); when the cap is hit, block the producer briefly on mdata.write/lock_empty (backpressure) or drop-and-count with an explicit warning counter. Trim pool nodes above min_xes_data_pool_size in ModuleData::reset().

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_manager.h:332-335 and 94-100). Both DataForPeriod (line 334) and ReturnData (line 391) emplace_front a new Data with no cap when empty is exhausted (i.e., whenever the writer lags and ready holds all pooled histograms). ModuleData::reset() re-inits and re-registers every pool node and only grows toward min_xes_data_pool_size (global.h:35, =5) — there is no trim path, so the high-water mark persists for the process lifetime. Each Data is TRoiN*npoints floats (default TRoiN=5000), tens of MB for realistic npoints, per module. Confirmed.</sub>

### TPX-050 · [MEDIUM] Writer error mid-measurement: EndFrame is attempted over the same broken transport and failures are swallowed - downstream consumer never learns the scan ended

**Claim.** When writer->write throws (e.g. Redis PUBLISH on a dropped connection), the catch handler calls writer->stop(error) on the same broken transport and ignores any exception from it, then destroys the writer. There is no fallback delivery of the EndFrame, and all frames since the last successful write are dropped with no buffering or retry.

**Proof.**

`src/include/xes_data_manager.h:240-246`
```cpp
} catch (std::exception& ex) {
    try {
        if (writer)
            writer->stop(std::string("writer: ") + ex.what());
    } catch (...) {}    // ignore exceptions
    logger << "writer thread exception: " << ex.what() << log_fatal;
```

**Impact.** A transient transport failure aborts the whole scan and leaves the BEC-side consumer waiting for an EndFrame that never arrives; it can only detect the failure by polling REST /state for 'except' (the exact coupling that bit incident #4). Any partial-period histogram in flight is lost.

**Fix.** Add a bounded retry (reconnect + resend) for the EndFrame in the error path, and document/guarantee that clients must treat /state=except or the WebSocket 'except' push as an implicit EndFrame. Consider a small bounded retry for data frames before declaring the writer dead.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (xes_data_manager.h:240-245, within cited 240-246). Both catch handlers (240-246 and 247-254) call writer->stop(error) on the same transport that just failed and swallow any exception with catch(...); the writer is then destroyed (line 258) with no retry, buffering, or fallback EndFrame delivery. For TCP the stop() would not even throw (silently badbit, per the TcpWriter finding), and for Redis the PUBLISH on a dropped connection throws and is ignored. Downstream detection is only via global::set_error / REST state. Confirmed.</sub>

### TPX-051 · [MEDIUM] Lost-wakeup race in thread_signal::single<no_shutdown>::send(): predicate set and notify issued without holding the waiter's mutex

**Claim.** single<no_shutdown>::send() sets the atomic flag and calls notify_all() without acquiring the mutex that wait_reset() holds around its predicate check. If send() runs between a waiter's '!signal' evaluation and its cond.wait(), the notification is lost and the waiter sleeps forever (no timeout, no shutdown escape). This is the type used for writer_finished and writer_shutdown; the single<with_shutdown> specialization does it correctly (locks in send). Window is narrow but the pattern executes once per measurement (await) and once at shutdown.

**Proof.**

`src/include/thread_signal.h:90-95`
```cpp
inline void send() noexcept
{
    signal = true;
    for (auto* sig : dep)
        sig->notify_all();
}
```
`src/include/thread_signal.h:160-165`
```cpp
inline void send() noexcept
{
    std::lock_guard lck{lock};
    signal = true;
    base::notify_all();
}
```

**Impact.** Rare but permanent hang of Manager::await() (scan never completes) or of the shutdown/join path - the DAQ appears frozen with no error, requiring a kill. Over thousands of scans at a beamline the window will eventually be hit.

**Fix.** In single<no_shutdown>::send(), take the lock before setting the flag (mirroring single<with_shutdown>::send). For dependent signals notified via the dep list, notify each dependent while holding that dependent's mutex (add a locked notify helper on base).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (thread_signal.h:90-95 and 160-165). Classic lost wakeup confirmed: wait_reset() (102-108) checks !signal under the mutex then releases it inside cond.wait; send() sets the atomic flag and calls notify_all WITHOUT taking that mutex, so it can run entirely between the waiter's predicate check and the wait entry — the notify is lost and the waiter sleeps forever (no timeout, no shutdown escape). The single<with_shutdown> specialization (160-165) locks correctly, proving the intended pattern. Affects writer_finished/writer_shutdown (xes_data_manager.h:118-119) — hanging Manager::a</sub>

### TPX-052 · [MEDIUM] Pixel-map chip-count validation (incident #3 code basis): strict equality with an error message that names neither expected nor actual count; from_file errors lack line context

**Claim.** from_json rejects any map whose 'chips' array length differs from the live detector layout with the bare message 'mismatch with number of chips from detector server' - confirming known incident #3. The message contains neither the expected nor the received count, and the check requires the ASI-server-derived layout, so a map built for a different chip count fails opaquely. In from_file, the invalid-chip/pixel/count messages carry no line number (only the getline-failure path includes one).

**Proof.**

`src/energy_points.cpp:71-72`
```cpp
if (numChips != nchips)
    throw Poco::RuntimeException{"mismatch with number of chips from detector server"};
```
`src/energy_points.cpp:147-149`
```cpp
unsigned k = parse<unsigned>(s, posN[0]);       // chip
if (k >= numChips)
    throw std::invalid_argument("invalid chip number in XESPoints file");
```

**Impact.** Beamline staff cannot tell from the REST error whether the map or the detector layout is wrong or by how much - this already cost real integration time (incident #3).

**Fix.** Include both counts: `throw Poco::RuntimeException{"pixel map has " + std::to_string(nchips) + " chips, detector layout has " + std::to_string(numChips)};` and append `" at line " + std::to_string(line)` to all from_file validation errors.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (energy_points.cpp:71-72 and 147-149). The mismatch message contains neither numChips nor nchips. In from_file, the validation errors at lines 144, 146, 149, and 152 carry no line number; only the getline-failure path (line 130) includes 'at line N'. Code claims fully verified (the incident-history framing is context I cannot verify from the repo, but the code basis is accurate). Medium severity for a diagnosability defect with operational history is acceptable.</sub>

### TPX-053 · [LOW] Unguarded writer->stop() on the external-stop path while every other use is null-checked

**Claim.** The writer thread's external-stop branch dereferences `writer` without the `if (writer)` guard used at every other call site (lines 165, 219, 225, 242). `writer` is nullptr after each measurement (writer.reset(nullptr) at regular/exception stop) until the next Reset() assigns it; the branch is reachable with a null writer only through the lifecycle races reported separately (stale stopWriter combined with a skipped Reset), but when reached it is an instant nullptr dereference.

**Proof.**

`src/include/xes_data_manager.h:191-194`
```cpp
if (__builtin_expect(stopWriter, false)) {
    writer->stop("writer: external stop");
    goto regular_stop;
}
```

**Impact.** Segfault of the DAQ process instead of a clean error if the external-stop path is entered without a live writer.

**Fix.** Guard it: `if (writer) writer->stop("writer: external stop");`.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (xes_data_manager.h:191-194). Line 192 dereferences writer with no guard while lines 165, 219, 225, 242, and 249 all use 'if (writer)'. writer is nulled at both stop paths (258, 265) and only reassigned in Reset() (415), so the branch with a null writer requires stale stopWriter plus a skipped/failed Reset (e.g. from_uri throwing at 415 leaves writer null and stopWriter true) — reachable only via lifecycle anomalies, exactly as the finding scopes it. Low severity appropriate.</sub>

### TPX-054 · [LOW] ModuleData copy constructor copies the pool by value but keeps ready/empty/fill pointers into the source object's pool

**Claim.** The documented not-proper copy constructor deep-copies `pool` (forward_list<Data>) while copying `ready`, `empty`, and `fill` verbatim - containers of raw Data* that still point into `other`'s pool nodes, not into the freshly copied pool. Today module_data is resized exactly once from empty (no copies actually run), but any future growth/copy of the vector silently produces cross-object aliasing and dangling pointers when the source is destroyed.

**Proof.**

`src/include/xes_data_manager.h:75-79`
```cpp
inline ModuleData(const ModuleData& other) noexcept
  : write{}, lock_ready{}, lock_empty{},
    ready{other.ready}, empty{other.empty}, fill{other.fill},
    pool{other.pool}, cache{}, final{other.final}
  {}
```

**Impact.** Latent use-after-free / writes into a foreign histogram pool the first time the code is refactored to resize or copy module_data (e.g. dynamic chip count support).

**Fix.** Delete the copy constructor and store ModuleData in a std::deque or vector<unique_ptr<ModuleData>> (no copyability needed), or have the copy constructor clear ready/empty/fill and rebuild them from the copied pool.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (xes_data_manager.h:75-79). pool (forward_list<Data>) is deep-copied while ready/empty/fill hold raw Data* copied verbatim, still pointing into other's pool nodes. Today module_data.resize(nThreads) at line 133 runs once from an empty vector (elements default-constructed with empty containers; the user-declared copy ctor also suppresses the implicit move ctor, so any future reallocation of populated elements would invoke this broken copy). Latent-only hazard, correctly rated low.</sub>

### TPX-055 · [LOW] const_cast write to const member time_roi in Manager::Reset() is undefined behavior

**Claim.** Manager declares `const TimeRoi time_roi;` and Reset() assigns through const_cast; modifying an object declared const is UB ([dcl.type.cv]), and the compiler may legally cache/fold reads of time_roi across the assignment. The same pattern exists in Analysis::Reset for its const members.

**Proof.**

`src/include/xes_data_manager.h:418`
```cpp
const_cast<TimeRoi&>(time_roi) = gvars.time_roi;
```
`src/include/xes_data_manager.h:122`
```cpp
const TimeRoi time_roi;                 //!< Time ROI
```

**Impact.** Works today, but an optimizer or toolchain upgrade may start using stale ROI values in the writer's StartFrame (wrong TRoiStart/Step/N advertised downstream) with no diagnostic.

**Fix.** Drop the const qualifier from the member (it is per-measurement state, not a constant) and delete the const_cast.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (xes_data_manager.h:418 and 122). The member is declared const, making it a const subobject; modifying it through const_cast is UB per [dcl.type.cv] even though the enclosing Manager object is non-const, and the compiler may fold reads of time_roi across the assignment (it is read by the writer thread at line 166 for the StartFrame). Same pattern confirmed in Analysis::Reset (analysis.h:125-126, const_cast of time_roi and TRoiStep_inv). Low severity appropriate — works today, latent under optimization.</sub>

### TPX-056 · [LOW] TCP/Redis JSON output streams floats with default 6-significant-digit precision, truncating large bin counts

**Claim.** TcpWriter::write and RedisPublisher::write serialize TDSpectra elements, and Total/BeforeRoi/AfterRoi via default ostream formatting (precision 6). Bin values above ~1e6 (aggregated over a full save_interval across all chips at high count rates) are rounded to 6 significant digits in the JSON (e.g. 1234567 -> 1.23457e+06), silently degrading histogram values beyond float's own quantization.

**Proof.**

`src/xes_data_writer.cpp:128-129`
```cpp
for (std::remove_cv_t<decltype(elements)> i=1; i<elements; i++)
    send << ',' << TDSpectra[i];
```

**Impact.** Systematic rounding error in exported spectra at high count rates, invisible downstream.

**Fix.** Set `send << std::setprecision(std::numeric_limits<float>::max_digits10)` (9) before streaming spectra in both writers (FileWriter as well for consistency).

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (xes_data_writer.cpp:128-129). The TDSpectra subclaim is correct: elements are float (xes_data.h:30-32) and default ostream precision 6 renders e.g. 1234567 as 1.23457e+06 in both TcpWriter (126-133) and RedisPublisher (279-287) — real truncation between ~1e6 and float's exact-integer limit (~1.6e7). Adjustment: the claim that Total/BeforeRoi/AfterRoi are also affected is wrong — they are int (xes_data.h:34-36), and stream precision does not apply to integer output; those serialize exactly. Core finding stands for the spectra only; severity remains low.</sub>

### TPX-057 · [LOW] FileWriter indexes TDSpectra with globals read at write time instead of the Data's own dimensions

**Claim.** FileWriter::write iterates using gvars.pix_map->npoints and gvars.time_roi.TRoiN fetched at write time, while data.TDSpectra was sized from those globals at Reset/pool-init time. Within a correctly sequenced measurement these agree; but combined with the Reset-while-writer-active race reported separately (or any future state-machine relaxation letting config change while the writer drains), the loop reads TDSpectra out of bounds. TcpWriter correctly uses data.TDSpectra.size() instead.

**Proof.**

`src/xes_data_writer.cpp:50-54`
```cpp
const auto NumEnergyPoints = gvars.pix_map->npoints;
const auto TRoiN = gvars.time_roi.TRoiN;
for (std::remove_cv_t<decltype(NumEnergyPoints)> i=0; i<NumEnergyPoints; i++) {
    for (std::remove_cv_t<decltype(TRoiN)> j=0; j<TRoiN; j++) {
            OutFile << TDSpectra[j * NumEnergyPoints + i] << " ";
```

**Impact.** Latent out-of-bounds read producing corrupted .xes files or a crash whenever configuration and writer draining can overlap.

**Fix.** Store NumEnergyPoints/TRoiN in xes::Data at Init time (or derive TRoiN = TDSpectra.size()/npoints from a Data-held npoints) and iterate those, asserting NumEnergyPoints*TRoiN == data.TDSpectra.size().

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (xes_data_writer.cpp:50-54). FileWriter::write fetches gvars.pix_map->npoints and gvars.time_roi.TRoiN at write time and indexes TDSpectra with them, while the vector was sized from those globals at Init/pool-reset time; TcpWriter (line 123) correctly iterates data.TDSpectra.size(). In a correctly sequenced measurement the state machine keeps config and writing disjoint, so this is latent — an OOB read only materializes combined with the Reset-while-writer-active desync or future relaxation, exactly as scoped. Low severity appropriate.</sub>


## Event decoding & memory safety

*12 findings: 1 critical, 3 high, 2 medium, 4 low, 2 info*

### TPX-058 · [CRITICAL] Scalar getToaClock computes in 32-bit int (bit-field promotion): signed-overflow UB and garbage TOA timestamps in default (non-USE_AVX) builds

**Claim.** In decoder.h, TOA.spidr is a 16-bit bit-field, so `event.spidr << 18` undergoes integral promotion to int and is computed in 32-bit signed arithmetic. For spidr >= 8192 (i.e. whenever the 16-bit SPIDR counter, which wraps every 26.84 s, exceeds 3.355 s) the shift overflows int: undefined behavior, and in practice a wrapped/negative 32-bit value that is sign-extended to u64 and truncated into event_t.ts:47. Verified by compiling the exact struct: `expect=10485761600 got=1895827008 MISMATCH`, and UBSan reports 'left shift of 40000 by 18 places cannot be represented in type int'. This is the decoder used by every build without USE_AVX=1 (compile.sh sets AVX_FLAGS empty when USE_AVX is unset; GENERIC only drops -march=native), so the default tpx3app binary silently produces corrupt timestamps for ~87% of every SPIDR wrap cycle: relative TOA (el.ts - tdc_ts) becomes astronomically large, events fall into AfterRoi and vanish from the histograms, and the reorder heap ordering breaks. The AVX2 path (avx2_decoder.h toaclk) does the same math in 64-bit epi64 lanes and is correct, so the two build paths produce different physics results.

**Proof.**

`src/include/decoder.h:208-213`
```cpp
inline static u64 getToaClock(TOA event) noexcept
    {
        // ftoa is on a 640 MHz clock
        // toa is on a 40 MHz clock
        return ((event.spidr << 18) + (event.ToA << 4)) - event.FToA;
    }
```
`src/include/decoder.h:52`
```cpp
u64 spidr: 16;     //!< SPIDR time (0.4096ms)
```
`src/include/data_handler.h:141-142`
```cpp
if (ev.type.id == 0xb) {
    event = { AsiRawStreamDecoder::getToaClock(ev.toa), 0ull, AsiRawStreamDecoder::flatPixel(ev.toa) };
```
`compile.sh:32-36`
```cpp
if [ -z "${USE_AVX}" ]; then
    AVX_FLAGS=""
    EXTRA_VERSION=""
else
    AVX_FLAGS="-DAVX_DECODE -DAVX_ADDER"
```

**Impact.** Silent, large-scale event mis-binning/data loss in the default production build for any acquisition once the detector SPIDR clock passes 3.36 s within its 26.8 s cycle; heap reorder and period-relative timing become garbage. UB additionally licenses the optimizer (-O3 -flto -ffast-math) to do anything with this expression.

**Fix.** Force 64-bit arithmetic: `return ((u64{event.spidr} << 18) + (u64{event.ToA} << 4)) - u64{event.FToA};`. Audit every other bit-field expression for the same promotion trap (Header.size, fine_ts etc. are safe today because their uses stay within int range; getTdcClock's ts:35 does not promote).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All four quotes verbatim at cited lines. Independently reproduced: compiled the exact TOA struct; decltype(event.spidr << 18) is int (u64:16 bit-field promotes to int per [conv.prom]), and spidr=40000,ToA=100 yields expect=10485761600 got=1895827008 at both -O3 -ffast-math and -O0, matching the finding's numbers exactly. Overflow threshold spidr>=8192 (8192<<18 = 2^31) and 87.5% figure (57344/65536) check out. compile.sh confirms USE_AVX unset leaves AVX_DECODE undefined, so data_handler.h uses the scalar iterator calling getToaClock; avx2::toaclk uses _mm256_slli_epi64 (64-bit lanes) and is u</sub>

### TPX-059 · [HIGH] No TOA/TDC timestamp wraparound handling: TOA clock wraps at 2^34 ticks (26.8 s), TDC clock at 2^36 (107.4 s); relative-time subtraction underflows

**Claim.** The decoded TOA clock is at most spidr(16 bits)<<18 + ToA<<4, i.e. < 2^34 ticks of 1.5625 ns (wraps every 26.84 s), while the decoded TDC clock is ts(35 bits)<<1 | finebit, i.e. < 2^36 ticks (wraps every 107.37 s). No code anywhere (timing.h is only a wall-clock Timer; grep for wrap handling finds nothing) extends or reconciles these counters. In data_handler.h the relative time is computed as the raw u64 subtraction `el.ts - tdc_ts`; once the detector clock exceeds 2^34 ticks, every TOA timestamp is numerically smaller than the last TDC timestamp, the subtraction underflows to a huge value (truncated to 48 bits in toa_event.ts), and Analyse_ignore_tot classifies every event as AfterRoi. Assuming SERVAL streams the raw field widths declared in decoder.h and does not reset counters mid-acquisition, only the first 26.8 s quarter of each 107.4 s TDC cycle produces valid histograms; any scan longer than ~27 s (or spanning a wrap) silently loses its TOA data. The heap comparator (raw 47-bit ts) also misorders events across each wrap boundary, corrupting period assignment. In the AVX2 path an additional artifact occurs at the wrap tick: toaclk underflow produces all-ones high bits that are OR-ed into the px field (`_mm256_or_si256(toa_ev, _mm256_slli_epi64(toa_pos, 48))`), attributing the event to pixel 65535.

**Proof.**

`src/include/decoder.h:196-200`
```cpp
inline static u64 getTdcClock(TDC event) noexcept
    {
        // Ignore TDC fine time stamp error state (set value to 0 in that case)
        return (event.ts << 1) | (event.fine_ts > 6 ? 1 : 0);
    }
```
`src/include/decoder.h:67`
```cpp
u64 ts: 35;        //!< Timestamp (3.125ns)
```
`src/include/data_handler.h:401-403`
```cpp
} else {
    toaHits++;
    analysis.ProcessEvent(chipIndex, period, { el.ts - tdc_ts, el.px });
```
`src/include/data_handler.h:41-44`
```cpp
inline bool operator<(const event_t& a, const event_t& b) noexcept
    {
        return a.ts > b.ts;
    }
```

**Impact.** Silent loss/misclassification of the majority of TOA events in long acquisitions (everything counted in AfterRoi), plus misordered periods at every 26.8 s boundary. This matches the scope hint that TDC clock wrap handling is expected but absent.

**Fix.** Maintain per-chip extended (unwrapped) 64-bit timestamps: detect a decrease of the raw TOA clock (or TDC clock) versus the previous one and add 2^34 (resp. 2^36) per wrap before pushing events into the heap; alternatively compute relative time modulo 2^34 with a signed window: `int64_t rel = (int64_t)((toa - tdc) << 30) >> 30;`-style sign-extension of the 34-bit difference.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim at cited lines (data_handler.h 401-403 is the pass-two loop; the same pattern repeats at 428-430). Counter widths re-derived from the bit-fields: TOA = spidr(16)<<18 + ToA(14)<<4 - FToA < ~2^34 ticks of 1.5625 ns = 26.84 s; TDC = ts(35)<<1|fine < 2^36 ticks = 107.37 s. Grep over src/ finds no wrap/rollover handling anywhere; timing.h is only a std::chrono wall-clock Timer; the decoder drops all packet types except 0xb/0x6, so even if SERVAL emitted heartbeat/extension packets they would be ignored. el.ts - tdc_ts is raw u64 subtraction stored into toa_event.ts:48 then read as i</sub>

### TPX-060 · [HIGH] Analysis::Register can write one bin past TDSpectra: float bin index reaches TRoiN for ROI windows longer than 2^24 ticks

**Claim.** The time bin is computed in float: `TP = (reltoa - TRoiStart) * TRoiStep_inv` with `TRoiStep_inv = 1.f/TRoiStep`. For ROI windows (TRoiStep*TRoiN) above 2^24 ticks (~26 ms), the int64->float conversion of the tick offset and/or the inexact reciprocal round upward so TP == TRoiN for the last ticks inside the guard `reltoa < TRoiEnd`. Register then writes `TDSpectra[TRoiN*npoints + energy_point] += weight`, up to npoints floats past the end of the heap buffer (TDSpectra.size() == TRoiN*npoints). Verified with a compiled replica using the build's -ffast-math: step=6400,N=5000 -> worst TP=5000 (max legal 4999) OUT OF BOUNDS; also step=1600/N=20000 and step=640/N=40000 overflow. The default (step=1, N=5000) is safe, so this triggers only for configurations with wide windows (e.g. 10 us bins at low TDC rates), set via the REST time ROI.

**Proof.**

`src/include/analysis.h:56-60`
```cpp
} else if (reltoa >= (int64_t)time_roi.TRoiEnd) {
            data.AfterRoi++;
        } else {
            const int TP = (reltoa - time_roi.TRoiStart) * TRoiStep_inv;
            Register(data, index, TP);
```
`src/include/analysis.h:40`
```cpp
data.TDSpectra[TimePoint * pix_map.npoints + part.energy_point] += part.weight;
```
`src/include/xes_data.h:46-48`
```cpp
: TDSpectra(
                    global::instance->time_roi.TRoiN * global::instance->pix_map->npoints
                  )
```

**Impact.** Heap buffer overflow (out-of-bounds float writes) in the hottest per-event path, silently corrupting adjacent xes::Data pool objects (period fields, counters) or the allocator heap; corrupt histograms or crashes for legitimate wide-window configurations.

**Fix.** Clamp after conversion: `const int TP = ...; if ((unsigned)TP >= time_roi.TRoiN) { data.AfterRoi++; return; }`, or compute the bin with integer arithmetic: `const u64 TP = (u64)(reltoa - (int64_t)time_roi.TRoiStart) / time_roi.TRoiStep;`.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (analysis.h 56-60, 40; xes_data.h 46-48). Independently reproduced with a compiled replica: step=6400, N=5000 gives worst TP=5000 against max legal 4999 (OUT OF BOUNDS), and this holds even at -O0 without fast-math — the int64->float conversion alone (31999999 rounds to 32000000.0f, spacing 2 above 2^24) pushes the product to 5000.0. Default step=1/N=5000 verified safe. TDSpectra is sized TRoiN*npoints (xes_data.h:46-48) so Register writes up to npoints floats past the end. Integer guard reltoa < TRoiEnd does not prevent the float rounding. Severity high appropriate: OOB heap w</sub>

### TPX-061 · [HIGH] aligned_allocator::allocate is noexcept and returns nullptr on failure; std::vector then writes through nullptr (and npoints from the pixel map is unbounded)

**Claim.** aligned_allocator::allocate violates the Allocator requirement to throw on failure: it is noexcept and returns whatever std::aligned_alloc returns, including nullptr. std::vector treats the returned pointer as valid and value-initializes elements, so any allocation failure becomes a write through nullptr (SIGSEGV of the whole daemon) instead of a catchable bad_alloc. This is reachable: TDSpectra is sized TRoiN * npoints where npoints = (max energy point in the uploaded pixel map) + 1 with no upper bound (energy_points.cpp), and TRoiN comes from the REST time ROI (which, per the separate `&&` validation bug, even admits negative values that wrap to ~2^64). A single oversized energy-point index or bad ROI kills tpx3app with SIGSEGV at setup, after which every scan fails with connection refused (same blast radius as the known test_server incident). Additionally, std::aligned_alloc formally requires size to be a multiple of the alignment; n*sizeof(float) is usually not a multiple of 32 — glibc tolerates this, but macOS/strict C11 implementations return NULL for every allocation.

**Proof.**

`src/include/aligned_allocator.h:39-41`
```cpp
inline T* allocate(std::size_t n) const noexcept {
        return static_cast<T*>(std::aligned_alloc(memory_alignment, n * sizeof(T)));
    }
```
`src/energy_points.cpp:88-89`
```cpp
auto p = pointList->getElement<unsigned>(k);
                    pmap.npoints = std::max(p, pmap.npoints);
```
`src/include/xes_data.h:96-99`
```cpp
inline void Init()
            {
                TDSpectra.resize(
                    global::instance->time_roi.TRoiN * global::instance->pix_map->npoints
```

**Impact.** Process-killing segfault (not a recoverable 'except' state) on OOM or on a pixel map containing one large energy-point index; portability failure on non-glibc platforms where every aligned_alloc returns NULL.

**Fix.** In allocate: round the byte size up to a multiple of memory_alignment, check the result and `throw std::bad_alloc{}` on nullptr (drop noexcept). Additionally validate an upper bound on energy_point in from_json/from_file (e.g. reject p > 10^6) and on TRoiN in SetTimeROI.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier; corrected lines: src/include/xes_data.h proof: 95-98 (other proofs' lines correct as cited)). Author confidence: high.</sub>

<sub>Verifier: Claim confirmed in substance; only a one-line quote offset corrected. aligned_allocator.h:39-41 verbatim: allocate is noexcept and returns aligned_alloc's result unchecked, violating the Allocator requirement (must throw bad_alloc); libstdc++/libc++ vectors do not null-check, so value-initialization writes through nullptr -> SIGSEGV. Reachability confirmed: TDSpectra uses aligned_allocator (xes_data.h:31), npoints = max uploaded energy-point + 1 with no upper bound (energy_points.cpp:88-89, 96; same in from_file:159), and TRoiN can wrap to ~2^64 via the separate && validation bug. macOS aligne</sub>

### TPX-062 · [MEDIUM] SetTimeROI validates with && instead of ||: non-positive TRoiStep or TRoiN accepted

**Claim.** The guard `if ((tRoiStep <= 0) && (tRoiN <= 0)) throw` only rejects configurations where BOTH values are non-positive, contradicting its own message 'TRoiStep and TRoiN must be positive'. tRoiStep=0 with tRoiN>0 is accepted: TRoiStep_inv becomes +inf in Analysis, TRoiEnd == TRoiStart so every event is classified Before/AfterRoi and the scan completes with empty histograms (surfaced only as 'no event data was collected'). Negative tRoiStep or tRoiN are assigned to u64 fields and wrap to ~2^64, feeding the TRoiN*npoints allocation (see allocator finding) or producing a wrapped TRoiEnd.

**Proof.**

`src/include/time_roi.h:37-38`
```cpp
if ((tRoiStep <= 0) && (tRoiN <= 0))
                        throw std::invalid_argument("TRoiStep and TRoiN must be positive");
```
`src/include/time_roi.h:40-44`
```cpp
TRoiStart = tRoiStart;
                TRoiStep = tRoiStep;
                TRoiN = tRoiN;

                TRoiEnd = TRoiStart + TRoiStep * TRoiN;
```

**Impact.** A malformed ROI from the BEC client is accepted instead of rejected at config time; consequences range from a silently empty measurement (step=0) to a giant allocation/crash (negative N), discovered only after the scan ran.

**Fix.** Change to `if ((tRoiStep <= 0) || (tRoiN <= 0))` and additionally reject tRoiStart < 0 if unsupported, plus an upper bound on tRoiN.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim at time_roi.h 37-38 and 40-44. Re-derived: the && guard only fires when BOTH tRoiStep<=0 AND tRoiN<=0, contradicting the exception message; tRoiStep=0 with tRoiN>0 passes, making TRoiStep_inv=inf in Analysis and TRoiEnd==TRoiStart so every event is Before/AfterRoi (silently empty histograms). Params are int, fields are u64, so negative inputs wrap to ~2^64 and feed the TRoiN*npoints allocation. No other validation exists upstream (SetTimeROI is the only guard). Severity medium appropriate.</sub>

### TPX-063 · [MEDIUM] Stream parser is fail-stop with no resync: any malformed header, truncated chunk, or >1 out-of-order chunk aborts the entire measurement in all analyser threads

**Claim.** Every analyser thread independently parses the full untrusted TCP stream. Any of: a header word without the TPX3 magic, a size field not a multiple of 8, a bogus small size, a packet-id sequence that cannot be repaired by the single-slot store/restore mechanism, or a connection that ends mid-chunk ('premature end of data package') throws a RuntimeException; the catch in analyseData calls stopNow() and sets a global error, ending the whole measurement in the 'except' state. There is no attempt to resync to the next TPX3 magic, and a single corrupted size field desyncs all chip consumers (each skips foreign packets via `pos += size + 1` using the same corrupt field). Note the packet-id word's type byte (0x50) is never verified, only its 48-bit count. An aborted SERVAL send or measurement abort mid-chunk therefore reliably lands the app in 'except' — the same class of mid-stream-abort fragility as the known test_server broken-pipe incident.

**Proof.**

`src/include/subreservation.h:227-230`
```cpp
if (__builtin_expect(content[pos].header.id != AsiRawStreamDecoder::chunk_id, false))
                    throw RuntimeException{"expected header has no TPX3 id"};
                if (__builtin_expect(content[pos].header.size % event_size != 0, false))
                    throw RuntimeException{"chunk size not a multiple of the event size"};
```
`src/include/subreservation.h:208`
```cpp
throw RuntimeException{std::string{"unable to handle reordered chunk, expected id "} + std::to_string(pkgid) + ", but got id " + std::to_string(content[pos].packet_id.count)};
```
`src/include/subreservation.h:294-297`
```cpp
if (! end) {
                        if (rest)
                            throw RuntimeException{"premature end of data package"};
                        return;
```
`src/include/subreservation.h:194`
```cpp
if (__builtin_expect(content[pos].packet_id.count != pkgid, false)) {
```

**Impact.** One corrupt or truncated chunk (network glitch, SERVAL abort, replay-server kill) fails the whole scan rather than dropping the bad chunk; with the BEC client this surfaces as the known 'except' state failures.

**Fix.** On parse mismatch, resync: scan forward word-by-word for the TPX3 magic with a plausible size/chip and count dropped bytes in a diagnostic counter instead of throwing; treat mid-chunk EOF as benign when stop_collect was requested; verify packet_id.type == 0x50 before trusting the count; consider a small ring of stored out-of-order chunks instead of exactly one (stored_pkgid).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All four quotes verbatim at cited lines (subreservation.h 227-230, 208, 294-297, 194). Re-derived: search_pkg/check_id/update contain no resync path — every mismatch throws Poco::RuntimeException, which analyseData's catch (data_handler.h 437-445) turns into stopNow() + global::set_error, i.e. whole-measurement failure. Foreign-chip skipping at subreservation.h:254 (pos += size + 1) trusts the same unvalidated size field, so one corrupt size desyncs every chip consumer. check_id compares only packet_id.count; the 0x50 type byte is never checked. The store/restore mechanism holds exactly one ou</sub>

### TPX-064 · [LOW] restore_data never returns the last restored jar: mlock'ed 32 KB buffer lost per reordered chunk

**Claim.** During chunk reordering, stored jars are deliberately not returned at traversal time (read_reservation is called with no_return). restore_data compensates by calling return_jar for each previous restored jar when moving to the next one, but on completion (store.empty()) it sets restored_jar = nullptr without returning it. When the last stored segment's jar differs from the current reservation jar, that jar's done counter remains one short of nthreads forever, so it never re-enters the free list: one pinned (mlock'ed) container is removed from circulation per reorder event until the collection is reset at measurement end, and the reader allocates replacement jars (jar_list grows).

**Proof.**

`src/include/subreservation.h:153-163`
```cpp
if (store.empty()) {
                end = reservation.end / event_size;
                content = (AsiRawStreamDecoder::Event*)current.jar->container.data;
                pos = current.pos;
                rest = current.rest;
                consume = current.consume;
                current = {};
                state = CHECK_ID;
                restored_jar = nullptr;
```
`src/include/subreservation.h:169-170`
```cpp
if (restored_jar && (restored_jar != restore.jar))
                buffers.return_jar(restored_jar);
```

**Impact.** Pinned memory growth proportional to reorder events within a measurement; heavy reordering can exhaust RLIMIT_MEMLOCK, making container pin() throw and failing the scan.

**Fix.** In the store.empty() branch, call `if (restored_jar && restored_jar != reservation.jar) buffers.return_jar(restored_jar);` before nulling restored_jar.

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (quote text spans 153-161 within the cited 153-163 range; 169-170 exact). Independently traced the jar accounting: jars J1..Jk holding the stored package's segments each miss one return_jar — J1..J(k-1) via store_data's read_reservation(reservation, true), Jk via update_reservation's no_return = (store.back().jar == reservation.jar) when Jk is finished with the store pending. restore_data returns J1..J(k-1) as it advances (lines 169-170) but the store.empty() branch nulls restored_jar without returning Jk. When the restore trigger point is in a later jar than Jk (i.e. last stor</sub>

### TPX-065 · [LOW] AVX2 event_iterator tail load reads up to 24 bytes past the consumer's fill level — a benign-value data race with the reader thread

**Claim.** In the AVX2 build, next() always loads a full 32-byte vector: `_mm256_load_si256(&data[pos >> 2])` where pos is aligned down to a multiple of 4 events. When `end` (pos + consume, derived from the jar's current byte fill level) is not a multiple of 4, the final load reads up to 3 events (24 bytes) beyond the fill level. The bytes stay inside the mmap'ed container (container_size is a multiple of 32), but the producer thread may be concurrently writing exactly those bytes via receiveBytes before publishing a higher level with put_data — a formal C++ data race on non-atomic memory. The decoded excess lanes are discarded by `while (pos + cur < end)`, so on x86 this is benign in practice, but it is UB the sanitizers/TSan will flag and it reads uninitialized/partial event bytes.

**Proof.**

`src/include/data_handler.h:90-93`
```cpp
while (pos + cur < end) {

                    if (cur == 0)
                        _mm256_store_si256((__m256i*)current, avx2::decode(_mm256_load_si256(&data[pos >> 2])));
```
`src/include/io_buf.h:335-343`
```cpp
inline void put_data(jar_t* jar, int level)
        {
            assert(jar);
            std::lock_guard lock{jar->level_lock};
            
            if (jar->level == level)
                final_jar.store(jar, std::memory_order_release);
            else
                jar->level = level;
```

**Impact.** Formally undefined behavior and TSan noise; on non-x86 or future aggressive optimization the discarded-lane guarantee is only conventionally safe. No practical corruption observed path since excess lanes are never consumed.

**Fix.** Round the consumer's usable end down to a multiple of 4 events for the vector loop and decode the ragged tail with the scalar path, or use _mm256_maskload_epi64 for the final partial group.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (data_handler.h 90-93, io_buf.h 335-343). Re-derived: consumer end = pos + consume where consume derives from reservation.end = the byte level the producer published (any value, not 4-event aligned); next() always loads a full 32-byte vector at pos&~3, so when end % 4 != 0 the final load covers up to 3 events past the published level. The producer thread concurrently writes exactly those bytes (readData writes into data[reservation.start..] with start = published level, no per-byte synchronization) — a formal C++ data race, though excess lanes are discarded by the pos+cur<end g</sub>

### TPX-066 · [LOW] Analysis::Reset and xes::Manager::Reset modify const-declared members via const_cast (UB under -O3 -flto)

**Claim.** Analysis declares `const TimeRoi time_roi` and `const float TRoiStep_inv`, then Reset() writes them through const_cast. Modifying an object declared const is undefined behavior regardless of the enclosing object's constness; with -O3 -flto=auto the compiler is entitled to cache the 'immutable' TRoiStep_inv/time_roi across calls, which would make REST-configured ROI changes silently ineffective for subsequent measurements. xes_data_manager.h:418 has the same pattern for its TimeRoi.

**Proof.**

`src/include/analysis.h:125-126`
```cpp
const_cast<TimeRoi&>(time_roi) = gvars.time_roi;
        const_cast<float&>(TRoiStep_inv) = 1.f/time_roi.TRoiStep;
```
`src/include/analysis.h:23-24`
```cpp
const TimeRoi time_roi;                 //!< Time ROI data
    const float TRoiStep_inv;               //!< 1. / TRoiStep
```
`compile.sh:21`
```cpp
SPEED_FLAGS+=" -O3 -ffast-math -DNDEBUG -flto=auto"
```

**Impact.** Latent miscompilation risk: stale ROI/step values used after reconfiguration, i.e. events binned with the previous scan's ROI without any error.

**Fix.** Drop the const qualifiers on these members (they are logically mutable configuration) and assign normally in Reset().

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (analysis.h 125-126, 23-24; compile.sh 21). Cross-checked xes_data_manager.h: 'const TimeRoi time_roi' declared at line 122 and 'const_cast<TimeRoi&>(time_roi) = gvars.time_roi;' at line 418, exactly as claimed. Modifying an object declared const (the const member subobjects) through const_cast is UB per [dcl.type.cv]/4 regardless of the enclosing object's constness; with -O3 -flto=auto the compiler may legally cache TRoiStep_inv/time_roi across Reset(). Latent risk, no observed miscompilation — severity low appropriate.</sub>

### TPX-067 · [LOW] event_t::valid and operator!= type-pun via reinterpret_cast (*(u64*)&event) — strict-aliasing UB

**Claim.** event_t is inspected by dereferencing its address as u64 (`*(u64*)&event`). Accessing an event_t object through a u64 lvalue violates the strict aliasing rule; with -O3 -flto the optimizer may reorder or fold these loads against event_t member writes (e.g. the heap array writes in analyseData). All mainstream compilers currently treat the u64-bit-field storage unit as compatible so it works today, but it is formally UB and fragile under LTO.

**Proof.**

`src/include/event_type.h:28-31`
```cpp
inline static bool valid(const event_t& event) noexcept
    {
        return *(u64*)&event != u64{0};
    }
```
`src/include/event_type.h:56-59`
```cpp
inline bool operator!=(const event_t&a, const event_t& b) noexcept
{
    return *(u64*)&a != *(u64*)&b;
}
```

**Impact.** Latent miscompilation risk in the hot decode loop under LTO/aggressive optimization; sanitizer findings.

**Fix.** Use `u64 v; std::memcpy(&v, &event, sizeof v);` or C++20 std::bit_cast<u64>(event); both compile to a single load.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim at event_type.h 28-31 and 56-59. Re-derived: event_t is a struct of u64 bit-fields; accessing it through a u64 glvalue is not among the permitted aliases in [basic.lval] (bit-field storage units are not nameable u64 objects), so it is formally UB, while every mainstream compiler treats the single-u64 storage unit as compatible today. valid() is called in the hot AVX2 decode loop (data_handler.h:102). memcpy/bit_cast fix compiles to a single load. Severity low appropriate.</sub>

### TPX-068 · [INFO] AVX2 zero-sentinel silently drops a legitimate all-zero TOA event; GENERIC and AVX2 builds disagree

**Claim.** avx2::decode maps unknown packet types to an all-zero lane and event_iterator discards lanes failing event_t::valid (all bits zero). A genuine TOA event at flat pixel 0 with spidr=0, ToA=0, FToA=0 (clock origin or the exact wrap tick) decodes to exactly zero and is silently dropped in the AVX2 build, while the GENERIC iterator returns it. Frequency is negligible (one specific tick per 26.8 s wrap at one pixel), but it makes the two build paths non-identical and is an in-band sentinel worth documenting.

**Proof.**

`src/include/avx2_decoder.h:78-80`
```cpp
\brief Decode raw event vector
    \param events Event vector to be decoded
    \return Decoded event_t event vector (element 0 for unknown)
```
`src/include/data_handler.h:102-103`
```cpp
if (event_t::valid(event))
                        return true;
```

**Impact.** One-in-billions silent event drop and a build-path behavioral difference that can confuse validation comparing GENERIC vs AVX2 output.

**Fix.** Use a dedicated invalid flag bit instead of the all-zero sentinel (e.g. reserve px=0xFFFF+is_tdc combination, or carry the type mask out of decode()).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (avx2_decoder.h 78-80 doc comment stating 'element 0 for unknown'; data_handler.h 102-103). Re-derived: a genuine 0xb event with spidr=0, ToA=0, FToA=0, PixAddr=0 decodes in avx2::decode to toaclk=0 | toapos<<48=0, an all-zero lane indistinguishable from the unknown-type sentinel, discarded by event_t::valid; the GENERIC iterator (data_handler.h 141-143) returns any 0xb event unconditionally, so the builds diverge on exactly this event. Frequency negligible as stated. Severity info appropriate.</sub>

### TPX-069 · [INFO] Known incident #3 code basis confirmed: pixel-map upload requires exact chip-count match with SERVAL layout

**Claim.** from_json resizes the map to the SERVAL-reported chip count and then rejects any uploaded map whose top-level 'chips' array length differs, with the exact error seen in integration ('mismatch with number of chips from detector server'). A client wanting to map only one chip must still send empty arrays for all other chips. Bounds on pixel index are enforced (index >= 65536 rejected), and to_map always sets pixels_per_chip = 65536, which is what keeps the unchecked PixelMap::operator[] indexing safe against the 16-bit flat pixels produced by the decoder.

**Proof.**

`src/energy_points.cpp:70-72`
```cpp
auto nchips = chipList->size();
        if (numChips != nchips)
            throw Poco::RuntimeException{"mismatch with number of chips from detector server"};
```
`src/energy_points.cpp:78-80`
```cpp
unsigned index = partLists->getValue<unsigned>("i");
                if (index >= numPixels)
                    throw Poco::RuntimeException{"invalid pixel index"};
```
`src/include/pixel_map.h:127-131`
```cpp
inline const Range operator[](const PixelIndex& index) const noexcept
    {
        auto base = index.chip * pixels_per_chip + index.flat_pixel;
        return {const_cast<PixelMap&>(*this), indices[base], indices[base + 1]};
```

**Impact.** Operational constraint for the BEC client (must always send all chips); memory safety of the unchecked hot-path map lookup depends on the invariant pixels_per_chip == 65536 established by to_map — any future map source that sets a smaller pixels_per_chip would make operator[] read out of bounds for high pixel addresses.

**Fix.** Either accept partial maps by padding missing chips with empty pixel lists, or keep the strict check but document it in the REST API; add a static_assert/runtime check that pixels_per_chip == chip_size*chip_size where PixelMap is installed.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (energy_points.cpp 70-72, 78-80; pixel_map.h 127-131). Re-derived: from_json resizes to layout.chip.size() and throws the exact 'mismatch with number of chips from detector server' message on any length difference, so partial maps require empty per-chip arrays. numPixels = chip_size*chip_size = 256*256 = 65536 (layout.h:14) bounds the pixel index, and to_map sets pixels_per_chip from the resized flat_pixel vector (=65536), which together with the indices sentinel (size pixels_per_chip*nchips+1) keeps the unchecked noexcept operator[] safe for the decoder's 16-bit flat pixels. S</sub>


## State machine, errors & process lifecycle

*18 findings: 1 critical, 5 high, 10 medium, 2 low*

### TPX-070 · [CRITICAL] WebSocket StateHandler: unsynchronized shared 'ws' pointer causes use-after-free and lets any handler thread destroy the current client's socket

**Claim.** The single static WebSocket pointer 'ws' is replaced under ws_mutex by every new /ws connection, but the per-connection handler loop reads and uses 'ws' WITHOUT holding ws_mutex. When client B connects while client A's handler thread is blocked in ws->receiveFrame(), the reset() deletes A's WebSocket out from under A's thread (use-after-free), or A's thread continues looping on B's socket object (two threads on one non-thread-safe socket). Additionally, when the old handler thread exits, its cleanup block shuts down and nulls whatever 'ws' currently points to - i.e. it destroys the NEW client's socket. This is the code basis of live incident #1 (competing clients silently steal/kill the state stream) plus a latent crash of the whole DAQ process.

**Proof.**

`src/include/rest_callbacks.h:156-157`
```cpp
std::lock_guard lock(ws_mutex);
ws.reset(new WebSocket(request, response));
```
`src/include/rest_callbacks.h:168-170`
```cpp
while ((ws != nullptr) && !stop_sig) {
    try {
        n = ws->receiveFrame(buffer, sizeof(buffer), flags);
```
`src/include/rest_callbacks.h:194-199`
```cpp
std::lock_guard<std::mutex> lock(ws_mutex);
if (ws != nullptr) {
    ws->shutdown();
    ws.reset(nullptr);
}
```

**Impact.** A second /ws client (monitoring tool, retrying BEC client, curl test) can crash tpx3app mid-scan via use-after-free, or silently close the legitimate client's state stream so it never sees 'await_connection'/'collect' - already observed as real scan failures.

**Fix.** Give each handler its own WebSocket (support N clients: keep a mutex-protected std::vector<std::shared_ptr<WebSocket>>; set_state iterates it, dropping dead entries). If single-client is intended, hold ws_mutex (or a shared_ptr copy taken under the mutex) for every ws access in the reader loop, and make the cleanup block only reset 'ws' if it still points to this handler's own socket instance.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All three quotes are verbatim (cleanup quote occupies 195-199 within cited 194-199). The reader loop at rest_callbacks.h:168-188 reads and dereferences the static unique_ptr 'ws' with no ws_mutex; Poco HTTPServer runs each /ws connection on its own pool thread, so a second connect executes ws.reset(new WebSocket...) at line 157 under the mutex while the first thread may be blocked inside ws->receiveFrame() (1s receive timeout keeps it there most of the time) — deleting the object under it (use-after-free), or the old thread continues on the new client's socket. The exit cleanup (195-199) shuts</sub>

### TPX-071 · [HIGH] REST ?restart=true permanently breaks the /ws state stream: StateHandler::stop_sig is set on every shutdown and never cleared

**Claim.** Tpx3App::main() ends every run (including a SIGUSR1 or /?restart=true triggered run) by calling StateHandler::stop(), which sets the static stop_sig=true. stop_sig is never reset anywhere (verified by grep: only set at rest_callbacks.h:229, read at :168). After the restart loop in main() re-runs the application, every new /ws connection sends the initial state, immediately fails the loop condition '!stop_sig', shuts the socket and closes. The advertised recovery path (?restart=true / 'tpx3app restart') therefore leaves the process with a WebSocket endpoint that accepts connections and instantly closes them, forever.

**Proof.**

`src/include/rest_callbacks.h:227-230`
```cpp
inline static void stop() noexcept
{
    stop_sig = true;
}
```
`src/main.cpp:810-811`
```cpp
set_state(global::shutdown);
StateHandler::stop();
```
`src/main.cpp:929-938`
```cpp
do {
    global::instance->stop.store(false);
    global::instance->restart.store(false);
    retval = app.run();
    if (global::instance->restart.load()) {
```

**Impact.** After any restart-based recovery, the BEC client's state monitoring via /ws is dead; all subsequent scans fail the same way as incident #1 (client never sees state transitions). Only a full process kill+start recovers, which is undocumented.

**Fix.** Reset stop_sig at the start of each run (e.g. add StateHandler::start() that sets stop_sig=false, call it from rest::start_service or at the top of Tpx3App::main), or make stop_sig a member of the RestService lifetime instead of a process-lifetime static.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (third proof's quote actually occupies main.cpp:929-933, inside the cited 929-938 range). Independent grep confirms stop_sig appears only at rest_callbacks.h:136 (declaration), :168 (read), :229 (set) — never reset. StateHandler members are 'static inline' with process lifetime, and the restart loop in main() re-invokes app.run() in the same process, so after any run ends via main.cpp:811 every subsequent /ws handler sends the initial state frame, immediately fails '!stop_sig' at :168, and closes via the cleanup block. Claim holds exactly as stated.</sub>

### TPX-072 · [HIGH] Restart accumulates stop_handlers holding dangling references to destroyed stack locals; REST stop/stop_collect after a restart invokes them (UB/crash)

**Claim.** Tpx3App::main() registers lambdas into the process-global gvars.stop_handlers that capture BY REFERENCE the local unique_ptrs copyPtr/dataHandlerPtr of that particular main() invocation. stop_handlers is never cleared (verified by grep). After a restart (SIGUSR1 or /?restart=true) the old locals are destroyed but the old lambdas remain; the next run appends new ones. Any /?stop, /?restart or /?stop_collect then iterates ALL handlers, dereferencing a dangling reference to a dead stack frame - undefined behavior. The window is concrete: the new run starts the REST service (main.cpp:657) BEFORE re-registering handlers (main.cpp:665/674), so a REST stop arriving in that window calls only the stale, dangling lambda.

**Proof.**

`src/main.cpp:674-676`
```cpp
gvars.stop_handlers.emplace_back([&dataHandlerPtr]() {
    dataHandlerPtr->stopNow();
});
```
`src/include/rest_callbacks.h:341-343`
```cpp
gvars.restart.store(true);
gvars.stop.store(true);
for (const auto& handler : gvars.stop_handlers)
```
`src/include/global.h:47`
```cpp
std::vector<stop_handler> stop_handlers;                               //!< Called by REST /?stop
```

**Impact.** After one restart, control-plane stop/abort commands execute through dangling references: potential SIGSEGV of the DAQ backend exactly when the operator tries to stop/abort a scan. Even when it happens to 'work' (identical stack layout across runs), it is UB and the handler list grows unboundedly with duplicate invocations.

**Fix.** Clear gvars.stop_handlers at the start of Tpx3App::main() (before rest::start_service), and register handlers that capture stable objects (e.g. weak/shared ownership of the handler objects, or route through a single atomic flag checked by DataHandler/CopyHandler) instead of references to stack-local unique_ptrs.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier; corrected lines: src/include/rest_callbacks.h: 340-342 (second proof; quote is off by one line)). Author confidence: high.</sub>

<sub>Verifier: Substance fully confirmed; only the second proof's line numbers are off by one (quote sits at rest_callbacks.h:340-342, not 341-343). Grep confirms stop_handlers is only emplaced (main.cpp:665, 674) and iterated (rest_callbacks.h:324, 342, 359) — never cleared. The lambdas capture the stack-local unique_ptrs copyPtr/dataHandlerPtr by reference; after a restart those frames are dead but the entries persist, and the new run starts the REST service (main.cpp:657) before appending fresh handlers (665/674), so a /?stop, /?restart or /?stop_collect in that window (or any time after, via the stale en</sub>

### TPX-073 · [HIGH] set_state() sends on the WebSocket with no exception guard and no send timeout; a broken or stalled WS client can abort the whole server loop or wedge the main thread

**Claim.** StateHandler::set_state() calls ws->sendFrame() while holding ws_mutex, with no try/catch and no send timeout ever set on the socket (only setReceiveTimeout at rest_callbacks.h:160). Poco sendFrame throws NetException/IOException on a broken peer (EPIPE/RST). set_state is invoked from Tpx3App::main OUTSIDE the try block (lines 686, 692, 704, 810) and INSIDE the catch handlers (lines 788, 798); an exception at any of these points propagates out of main(), terminating the server-mode loop entirely (Poco::Application::run catches it and the process exits unless a restart was pending); thrown at line 810 it also skips StateHandler::stop()/rest::stop_service/pipeline shutdown. Separately, if the WS client stops reading and the TCP send buffer fills, sendFrame blocks indefinitely with ws_mutex held, freezing the state machine and blocking new /ws connections.

**Proof.**

`src/include/rest_callbacks.h:212-217`
```cpp
std::lock_guard<std::mutex> lock(ws_mutex);
if (global::instance->state != state) {
    global::instance->state = state;
    if (ws == nullptr)
        return;
    ws->sendFrame(state.data(), state.size(), WebSocket::FRAME_TEXT);
```
`src/main.cpp:703-706`
```cpp
gvars.stop_collect = false;
set_state(global::setup);

try {
```
`src/main.cpp:786-788`
```cpp
} catch (Poco::Exception& ex) {
    gvars.set_error(ex.displayText());
    set_state(global::except);
```

**Impact.** A single ungracefully-disconnected or stalled WebSocket client (laptop sleep, killed process, network drop) can take down or freeze the entire DAQ backend between scans - a process-restart-required failure that is not documented as such.

**Fix.** In set_state(), wrap sendFrame in try/catch: on any exception, log, ws->close()/ws.reset() and continue (state update must never throw). Set a short send timeout (ws->setSendTimeout) when the WebSocket is created. Never call sendFrame from catch blocks without a guard.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim. set_state (rest_callbacks.h:209-222) calls ws->sendFrame under ws_mutex with no try/catch; the only timeout set on the socket is setReceiveTimeout at :160. Verified call sites: main.cpp:686, 692, 704, 810 are outside the try block (which spans 706-785), and 788/798 are inside catch handlers — a throw from any of these propagates out of Tpx3App::main (throwing a new exception from a catch block simply propagates, no terminate), Poco's Application::run catches and returns, and the outer do/while exits unless restart was pending. A throw at 810 skips StateHandler::stop()/rest::st</sub>

### TPX-074 · [HIGH] await_connection poll loop only honors stop_collect: REST /?stop and /?restart hang indefinitely; SIGTERM escapes only via a spurious EINTR exception

**Claim.** The accept-wait loop breaks on poll timeout only when gvars.stop_collect is set; gvars.stop and gvars.restart are never consulted. A REST /?stop=true or /?restart=true issued while the program is in await_connection (a state the client is explicitly told about) sets stop/restart flags and runs stop_handlers (which do nothing before run_async), then the loop keeps polling every ~300 ms forever until SERVAL connects or a separate /?stop_collect arrives. SIGTERM/SIGUSR1 do break out, but only accidentally: the signal makes poll fail with EINTR, which the code converts into a thrown RuntimeException ('poll failed - Interrupted system call'), polluting last-error and routing an orderly stop/restart through the exception/except path.

**Proof.**

`src/main.cpp:741-748`
```cpp
if (ret == -1) {
    throw Poco::RuntimeException(std::string{"poll failed - "} + std::strerror(errno));
} else if (ret == 0) {  // timeout
    if (gvars.stop_collect)
        break;
} else if (fds[0].revents & POLLIN) {
    break;
}
```
`src/include/rest_callbacks.h:337-341`
```cpp
global::instance->get_callbacks["/?restart"] = [](const std::string& val) -> std::string {
    if (val == "true") {
        auto& gvars = *global::instance;
        gvars.restart.store(true);
        gvars.stop.store(true);
```

**Impact.** If a scan is aborted before SERVAL connects (realistic: BEC abort between /?start and measurement start), REST stop/restart appear accepted (200 OK) but the process stays wedged in await_connection; recovery paths silently fail. Signal-based stop works but records a bogus error and can turn a clean SIGUSR1 restart into an error cycle.

**Fix.** Change the timeout branch to 'if (gvars.stop_collect || gvars.stop) break;' and treat ret==-1 && errno==EINTR as 'continue' (re-check flags) instead of throwing. After the loop, handle the stop case like the stop_collect case.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp:741-748, rest_callbacks.h:337-341). The timeout branch checks only gvars.stop_collect; gvars.stop/restart are never consulted, and the loop is 'while (true)'. One nuance: the stop_handlers invoked by /?stop are not literally 'nothing' — dataHandlerPtr->stopNow() sets the iobuf stop_flag, which persists and would abort the NEXT collection immediately — but it does not break the poll loop, which is the finding's point. The EINTR path is real: poll returns -1, the code throws RuntimeException('poll failed - Interrupted system call'), which lands in the catch at 786-788 s</sub>

### TPX-075 · [HIGH] SIGTERM/SIGINT (and REST /?stop on an idle stream) cannot stop a running collection; forced SIGKILL then leaves a stale pid file

**Claim.** The signal handler only sets atomic flags; nothing invokes the stop_handlers or DataHandler::stopNow() on a signal, and the reader loop never checks gvars.stop: on receive timeout it re-loops unless gvars.stop_collect is set, and the buffer-level stop_flag is set only by REST callbacks. Therefore during 'collect': (a) SIGTERM/SIGINT have no effect until the raw stream ends by itself (Poco receiveBytes retries EINTR internally, and the main loop condition '!gvars.stop' is only evaluated between collections); (b) REST /?stop sets stop_flag via stopNow(), but if the TCP connection is open-but-idle the reader is stuck in the receiveBytes/timeout retry loop, which only exits on stop_collect - so /?stop also hangs. Operators then escalate to kill -9, which leaves /tmp/tpx3app.pid behind (see separate lockfile finding), blocking the next start.

**Proof.**

`src/main.cpp:64-69`
```cpp
inline static void sigint_handler([[maybe_unused]] int sig)
{
    if (sig == SIGUSR1)
        global::instance->restart = true;
    global::instance->stop = true;
}
```
`src/include/data_handler.h:213-218`
```cpp
} catch (Poco::TimeoutException&) {
    if (global::instance->stop_collect) {
        stopNow();
        return 0;
    }
}
```
`src/include/io_buf.h:260-263`
```cpp
inline void stop_now() noexcept
{
    stop_flag.store(true, std::memory_order_release);
}
```

**Impact.** systemd/ops shutdown of tpx3app during an active or stalled collection hangs until the stream ends; forced kill corrupts the lifecycle (stale pid file, no state cleanup). A stalled replay/test server (incident #2 conditions) makes even REST /?stop ineffective.

**Fix.** In DataHandler::readData's TimeoutException branch check 'stop_collect || global::instance->stop' (and stop_flag), and have the signal handler additionally be observed: e.g. main-loop-adjacent watchdog, or make /?stop set stop_collect too, and have sigint-driven stop call databuf.stop_now() via a signal-safe atomic checked in readData.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim (main.cpp:64-69, data_handler.h:213-218, io_buf.h:260-263). Verified: the signal handler only sets atomic flags; nothing invokes stop_handlers on a signal; during collect the main thread is blocked in dataHandler.await() (condition wait, signal-immune), and the reader's inner readData(buf,size) loop re-loops on Poco::TimeoutException unless stop_collect is set — gvars.stop and the iobuf stop_flag are never checked inside that loop (stop_flag is only observed in write_reservation, reached only after readData returns, which an open-but-idle connection prevents). So /?stop (wh</sub>

### TPX-076 · [MEDIUM] TimeRoi::SetTimeROI validates with && instead of ||, and negative/oversized values wrap through int->u64 conversion into garbage histogram parameters

**Claim.** SetTimeROI throws only if BOTH TRoiStep<=0 AND TRoiN<=0 ('must be positive' per its own doc); a single negative TRoiStep (or TRoiN) passes and is assigned to u64 fields, wrapping to ~1.8e19; TRoiStart is not validated at all. Analysis then computes TRoiStep_inv = 1.f/TRoiStep (~0) so all events collapse into bin 0, silently corrupting XES output. Reachable from Processing.ini in application mode (getInt returns negatives directly) and from /other-config PUT in server mode where getValue<u64> results are narrowed to the int parameters of SetTimeROI (values > 2^31 wrap to negative).

**Proof.**

`src/include/time_roi.h:37-42`
```cpp
if ((tRoiStep <= 0) && (tRoiN <= 0))
        throw std::invalid_argument("TRoiStep and TRoiN must be positive");

TRoiStart = tRoiStart;
TRoiStep = tRoiStep;
TRoiN = tRoiN;
```
`src/include/analysis.h:125-126`
```cpp
const_cast<TimeRoi&>(time_roi) = gvars.time_roi;
const_cast<float&>(TRoiStep_inv) = 1.f/time_roi.TRoiStep;
```

**Impact.** A typo'd or negative time-ROI value is silently accepted and produces scientifically invalid histograms for the whole scan with no error anywhere - a silent data-integrity failure.

**Fix.** Change the condition to '(tRoiStep <= 0) || (tRoiN <= 0)' and additionally reject tRoiStart < 0; in the /other-config callback validate ranges before narrowing (take the values as int64 and bounds-check against int/u64 limits).

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (time_roi.h:37-42, analysis.h:125-126). The '&&' means a single non-positive value passes; TRoiStart is never validated; all three int parameters are assigned to u64 fields (wrap). Verified the consumer: analysis.h:59 computes TP = (reltoa - TRoiStart) * TRoiStep_inv, so TRoiStep_inv ≈ 1/1.8e19 ≈ 0 collapses in-ROI events into bin 0 (and the wrapped TRoiEnd garbles the ROI bounds checks at :54-56). Both entry paths verified: processing.cpp:30-35 passes config.getInt() values (negatives flow through directly in application mode), and rest_callbacks.h:556-560 reads getValue<u64> </sub>

### TPX-077 · [MEDIUM] /other-config PUT is not atomic: output_uri and save_interval are committed before TRoi parsing/validation can throw

**Claim.** The /other-config PUT handler assigns gvars.output_uri and gvars.save_interval, and only afterwards extracts and validates the three TRoi values (getValue throws on missing/wrong-typed keys; SetTimeROI throws std::invalid_argument). A PUT that fails at the TRoi stage returns HTTP 400 but has already mutated output_uri and save_interval, leaving the process in a mixed old/new configuration with no way for the client to know which parts applied.

**Proof.**

`src/include/rest_callbacks.h:553-560`
```cpp
gvars.output_uri = obj->getValue<decltype(gvars.output_uri)>("output_uri");
gvars.save_interval = save_interval;
auto& time_roi = gvars.time_roi;
time_roi.SetTimeROI(
    obj->getValue<decltype(time_roi.TRoiStart)>("TRoiStart"),
    obj->getValue<decltype(time_roi.TRoiStep)>("TRoiStep"),
    obj->getValue<decltype(time_roi.TRoiN)>("TRoiN")
);
```

**Impact.** After a rejected reconfigure (client bug, schema drift), the next scan silently runs with a half-applied configuration - e.g. new output_uri/save_interval but old time ROI - producing data attributed to the wrong settings.

**Fix.** Parse and validate ALL values into locals first (including calling SetTimeROI on a temporary TimeRoi), then commit all globals in one block at the end of the handler.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (rest_callbacks.h:553-560). gvars.output_uri and gvars.save_interval are assigned at 553-554 before the three getValue calls (which throw Poco exceptions on missing/mistyped keys) and before SetTimeROI (which can throw std::invalid_argument). RestHandler::handleRequest catches these and returns HTTP 400 (rest_callbacks.h:110-116), leaving the globals half-mutated with no rollback. Claim holds exactly as stated; severity medium is appropriate.</sub>

### TPX-078 · [MEDIUM] global state string_view and REST-configurable globals are read/written across threads without a common lock (data race, TOCTOU on the 'config' gate)

**Claim.** global::instance->state (a non-atomic std::string_view, flagged 'TODOD: protect with lock' in the header) is written by the main thread inside set_state (under ws_mutex in server mode, no lock otherwise) but read by REST worker threads (/state, /?start, /pixel-map PUT, /other-config PUT) without taking ws_mutex - a C++ data race with possible torn pointer/size reads. Moreover the 'config' gate does not synchronize with the consumer: configLock is only taken REST-vs-REST; the main thread reads pix_map/time_roi/output_uri in processing::init and Analysis::Reset/xes::Manager::Reset with no lock, so a slow pixel-map PUT that passed the state check just before /?start flips the state can assign gvars.pix_map (a unique_ptr) concurrently with the main thread reading it.

**Proof.**

`src/include/global.h:114`
```cpp
std::string_view state{init};                                                //!< program state (TODOD: protect with lock, if necessary)
```
`src/include/rest_callbacks.h:477-482`
```cpp
auto& gvars = *global::instance;
if (gvars.state != "config")
    throw Poco::RuntimeException("not in config state");
std::unique_ptr<PixelIndexToEp> pmap{new PixelIndexToEp};
PixelIndexToEp::from(*pmap, in, PixelIndexToEp::JSON_STREAM);
gvars.pix_map = pmap->to_map();
```
`src/processing.cpp:52-53`
```cpp
if (gvars.pix_map == nullptr)
        throw Poco::RuntimeException("Pixelmap uninitialized");
```

**Impact.** Undefined behavior under concurrent control access; realistically, a retried/competing client (incident #1 shows these exist) can race a large pixel-map PUT against /?start, crashing the process (unique_ptr reassignment during read) or running the scan with a half-installed map.

**Fix.** Make state a std::atomic<const char*> or guard all state reads/writes with one mutex; have the main thread take global::configLock() across the config-consuming section of setup (processing::init + Analysis::Reset), and re-check state under that lock inside the PUT handlers after parsing.

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim, including the 'TODOD: protect with lock' comment at global.h:114. Verified both halves: (1) state is a non-atomic string_view written in set_state under ws_mutex (server mode) but read without any lock by /state (rest_callbacks.h:403), /?start (:429), /pixel-map PUT (:478), /other-config PUT (:546) — a C++ data race on a 16-byte object. (2) Grep confirms configLock is taken only inside rest_callbacks.h (:459, :476, :497, :525, :544); processing::init (processing.cpp:52) and Analysis::Reset (analysis.h:125-127) read pix_map/time_roi/output_uri with no lock, and /?start does not</sub>

### TPX-079 · [MEDIUM] Stale pid file after an unclean death permanently blocks startup (O_EXCL check, flock never used for staleness detection)

**Claim.** Lockfile creation uses O_CREAT|O_EXCL and treats EEXIST as 'another tpx3app is running'. After SIGKILL, OOM-kill, or power loss the file survives, and every subsequent start fails until someone manually deletes /tmp/tpx3app.pid - even though the code also takes an flock, which would be sufficient (and reliable) to detect a genuinely running instance. Combined with the collect-cannot-be-SIGTERMed finding, kill -9 is a realistic path, making this a self-inflicted deadlock of the deployment.

**Proof.**

`src/main.cpp:87-91`
```cpp
fd = open(lock_file.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
if (fd < 0) {
    if (errno == EEXIST) {
        throw Poco::RuntimeException(std::string{"lockfile exists at "} + lock_file + ", is another tpx3app already running?");
```

**Impact.** Beamline restart after any crash requires undocumented manual intervention (delete the pid file); automated restarts (systemd Restart=) fail permanently.

**Fix.** Open with O_CREAT (no O_EXCL), then flock(fd, LOCK_EX|LOCK_NB): if the lock succeeds, truncate and write the new pid (stale file is harmless); only if flock fails report 'already running'. Keep unlink-on-exit as-is.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (occupies main.cpp:87-90, within the cited 87-91). The lockfile is created with O_CREAT|O_EXCL and EEXIST is unconditionally treated as 'another tpx3app is running' (main.cpp:87-93); flock(LOCK_EX) at :95 runs only after successful exclusive creation, so it never serves as a staleness probe. Cleanup happens only via the destructor/atexit (:112-118, :128-139), which SIGKILL/OOM/power-loss bypass — the next start then always throws until the file is manually removed. Claim and severity (medium, operability) hold.</sub>

### TPX-080 · [MEDIUM] Real errors are silently discarded: set_error() with default empty argument clears last_error on stop and on stop_collect coincidence; DataForPeriod consumes the error destructively across threads

**Claim.** Three code paths erase genuine error information: (1) at the top of the server loop, if an error is pending but stop_collect is set, gvars.set_error() (default arg = "") wipes the message and 'except' is never entered - so a worker-thread failure that coincides with a client abort is invisible to /last-error; (2) on stop during the config wait, gvars.set_error() clears the error, so the final error check at main.cpp:803 passes and the process exits EXIT_OK despite a recorded failure; (3) xes::Manager::DataForPeriod/ReturnData throw using global::get_error(global::reset_error), which resets the shared error; with multiple analyser threads racing, later threads retrieve an empty string and set_error() an empty message, potentially leaving error_empty()==true after a real failure.

**Proof.**

`src/main.cpp:682-687`
```cpp
if (! gvars.error_empty()) {
    if (gvars.stop_collect) {   // prevent racing effects on data collection stop
        gvars.set_error();
    } else {
        set_state(global::except);
    }
}
```
`src/main.cpp:696-699`
```cpp
if (gvars.stop) {
    gvars.set_error();
    break; // exit server mode loop
}
```
`src/include/xes_data_manager.h:308-309`
```cpp
if (__builtin_expect(stopWriter, 0))
    throw Poco::RuntimeException(global::get_error(global::reset_error));
```

**Impact.** Failed scans can report success (exit 0, /last-error = 'none'), so partial/corrupt XES data is treated as good, and operators lose the diagnostic needed to fix the underlying failure.

**Fix.** Distinguish 'expected abort' from 'clear error': only clear the error if it was set by the abort itself (e.g. tag errors, or never auto-clear - let /last-error be the only reset). In DataForPeriod/ReturnData use get_error(no_error_reset) so the message survives for the main loop.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim; global.cpp confirms set_error's default arg overwrites last_error with "" and get_error(reset_error) swaps it out. Sub-claims (1) and (2) fully confirmed: main.cpp:682-687 wipes a pending error when stop_collect is set, and main.cpp:696-699 clears it on stop so the checks at :803/:818 pass and the process exits EXIT_OK. Sub-claim (3) is directionally right but its endpoint is overstated: xes_data_manager.h:308-309 does destructively consume the error, and racing analyser threads that then throw RuntimeException("") re-set it via set_error(ex.displayText()) in data_handler.h:43</sub>

### TPX-081 · [MEDIUM] Per-cycle ServerSocket rebind happens while the previous listening socket is still open; correctness depends on Poco's implicit SO_REUSEPORT, and the stale socket keeps accepting between scans

**Claim.** Each server-loop iteration executes serverSocket.reset(new ServerSocket{gvars.clientAddress}): unique_ptr::reset constructs the NEW socket (which binds and listens inside the Poco constructor) BEFORE destroying the OLD one, so two sockets are briefly bound to 8451; whether the second bind succeeds depends on the Poco version's bind() setting SO_REUSEPORT implicitly (setReuseAddress/setReusePort on lines 714-715 run only after the constructor has already bound and listened, so they cannot help the current bind). On Poco builds where bind(addr,true) sets only SO_REUSEADDR, every second scan fails with 'Address already in use' -> except state. Independently, between scans (config state) the previous socket remains listening: a SERVAL or replay server connecting early is accepted into a backlog nobody drains and silently reset later.

**Proof.**

`src/main.cpp:713-715`
```cpp
serverSocket.reset(new ServerSocket{gvars.clientAddress});
serverSocket->setReuseAddress(true);
serverSocket->setReusePort(true);
```

**Impact.** Fragile dependency on Poco library internals for the core scan cycle: a Poco upgrade/downgrade can make every second scan fail; stray early connections from SERVAL land in a dead socket's backlog and are dropped without any log.

**Fix.** Call serverSocket.reset() (close old) before constructing the new socket, or better: create the socket unbound, set reuse options, then bind+listen explicitly; close it explicitly when leaving collect.

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quote verbatim (main.cpp:713-715). Mechanically verified: the 'new ServerSocket{...}' expression is evaluated (Poco constructor binds and listens) before unique_ptr::reset destroys the old socket, so two sockets briefly contend for 8451; setReuseAddress/setReusePort at 714-715 run post-bind and cannot affect the just-completed bind; serverSocket is never closed between scans (dataStream.close() at :783 closes only the accepted connection), so the old listener keeps accepting into an undrained backlog through the whole config state. The Poco-version-dependence claim is plausible (two-arg bind's</sub>

### TPX-082 · [MEDIUM] Copy mode (--stream-to-file): a 300 ms receive lull aborts the copy, and the abort sets no global error, so the truncated capture looks successful

**Claim.** main() sets a 300 ms receive timeout on the data stream for both modes, but CopyHandler::readBytes has no TimeoutException handling (unlike DataHandler::readData): the first lull >300 ms throws, is caught by the generic catch in CopyHandler::readData, which calls stopNow() and only LOGS - it never calls global::set_error. main's copy path then completes normally (await/logOutput), gvars.error_empty() stays true, and the process exits EXIT_OK with a silently truncated raw capture.

**Proof.**

`src/main.cpp:757`
```cpp
dataStream.setReceiveTimeout(gvars.collect_timeout);
```
`src/include/copy_handler.h:52-55`
```cpp
int numRead = dataStream.receiveBytes(&static_cast<char*>(buf)[numBytes], size - numBytes);
if (numRead == 0)
    break;
numBytes += numRead;
```
`src/include/copy_handler.h:105-107`
```cpp
} catch (Poco::Exception& ex) {
    stopNow();
    logger << "reader exception: " << ex.displayText() << log_critical;
```

**Impact.** Reference raw-stream captures (used for the replay test server and debugging) are silently cut off at the first beam pause or stream hiccup, and exit status reports success.

**Fix.** Mirror DataHandler::readData's TimeoutException handling in CopyHandler::readBytes (continue unless stop requested), and call global::set_error in CopyHandler's catch blocks so main reports the failure.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp:757; copy_handler.h:52-55, 105-107). Verified: setReceiveTimeout(collect_timeout=300000us) is applied before the mode branch, so it governs copy mode too; CopyHandler::readBytes has no Poco::TimeoutException handler (contrast data_handler.h:213-218), so the first >300ms lull propagates to readData's generic catch at copy_handler.h:105-111, which calls stopNow() and logs but never calls global::set_error (grep confirms no set_error anywhere in copy_handler.h). main's copy path then runs await()/logOutput() normally, gvars.error_empty() stays true, and main.cpp:818-819 </sub>

### TPX-083 · [MEDIUM] except is only a transient state in server mode, and /last-error is a destructive single-consumer read

**Claim.** After an exception, the catch sets state 'except', but the very next server-loop iteration immediately calls set_state(global::config) (via the wait-for-start block), so 'except' is visible on /state only for microseconds unless the client happens to poll in that window; the durable failure record is /last-error, which resets on read (get_error(reset_error)), so a second reader (second client, monitoring, or the xes writer path that also consumes it) destroys the evidence for the first. This is the code basis for incident #4 (client not modeling 'except') and makes error handling fragile for any polling client.

**Proof.**

`src/main.cpp:690-692`
```cpp
if (server_mode) {  // wait for start signal
    using namespace std::chrono_literals;
    set_state(global::config);
```
`src/include/rest_callbacks.h:386-387`
```cpp
std::string err = global::get_error(global::reset_error);
json.startObject();
```

**Impact.** A polling client can miss the failure entirely and interpret the follow-up 'config' as a clean cycle; with multiple observers, /last-error races mean the party that needs the error message may read 'none'.

**Fix.** Stay in 'except' until the client acknowledges (e.g. require /?start or an explicit /?clear-error to leave except), or include last-error in the /state payload non-destructively and add an explicit reset endpoint.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (main.cpp:690-692, rest_callbacks.h:386-387). Verified the flow: catch at main.cpp:786-799 sets state 'except'; the loop condition (server_mode && !stop) is still true, and the very next iteration reaches set_state(global::config) at :692 with nothing in between that waits, so 'except' is observable via polled /state only for a sub-millisecond window (WS-push clients do receive the except frame, but the finding is explicitly about polling clients, matching incident #4). /last-error uses get_error(reset_error) (global.cpp:24-35 swaps the message out), so any second reader — incl</sub>

### TPX-084 · [MEDIUM] DataHandler::await() is not exception-safe: a throwing dataStream.shutdown() resets IO buffers under running analyser threads and desynchronizes the finish signals

**Claim.** await() constructs an iobuf::resetter whose destructor unconditionally calls databuf.reset() at scope exit; between the two waits it calls dataStream.shutdown()/close(), which Poco implements as throwing calls (NetException on e.g. ENOTCONN after a peer RST - realistic when a measurement is aborted, cf. incident #2). If shutdown() throws: (a) the resetter destructor reinitializes the jar free-list while analyser threads still traverse jar pointers (data race on live buffers); (b) analysis_finished is never wait_reset and Analysis::await() in main.cpp (line 779) is skipped, leaving stale finished-signal counts that make the NEXT cycle's await return prematurely or hang.

**Proof.**

`src/include/data_handler.h:541-548`
```cpp
inline void await()
{
    iobuf::resetter reset(databuf);
    reader_finished.wait_reset();
    dataStream.shutdown();
    dataStream.close();
    analysis_finished.wait_reset();
}
```
`src/include/io_buf.h:457-460`
```cpp
inline ~resetter() noexcept
{
    bufs.reset();
}
```

**Impact.** One RST-terminated connection can corrupt the pipeline's internal state so that a later scan hangs or completes instantly with no data - an unrecoverable wedge that needs a process restart and is not documented as such.

**Fix.** Wrap shutdown()/close() in try/catch (ignore NetException - the socket is being torn down anyway), and always execute analysis_finished.wait_reset() before the buffer reset (move the resetter after both waits or make the sequence noexcept).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (data_handler.h:541-548, io_buf.h:457-460). Poco's SocketImpl::shutdown() reports failure by throwing (NetException on e.g. ENOTCONN after a peer RST), which is realistic for aborted measurements. If it throws: the resetter's destructor runs during unwinding and calls databuf.reset() (rewiring jar next-pointers/levels/free list with no synchronization, io_buf.h:241-255) while analyser threads — whose completion is only awaited by the skipped analysis_finished.wait_reset() — may still traverse jars; and thread_signal.h confirms analysis_finished is multi<send> whose 'signal' fla</sub>

### TPX-085 · [MEDIUM] Ini config file silently ignores save-interval and cpu-affinity keys (and any typo'd key)

**Claim.** handleConfigFile maps only a fixed subset of options (use-syslog, loglevel, server, address, control, bpc-file, dacs-file, stream-to-file, pid-file, buf-size, reorder-queue-size, server-mode). 'save-interval' and 'cpu-affinity' - both valid CLI options - are absent, and unknown keys produce no diagnostic, so a config file containing save-interval=200000 or a misspelled key runs silently with defaults. The related runtime check in processing::init also emits a wrong diagnostic (it prints the current save_interval as the bound rather than min_save_interval).

**Proof.**

`src/main.cpp:554-561`
```cpp
std::uint32_t argint;
argint = cf.getUInt("buf-size", 0);
if (argint > 0)
    handleNumber("buf-size", std::to_string(argint));
argint = cf.getUInt("reorder-queue-size", 0);
if (argint > 0)
    handleNumber("reorder-queue-size", std::to_string(argint));
```
`src/processing.cpp:24-25`
```cpp
if (gvars.save_interval < gvars.min_save_interval)
        throw Poco::RuntimeException(std::string{"save_interval below "} + std::to_string(gvars.save_interval));
```

**Impact.** Deployments driven by ini files (the documented mode for beamline operation) silently run with wrong histogram save cadence or without the intended CPU pinning; misconfigurations are undetectable from logs.

**Fix.** Iterate the ini keys and dispatch through the same option table as the CLI (or at least add save-interval and cpu-affinity), and log a warning for every unrecognized key. Fix the error text to include min_save_interval.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (occupies main.cpp:555-561 within the cited 554-561). handleConfigFile (main.cpp:513-572) dispatches exactly the listed fixed subset — use-syslog, loglevel, server, address, control, bpc-file, dacs-file, stream-to-file, pid-file, buf-size, reorder-queue-size, server-mode — with no save-interval, no cpu-affinity, and no iteration over the file's keys, so unknown/typo'd keys produce no diagnostic. The related diagnostic bug is also verbatim at processing.cpp:24-25: it interpolates gvars.save_interval (the offending current value) where min_save_interval belongs, yielding a nonsens</sub>

### TPX-086 · [LOW] 1 ms busy-poll while waiting in config state instead of a condition/event wait

**Claim.** In server mode the main thread waits for /?start by polling two atomics with a 1 ms sleep, i.e. ~1000 wakeups/s for the entire (potentially hours-long) idle time between scans; the same pattern gives /?start a fixed 1 ms latency floor and burns CPU/power on the DAQ host shared with the collectors.

**Proof.**

`src/main.cpp:693-695`
```cpp
while (!gvars.stop && !gvars.start) {
    std::this_thread::sleep_for(1ms);
}
```

**Impact.** Constant background CPU wakeups on the DAQ machine; no functional failure, but it is exactly the pattern that masks lost-wakeup bugs if flags are ever made non-atomic.

**Fix.** Use a std::condition_variable (or the existing thread_signal utilities) notified by the /?start, /?stop and signal paths; alternatively raise the sleep to ~50 ms since start latency is not critical.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (main.cpp:693-695). In server mode the main thread polls two atomics with a 1 ms sleep for the entire idle time between scans (~1000 wakeups/s) and imposes a ~1 ms latency floor on /?start. The codebase's own thread_signal utilities show a condition-variable alternative exists. Low severity is correct — no functional failure.</sub>

### TPX-087 · [LOW] /?kill calls std::exit on an HTTP worker thread while collector threads run; unauthenticated remote process termination

**Claim.** The /?kill=true GET handler executes std::exit(EXIT_FAILURE) directly on the REST worker thread: static destructors and atexit handlers run concurrently with live reader/analyser/writer threads (undefined behavior, possible crash-in-exit instead of clean termination), the HTTP response is never sent, and - like the whole control interface - the endpoint has no authentication, so anyone who can reach controlAddress (configurable to a routable interface) can kill or stop the DAQ.

**Proof.**

`src/include/rest_callbacks.h:370-373`
```cpp
global::instance->get_callbacks["/?kill"] = [](const std::string& val) -> std::string {
    if (val == "true")
        std::exit(EXIT_FAILURE);
```

**Impact.** Kill requests may themselves crash messily (worse diagnostics), and on a shared beamline network the control plane allows unauthenticated stop/kill/reconfigure of the detector backend.

**Fix.** Implement kill as SIGTERM-to-self (raise(SIGTERM)) or _Exit after minimal cleanup; document that controlAddress must stay on localhost or add a token check for destructive endpoints.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim (occupies rest_callbacks.h:370-372 within the cited 370-373). std::exit runs on the REST worker thread: it does not unwind any stack, so the reader/analyser/writer threads (locals of Tpx3App::main) keep running while static destructors (global::instance, logger internals) and atexit handlers execute — UB with crash-in-exit potential; the HTTP response is never sent because std::exit fires inside the callback before RestHandler reaches response.send() (:118-119). Verified there is no authentication anywhere in RestHandler, and controlAddress is CLI/ini-configurable to any interfa</sub>


## Build, deployment, operations & test

*14 findings: 2 high, 7 medium, 4 low, 1 info*

### TPX-088 · [HIGH] Replay test server shuts down the whole process on any send error (broken pipe on measurement abort)

**Claim.** Any exception thrown while sending replay data (e.g. ECONNRESET/EPIPE when tpx3app aborts a measurement mid-replay) propagates out of the send loop to the catch blocks at the end of send_data(), which then sets stop_server; main() is blocked on stop_server.await(true) and reacts by joining the sender and stopping the HTTP server, terminating the process. This is the exact code basis for known incident 2 (every subsequent scan fails with connection refused on 8080). The 'sender stalled' RuntimeException (32 consecutive zero-byte sends) takes the same fatal path. Additionally, if the linked Poco build does not pass MSG_NOSIGNAL on send (assumption, platform-dependent), the process can die even earlier from an unhandled SIGPIPE — no handler is installed anywhere in test_server.cpp.

**Proof.**

`src/test_server.cpp:426, 432-434`
```cpp
int sz = con.sendBytes(data, send_size);
...
if (sz <= 0) {
    if (++stall_count >= 32u)
        throw RuntimeException("sender stalled");
```
`src/test_server.cpp:453-460`
```cpp
} catch (Poco::Exception& ex) {
    std::cerr << "data sender exception: " << ex.displayText() << '\n';
...
stop_server.set_notify(true);
```
`src/test_server.cpp:973-978`
```cpp
stop_server.await(true);
if (data_sender.joinable()) {
    std::cout << "joining sender thread ...\n";
    data_sender.join();
}
server.stop();
```
`container/README.md:77-79`
```cpp
Note that `tpx3-server` exits when a measurement is aborted mid-replay (its data sender stops on a broken pipe) —
symptom on the tpx3-app side is an `except` state with "ASI server PUT request for /server/destination failed -
Connection refused".
```

**Impact.** Aborting or failing a single measurement kills the replay server; all subsequent integration-test scans fail with connection refused on port 8080 until someone manually restarts the container. This has already caused real cascading scan failures during integration testing; the README documents a manual workaround instead of a fix.

**Fix.** Wrap the per-measurement block (from StreamSocket con{destination}; through con.shutdown()) in its own try/catch inside the do-loop: on Poco::Exception, log, close the socket, fd.rewind(), reset per-measurement signals, and 'continue' back to waiting for the next /measurement/start instead of falling through to stop_server. Also install std::signal(SIGPIPE, SIG_IGN) at the top of main(), and consider replacing the fatal 'sender stalled' throw with an abort of only the current replay. The file's own TODO (lines 5-7: 'don't end server loop after start') acknowledges this.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All four quotes verbatim at cited lines. Re-derived the path: con.sendBytes (line 426) throws Poco NetException on ECONNRESET/EPIPE inside the send loop; nothing between the loop and the function-level try/catch (377/453-459) handles it per-measurement; line 460 stop_server.set_notify(true) runs unconditionally after the catch; main() blocked at stop_server.await(true) (line 973) then joins the sender and calls server.stop() (978), ending the process. Poco::RuntimeException('sender stalled', line 434) is caught by catch(Poco::Exception&) at 453 — same fatal path. Grep confirms no SIGPIPE handl</sub>

### TPX-089 · [HIGH] systemd units: BindsTo without After gives no start ordering, and a serval stop/restart leaves tpx3app permanently down

**Claim.** tpx3app.service declares BindsTo=serval.service but no After=serval.service. Per systemd semantics: (1) there is no startup ordering, so tpx3app can start (and try to contact the ASI server, which the README states it does at startup) before serval is listening — serval's ExecStartPost sleep-5 readiness hack is ineffective because delaying the start job only matters to units ordered After= it; (2) when serval is stopped or restarted (systemctl restart serval, or a serval crash reaching inactive/failed), the BindsTo dependency stops tpx3app as a deliberate systemd stop, to which Restart=always does not apply — serval restarts itself (Restart=always) but nothing ever starts tpx3app again. Additionally, if tpx3app exits when serval is unreachable at startup (assumption consistent with the README's 'start tpx3-server first'), Restart=always + RestartSec=2 hits the default start-limit (5 starts / 10 s) within ~10 seconds and the unit enters permanent failed state.

**Proof.**

`unit_files/tpx3app.service:1-3`
```cpp
[Unit]
Description=tpx3app - TimePix detector stream handler
BindsTo=serval.service
```
`unit_files/tpx3app.service:12-13`
```cpp
Restart=always
RestartSec=2
```
`unit_files/serval.service:14-16`
```cpp
ExecStartPost=/usr/bin/sleep 5
Restart=always
RestartSec=10
```
`container/README.md:41-42`
```cpp
Start `tpx3-server` *first* —
`tpx3-app` contacts the ASI server at startup:
```

**Impact.** After any serval crash or maintenance restart, the detector backend stays down until an operator manually runs systemctl start tpx3app — every scan fails in the meantime (same failure signature as known incident 2: connection refused / except state). Boot-time races between the two services produce intermittent startup failures.

**Fix.** In tpx3app.service add 'After=serval.service' next to BindsTo. Make serval revive its consumer: add 'Upholds=tpx3app.service' to serval's [Unit] (systemd >= 249), or use PartOf=serval.service in tpx3app so restarts propagate. Guard against the start-limit trap with 'StartLimitIntervalSec=0' (or StartLimitBurst raised) in tpx3app.service. Replace the sleep-5 hack with a real readiness check (ExecStartPost script polling http://localhost:8080/dashboard, or Type=notify wrapper).

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (tpx3app.service lines 1-3 have BindsTo=serval.service and no After=; serval.service 14-16; README 41-42). Confirmed: no startup ordering, so the ExecStartPost sleep-5 cannot gate tpx3app (ordering-only effect); a serval crash momentarily passes through inactive/failed before its auto-restart, BindsTo stop-propagates that to tpx3app as a deliberate stop (Restart=always never applies to systemd-initiated stops), and serval's self-restart pulls nothing back in — tpx3app stays down until manual start. The stop-then-start case likewise leaves it down (serval's [Install] RequiredBy </sub>

### TPX-090 · [MEDIUM] Replay server latches start/stop signals: a stop received while idle silently truncates the NEXT measurement

**Claim.** GET /measurement/stop sets the sticky boolean signal stop_collect=true, but the sender only consumes it (stop_collect.reset(false)) inside the active send loop. If the stop request arrives after a replay already completed (a normal client always sends stop at scan end, or during an abort race), the flag stays latched; on the next /measurement/start the very first loop iteration consumes the stale flag and breaks after sending at most one 4 MiB chunk, producing a silently truncated replay. Symmetrically, start_collect set during an active replay is latched and triggers an immediate unrequested second replay after the current one finishes.

**Proof.**

`src/test_server.cpp:635-639`
```cpp
inline void get_measurement_stop([[maybe_unused]] HTTPServerRequest& request, HTTPServerResponse& response)
{
    stop_collect = true;
```
`src/test_server.cpp:438-441`
```cpp
if (stop_server)
    goto stop_reader;
if (stop_collect.reset(false))
    break;
```
`src/test_server.cpp:386-390`
```cpp
do {
    if (stop_server)
        goto stop_reader;
    if (start_collect.reset(false))
        break;
```

**Impact.** Repeated measurements against the replay server are not robust: a routine stop-after-completion poisons the following scan with short/incomplete event data, which looks like a detector or decoder fault (wrong histogram counts) rather than a test-server bug.

**Fix.** Clear both start_collect and stop_collect (reset(false)) at the top of each measurement cycle, immediately after the start signal is consumed at line 389, and also drain stop_collect in the idle wait loop. Alternatively replace the two booleans with a single state variable (IDLE/RUNNING/STOP_REQUESTED) protected by one mutex.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (get_measurement_stop sets stop_collect=true at line 637; consumed only via stop_collect.reset(false) at line 440 inside the active send loop; start consumed at 389). Counter-evidence search: neither signal is cleared anywhere else — after a replay completes (sent==fd.len) the code goes to con.shutdown()/fd.rewind() and back to the idle wait, never draining stop_collect; the idle loop (386-393) checks only stop_server and start_collect. So a stop latched while idle survives to the next start and breaks the send loop after the first chunk (read_bytes caps at BUF_SIZE=4 MiB, line</sub>

### TPX-091 · [MEDIUM] Container images are built from unpinned branch HEAD cloned over the network — unreproducible deployments

**Claim.** Both Dockerfiles clone the GitHub repository at whatever the tip of the 'dev' branch is at build time (ARG REPO_BRANCH=dev, git clone --branch ... --single-branch), with no commit SHA pin, no tag, and no checksum. The base image ubuntu:24.04 and all apt packages (libpoco-dev, libjemalloc-dev, build-essential) are also unpinned. Consequently two builds of the 'same' image can contain different code and different library versions, and the image never contains uncommitted/unpushed local changes being tested — the local checkout is ignored entirely (the build context passed as '.' in the README is unused).

**Proof.**

`container/tpx3app-container.docker:8, 17-19`
```cpp
ARG REPO_BRANCH=dev
...
&& mkdir build && cd build \
&& git clone --branch ${REPO_BRANCH} --single-branch https://github.com/paulscherrerinstitute/TimePixFly.git . \
&& chmod +x compile.sh && GENERIC=1 ./compile.sh
```
`container/server-container.docker:18-19`
```cpp
&& git clone --branch ${REPO_BRANCH} --single-branch https://github.com/paulscherrerinstitute/TimePixFly.git . \
&& chmod +x compile.sh && GENERIC=1 ./compile.sh server
```
`container/tpx3app-container.docker:2`
```cpp
FROM ubuntu:24.04 AS builder
```

**Impact.** A container rebuilt to reproduce a beamline incident may silently contain different code than the image that misbehaved; rolling 'the same' image to another host can change behavior; images built while dev moves are untraceable except via the runtime /version REST reply. Local integration fixes are invisible to container builds until pushed to GitHub, a classic source of 'works locally, fails in container' confusion.

**Fix.** Add ARG REPO_COMMIT and 'git checkout ${REPO_COMMIT}' (fail if unset), or better: COPY the build context into the builder stage so the image is built from the checkout being tested. Pin the base image by digest (ubuntu:24.04@sha256:...), tag pushed images with the git SHA, and record the SHA in an OCI label (org.opencontainers.image.revision).

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim: both Dockerfiles clone github.com/paulscherrerinstitute/TimePixFly at ARG REPO_BRANCH=dev with no SHA pin (tpx3app-container.docker lines 8, 17-19; server-container.docker 18-19), base is unpinned ubuntu:24.04, apt packages unpinned, and neither Dockerfile has any COPY from the build context, so the '.' context in the README docker build commands (lines 33-34) is indeed unused and local changes never enter the image. Claim fully accurate. Severity adjusted high->medium: the production deployment path is the systemd unit executing /home/asi/git/tpx3app from a local checkout</sub>

### TPX-092 · [MEDIUM] No CI and no automated test execution; unit tests cover only 3 components, none of the failure-prone network/REST/WS code

**Claim.** The repository contains no CI configuration of any kind (verified by full enumeration: no .github/, no GitLab/Jenkins/Travis files, no Makefile/CMake — only compile.sh). The 'test' binary exists only as a manual compile.sh target and registers exactly three test units (pixmap::energypoints, cpu_mask::parse, iobuf::subreservation) plus two AVX units that are compiled out unless USE_AVX is set and the build CPU has AVX2 — so they are never built in the GENERIC=1 container builds. Nothing exercises the REST callbacks, the WebSocket state stream (known incident 1 lives there), the TCP decode path end-to-end, or the JSON frame writer. The Julia scripts in test_data/ (check_histogram.jl, check_sequence.jl, check_tdc_clock.jl, check_end_readout.jl, count_toa.jl, aggregated-energy.jl) and generate_data/ (generate_data.jl, generate_map.jl, save_raw_stream.jl) are standalone manual verification tools invoked by no harness.

**Proof.**

`compile.sh:108-111`
```cpp
"test")
    cmd="${CXX} -I src/include src/test.cpp src/energy_points.cpp src/global.cpp -std=c++17 ${TEST_FLAGS} ${LDFLAGS} -o test"
```
`compile.sh:132`
```cpp
echo "  test           some unit tests for parts of the queueing code"
```
`src/test.cpp:722-736`
```cpp
tests.insert({
    "pixmap::energypoints",
...
    "cpu_mask::parse",
...
    "iobuf::subreservation",
```
`src/test.cpp:13-14`
```cpp
#if defined(__AVX2__) && defined(AVX_DECODE)
    #define USE_AVX
```

**Impact.** All four known live incidents (single WS client slot, replay-server death, pixel-map chip-count rejection, undeclared 'except' state) occurred in code with zero automated coverage. Regressions in the protocol/contract surface can only be caught by live beamline integration runs, which is exactly where they have been caught so far — at the cost of failed scans.

**Fix.** Add a CI workflow (GitHub Actions, since the repo is on github.com/paulscherrerinstitute) that builds tpx3app, server, and test with GENERIC=1 in the ubuntu:24.04 toolchain, runs ./test, and adds a smoke test: start ./server with a small generated raw file, start ./tpx3app -S, drive one measurement via curl (the container/README.md curl checks are a ready-made template), and assert on the emitted JSON frames. Extend test.cpp (or add a second test binary) with tests for rest_callbacks.h handlers and the WS state push.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Independently re-enumerated the repo: no .github/, no *.yml/yaml CI files, no Makefile/CMakeLists/Jenkinsfile/.travis/.gitlab anywhere — compile.sh is the only build entry (quote at 108-111 and 132 verbatim). test.cpp init_tests() (722-749) registers exactly pixmap::energypoints, cpu_mask::parse, iobuf::subreservation, plus two AVX units gated by USE_AVX requiring both __AVX2__ and AVX_DECODE (lines 13-14 verbatim) — absent under GENERIC=1 (and the container builds never build the test target at all). test.cpp includes only global.h/cpu_mask.h/subreservation.h/event_type.h — no REST, WS, TCP-d</sub>

### TPX-093 · [MEDIUM] No release versioning: /version REST reply is 'branch shorthash date' with no tags and no dirty-tree marker; production runs from a live git working tree

**Claim.** compile.sh generates src/include/version.h at build time from 'git branch --show-current' plus the last commit short hash and date; version.h is gitignored. There are no semver tags or release artifacts anywhere (archive.sh producing date-prefixed tarballs is the only 'release' mechanism). The version string does not record whether the working tree was dirty, so a binary compiled from locally modified sources reports the clean last-commit hash via GET /version. The production systemd unit executes the binary straight out of /home/asi/git, i.e. deployment is 'whatever was last compiled in that checkout', with no link between the running binary and a released, reviewable state. In a detached-HEAD checkout 'git branch --show-current' is empty, degrading the version to ' hash date'.

**Proof.**

`compile.sh:70`
```cpp
VERSION="$(git branch --show-current) $(git log -n1 --format="%h %as")"
```
`src/include/rest_callbacks.h:413-415`
```cpp
global::instance->get_callbacks["/version"] = []([[maybe_unused]] const std::string& val) -> std::string {
    std::ostringstream oss;
    oss << R"({"type":"VersionString","version":")" << VERSION << R"("})";
```
`unit_files/tpx3app.service:10-11`
```cpp
WorkingDirectory=/home/asi/git
ExecStart=/home/asi/git/tpx3app --control=%H.psi.ch:8452 --initial-period=7633 --max-period-queues=6 --loglevel=debug -S
```
`archive.sh:47`
```cpp
CMD="tar czf $BACKUP_FILE src/*.cpp src/include/*.h compile.sh archive.sh LICENSE README.md generate_data/*.jl test_data/*.jl container/*.docker doc unit_files"
```

**Impact.** When debugging a beamline incident, GET /version can actively mislead: it may name a commit whose code is not what is running (dirty tree), and there is no tag to check out to reproduce the deployed binary. Rollback means 'git checkout something and recompile in the production home directory'.

**Fix.** Use 'git describe --tags --always --dirty' in compile.sh (and start tagging releases, e.g. v0.x.y). Deploy versioned binaries (or the pinned container images from the previous finding) to a path like /opt/tpx3app/<version>/ with a symlink, instead of executing from a mutable git working tree; keep archive.sh only as a convenience backup, not as the release mechanism.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All quotes verbatim (compile.sh line 70, rest_callbacks.h 413-415, tpx3app.service 10-11, archive.sh 47). version.h confirmed gitignored. git tag shows exactly one tag, 'previous-master' — a branch-preservation marker, not a semver/release tag, so 'no semver tags or release artifacts' holds as stated. The VERSION format ('branch %h %as') indeed carries no --dirty marker, and 'git branch --show-current' prints empty on detached HEAD, degrading the string exactly as claimed. Production ExecStart runs the binary from the mutable /home/asi/git checkout. Severity medium appropriate.</sub>

### TPX-094 · [MEDIUM] Unauthenticated control plane exposed on host network / public FQDN; containers run as root; remote /kill endpoint

**Claim.** The production unit binds tpx3app's unauthenticated REST control (state changes, start, shutdown-class commands) to the host's public FQDN (%H.psi.ch:8452), not localhost. The documented container deployment uses --network=host with no USER directive in either Dockerfile, so both apps run as root in the host network namespace. The replay server additionally exposes GET /kill, which calls std::exit(EXIT_SUCCESS) without any authentication, and GET /stop//server/shutdown with the same trust model (mitigated only by its default localhost:8080 bind — a -b 0.0.0.0 bind removes that). The runtime container stage also installs -dev packages (headers, extra attack surface) instead of runtime libraries.

**Proof.**

`unit_files/tpx3app.service:11`
```cpp
ExecStart=/home/asi/git/tpx3app --control=%H.psi.ch:8452 --initial-period=7633 --max-period-queues=6 --loglevel=debug -S
```
`container/README.md:15-19`
```cpp
podman run --rm --network=host ... --name=tpx3-server tpx3-server ...
podman run --rm --network=host ... --name=tpx3-app tpx3-app -S -ldebug

The --network=host lets the container use the same network namespace as the host.
```
`src/test_server.cpp:733-737`
```cpp
inline void get_kill([[maybe_unused]] HTTPServerRequest& request, [[maybe_unused]] HTTPServerResponse& response)
{
    std::cout << "kill server\n";
    std::exit(EXIT_SUCCESS);
}
```
`container/tpx3app-container.docker:27-30, 32-37`
```cpp
RUN apt-get update && apt-get install -y \
    libpoco-dev \
    libjemalloc-dev \
...
WORKDIR /app
...
ENTRYPOINT ["./tpx3app"]
```

**Impact.** Any host that can reach the beamline machine on port 8452 can reconfigure, start, or stop the DAQ mid-scan (trivial denial of service / data-loss vector); a compromise of either process is a root compromise inside the host network namespace. The /kill endpoint makes remote process termination a one-line curl.

**Fix.** Bind the control REST to localhost or a dedicated instrumentation VLAN interface unless remote control is required, or front it with an authenticating reverse proxy; add 'USER' (a non-root uid) to both Dockerfiles and install libpocofoundation/libpoconet/... runtime packages instead of -dev in the final stage; drop or gate /kill behind a bind-address check; prefer a pod-internal network over --network=host where the replay setup allows it.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (unit line 11 binds control to %H.psi.ch:8452; README 15-19 --network=host; get_kill at test_server.cpp 733-737 calls std::exit(EXIT_SUCCESS) with no auth; /kill and /stop + /server/shutdown registered at 793-795). Verified neither Dockerfile contains a USER directive (both read in full) so containers run as root, and both final stages install libpoco-dev/libjemalloc-dev rather than runtime libs (lines 27-30). Grep of rest_callbacks.h and main.cpp finds no auth/token/credential handling anywhere in the REST layer. Default bind localhost:8080 for the replay server (line 75) is c</sub>

### TPX-095 · [MEDIUM] Production data path compiled with -ffast-math; -march=native default makes binaries host-specific (GENERIC=1 only omits it)

**Claim.** The default (release) build uses -O3 -ffast-math -fno-trapping-math -flto=auto. -ffast-math implies -ffinite-math-only and value-unsafe reassociation: any NaN/Inf checks in the float histogram/weight path are legally optimized away and accumulation order may change results, in a program whose core output is floating-point weighted XES histograms (pixel-map weights are floats, cf. EpPart{1,.8} in the unit tests). Independently, NATIVE_FLAGS defaults to -march=native, so a tpx3app or server binary compiled on one machine (e.g. a newer build host) can die with SIGILL on an older production CPU; GENERIC=1 changes nothing except omitting -march=native. There is also no sanitizer build target for debugging the known concurrency-sensitive code.

**Proof.**

`compile.sh:20-22`
```cpp
if [ -z "${DEBUG}" ]; then
    SPEED_FLAGS+=" -O3 -ffast-math -DNDEBUG -flto=auto"
    SPEED_FLAGS+=" -fno-trapping-math -fno-semantic-interposition -funroll-loops -ftree-vectorize -finline-functions"
```
`compile.sh:14-18`
```cpp
if [ -n "${GENERIC}" ]; then
    NATIVE_FLAGS=""
else
    NATIVE_FLAGS="-march=native"
fi
```
`src/test.cpp:354`
```cpp
check_eq(unit, t, pite.at({0,1}).part[1], EpPart{1,.8});
```

**Impact.** Subtly different histogram values between builds/compilers are possible and NaN propagation from a corrupt pixel map would be undetectable at runtime (isnan is constant-folded to false under -ffinite-math-only); copying a natively-built binary between hosts (the /home/asi/git deployment style invites this) risks SIGILL crashes at scan time.

**Fix.** Replace -ffast-math with the safe subset actually needed (e.g. -fno-math-errno -fno-signed-zeros) or at minimum add -fno-finite-math-only if NaN checking matters; add a compile.sh SANITIZE=1 option (-fsanitize=address,undefined with -O1) and use it in CI; default GENERIC=1 for any binary that may be copied between hosts and document -march=native as an opt-in (NATIVE=1).

<sub>Verification: **confirmed**. Author confidence: medium.</sub>

<sub>Verifier: Quotes verbatim (compile.sh 20-22 release flags incl. -O3 -ffast-math; 14-18 shows GENERIC solely empties NATIVE_FLAGS, otherwise -march=native; test.cpp 354). -ffast-math does imply -ffinite-math-only and value-unsafe reassociation in GCC; pixel-map weights are float (energy_points.h line 21: 'float weight'). One nuance: grep finds no isnan/isinf/isfinite anywhere in src/, so the 'NaN checks optimized away' point is about checks one might add, not existing ones — the claim's conditional phrasing ('any NaN/Inf checks ... are legally optimized away') survives, and reassociation-order nondetermi</sub>

### TPX-096 · [MEDIUM] Replay server binds port 8080 during static initialization — port conflict aborts via std::terminate, and --help/-b still require 8080 free

**Claim.** bind_to is a namespace-scope global constructed before main(): ServerSocket's constructor binds and listens on localhost:8080 at static-init time. If the port is taken (e.g. a leftover replay server, or serval itself, which also uses 8080), the Poco NetException is thrown outside any try block and the process dies via std::terminate with no usable diagnostic. Because the -b option only reassigns bind_to inside the option handler, even 'server --help' or 'server -b other:port' first requires binding localhost:8080.

**Proof.**

`src/test_server.cpp:75`
```cpp
ServerSocket bind_to{SocketAddress{"localhost:8080"}}; //!< Server binding address
```
`src/test_server.cpp:829-832`
```cpp
else if (name == "layout")
    layout_name = value;
else if (name == "bind")
    bind_to = ServerSocket{SocketAddress{value}};
```

**Impact.** On the machine where serval (real ASI server, port 8080) runs, the replay server cannot start at all and crashes with an unhandled-exception abort instead of a clear error; restart races after the known exit-on-abort failure produce confusing terminate() messages rather than 'address already in use: use -b'.

**Fix.** Make bind_to an unbound default-constructed ServerSocket (or a plain SocketAddress) and perform the bind inside main()'s try block after argument parsing, e.g. ServerSocket sock; sock.bind(addr, true); sock.listen(); so port conflicts produce the existing 'Error: ...' message path and -b takes effect before any bind.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (line 75: ServerSocket bind_to{SocketAddress{"localhost:8080"}} at namespace scope inside the anonymous namespace opened at line 71; option handler reassignment at 831-832). Poco's ServerSocket(const SocketAddress&) constructor binds and listens immediately, so the bind happens during dynamic initialization of a non-local static, before main() and outside any try scope — a NetException there reaches std::terminate with no argument parsing having run, so --help and -b cannot take effect first. serval does own port 8080 on the production host (serval.service ExecStop curls localh</sub>

### TPX-097 · [LOW] Replay sender latent defects: partial-send data loss in default (non-mmap) mode, wrong mmap failure check, wrong errno in open error

**Claim.** (1) In the default non-mmap mode, read_bytes() reads sequentially from the file descriptor while the caller tracks progress with 'sent' (bytes accepted by the socket). If sendBytes() ever accepts fewer bytes than were read (possible on a blocking socket when interrupted by a signal or if a send timeout is ever configured — stated assumption: rare in the current setup), the unsent tail of the buffer is silently skipped (file position has already advanced), corrupting the replayed event stream; the loop then reads 0 bytes at EOF while sent < fd.len, and after 32 zero-send iterations the fatal 'sender stalled' exception kills the server. (2) The mmap error check tests for nullptr, but mmap returns MAP_FAILED ((void*)-1) on failure, so a failed mmap proceeds to a segfault. (3) The open() error message passes fd (-1) instead of errno to strerror, yielding a wrong reason text. (4) buf.reserve(BUF_SIZE) + writing through buf.data() writes into reserved-but-unsized vector storage (formally UB, works in practice).

**Proof.**

`src/test_server.cpp:349-355`
```cpp
if (use_mmap)
    return &data[after];
int nbytes = read(fd.fd, data, size);
if (nbytes < 0)
    throw Poco::SystemException("read failed", errno);
size = nbytes;
```
`src/test_server.cpp:425-429`
```cpp
const char* data = fd.read_bytes(send_size, sent);
int sz = con.sendBytes(data, send_size);
if (verbose)
    std::cout << "data sender: sent " << sz << " bytes\n";
sent += sz;
```
`src/test_server.cpp:318-323`
```cpp
if (! (data = (char *)mmap(nullptr, len, PROT_READ, MAP_PRIVATE
    #ifdef MAP_POPULATE
    | MAP_POPULATE
    #endif
    , fd.fd, 0)))
    throw Poco::RuntimeException(std::string("mmap failed: ") + std::strerror(errno));
```
`src/test_server.cpp:263-264`
```cpp
if (fd < 0)
    throw Poco::RuntimeException(std::string("unable to open file ") + name + ": " + std::strerror(fd));
```

**Impact.** Under the (rare) partial-send condition the replayed stream is silently corrupted — tpx3app would report bogus chunk sizes or wrong histograms, wasting debugging time on a phantom decoder bug — and the server then dies via the fatal-stall path. The mmap and strerror defects degrade diagnosability of -m mode failures.

**Fix.** Track a separate file offset in read_bytes (honor the 'after' parameter in non-mmap mode via pread(fd.fd, data, size, after)), which makes 'sent' the single source of truth in both modes; compare the mmap result against MAP_FAILED; use std::strerror(errno) in the open error; use buf.resize(BUF_SIZE) instead of reserve().

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: All four quotes verbatim. (1) read_bytes (349-355) advances the kernel file offset by the full nbytes read while the caller advances only by sz accepted by sendBytes (425-429); a short send therefore skips the unsent tail, and at EOF read returns 0 -> size=0 -> sendBytes returns 0 -> 32 iterations of the stall counter -> fatal 'sender stalled' throw. Correctly hedged as rare on a blocking socket. (2) Line 318-322: mmap result tested with '!' but failure is MAP_FAILED ((void*)-1), truthy — failed mmap proceeds to data=(char*)-1 and segfaults on &data[after]. (3) Line 264: std::strerror(fd) pass</sub>

### TPX-098 · [LOW] compile.sh DEBUG mode: missing separator corrupts user-supplied SPEED_FLAGS, and 'debug' builds still define NDEBUG

**Claim.** In the DEBUG (non-NOOPT) branch, SPEED_FLAGS+="-Og -ggdb -DNDEBUG" appends without a leading space, so an environment-provided SPEED_FLAGS (documented as a supported override in the help text) gets concatenated into a broken token (e.g. '-DX' becomes '-DX-Og'). The same branch also defines -DNDEBUG, so a DEBUG=1 build — the one used for debugging — has all assert() checks compiled out; asserts are only active with DEBUG=1 NOOPT=1. Similarly, line 23 assigns TEST_FLAGS with '=' semantics via a first '+=' on an empty var but line 26 appends, making env TEST_FLAGS honored in one branch and combined differently in another.

**Proof.**

`compile.sh:24-26`
```cpp
elif [ -z "${NOOPT}" ]; then
    SPEED_FLAGS+="-Og -ggdb -DNDEBUG"
    TEST_FLAGS+=" -Og -ggdb"
```
`compile.sh:142`
```cpp
echo "    SPEED_FLAGS  extra optimization flags"
```

**Impact.** A developer exporting SPEED_FLAGS while using DEBUG=1 gets a mangled compiler invocation (build failure or a silently dropped define); debug builds behave differently from expectation because asserts are disabled, hiding invariant violations exactly when someone is hunting a bug.

**Fix.** Change line 25 to SPEED_FLAGS+=" -Og -ggdb" (leading space) and drop -DNDEBUG from the DEBUG branch so asserts are active in debug builds; keep -DNDEBUG only in the release branch.

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Both primary defects confirmed verbatim: line 25 is SPEED_FLAGS+="-Og -ggdb -DNDEBUG" with no leading space (env SPEED_FLAGS='-DX' yields the broken token '-DX-Og'), SPEED_FLAGS is documented as a supported override (line 142), and the DEBUG-without-NOOPT branch defines -DNDEBUG so asserts are dead in the standard debug build — only DEBUG=1 NOOPT=1 (line 28) keeps them. Minor correction to the tertiary observation: line 23 is a plain assignment TEST_FLAGS=" -Og -ggdb" (not 'a first += on an empty var') — the mechanism description is wrong but the conclusion stands: env TEST_FLAGS is clobbered </sub>

### TPX-099 · [LOW] Production unit runs at --loglevel=debug; docker-run deployment has unbounded log growth

**Claim.** The production systemd unit permanently runs tpx3app with --loglevel=debug into the journal, and the documented container invocation also uses -ldebug. Under systemd, journald rotates but also rate-limits per service (default ~1000 messages/30s), so debug-level bursts during high-rate collection can cause journald to drop messages — including the interesting ones during an incident. Under the documented 'docker run' deployment there is no log-driver configuration at all, so the default json-file driver grows without rotation for long-running detached containers.

**Proof.**

`unit_files/tpx3app.service:11, 14-15`
```cpp
ExecStart=/home/asi/git/tpx3app --control=%H.psi.ch:8452 --initial-period=7633 --max-period-queues=6 --loglevel=debug -S
...
StandardOutput=journal
StandardError=journal
```
`container/README.md:46`
```cpp
docker run --rm --network=host --mount=type=bind,source=$PWD/output,destination=/output -d --name=tpx3-app tpx3-app -S -ldebug
```

**Impact.** Diagnostic messages can be silently dropped by journald rate limiting exactly during high-load incidents, and long-lived docker deployments can fill the host disk with container logs, eventually failing the DAQ host.

**Fix.** Default to --loglevel=info in the unit and document a drop-in override for debug sessions; add LogRateLimitIntervalSec=0 (or a raised burst) to the unit if debug must stay; in the docker instructions add --log-opt max-size=100m --log-opt max-file=5 (or use the journald log driver).

<sub>Verification: **adjusted** (severity/lines adjusted by verifier). Author confidence: high.</sub>

<sub>Verifier: Quotes verbatim (unit line 11 --loglevel=debug with StandardOutput/StandardError=journal at 14-15; README line 46 runs -d with -ldebug and no --log-opt). Core claims hold: journald per-service rate limiting can drop debug bursts, and the documented detached docker run uses the default json-file driver with no rotation options. Correction: the cited default of '~1000 messages/30s' is the pre-v240 systemd default; since systemd 240 LogRateLimitBurst defaults to 10000 per 30s (~333 msg/s) — still plausibly exceeded by debug logging during high-rate collection, but the figure as stated is wrong fo</sub>

### TPX-100 · [LOW] tpx3app.service lacks any hardening or resource-limit directives

**Claim.** Beyond User=asi/Group=asi, the unit sets no sandboxing or resource directives: no NoNewPrivileges, ProtectSystem, ProtectHome/ReadWritePaths, PrivateTmp, CapabilityBoundingSet, no LimitNOFILE/LimitCORE, no MemoryMax/TasksMax, and no OOMPolicy/watchdog — for a network-facing process linked with jemalloc that buffers high-rate detector data (queue tuning flags --initial-period/--max-period-queues imply significant memory use).

**Proof.**

`unit_files/tpx3app.service:5-16`
```cpp
[Service]
Type=simple
User=asi
Group=asi
StandardInput=null
WorkingDirectory=/home/asi/git
ExecStart=/home/asi/git/tpx3app --control=%H.psi.ch:8452 --initial-period=7633 --max-period-queues=6 --loglevel=debug -S
Restart=always
RestartSec=2
StandardOutput=journal
StandardError=journal
```

**Impact.** A malformed-input-driven memory blowup or leak in tpx3app can OOM the whole beamline host (taking serval and everything else down) instead of just the service; a compromise of the unauthenticated REST port gets an unconfined process; core dumps and fd limits are whatever the distro defaults happen to be.

**Fix.** Add to [Service]: NoNewPrivileges=yes, ProtectSystem=strict, ProtectHome=tmpfs with ReadWritePaths=/home/asi/git (or better, move the binary out of $HOME per the versioning finding), PrivateTmp=yes, MemoryMax= sized to the queue configuration, LimitNOFILE=..., and consider WatchdogSec= with sd_notify support so a hung collect loop is restarted automatically.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim; read the full 18-line unit file. Beyond User=asi/Group=asi there are no sandboxing directives (no NoNewPrivileges, ProtectSystem, ProtectHome, PrivateTmp, CapabilityBoundingSet), no resource limits (LimitNOFILE/LimitCORE/MemoryMax/TasksMax), and no OOMPolicy/WatchdogSec — for a jemalloc-linked, network-facing process whose --initial-period/--max-period-queues flags imply substantial buffering. No drop-in or override evidence elsewhere in the repo. Severity low (hardening-gap, not an active defect) is right.</sub>

### TPX-101 · [INFO] archive.sh: unquoted eval'd tar command; backup mechanism silently sensitive to word-splitting and glob misses

**Claim.** The archive command is built as a string and eval'd with $BACKUP_FILE unquoted, so an ARCHIVE_DEFAULT or -backup path containing spaces or shell metacharacters splits or is interpreted by the shell; the fixed glob list also means any future source subdirectory (e.g. a new dir under src/include/) is silently omitted from 'backups'. This script doubles as the de-facto release mechanism (see versioning finding), amplifying the consequence of a silent omission.

**Proof.**

`archive.sh:47-49`
```cpp
CMD="tar czf $BACKUP_FILE src/*.cpp src/include/*.h compile.sh archive.sh LICENSE README.md generate_data/*.jl test_data/*.jl container/*.docker doc unit_files"
echo "$CMD"
eval "$CMD"
```

**Impact.** A date-stamped 'code backup' can be incomplete or written to an unintended path without any error, discovered only when someone tries to restore from it.

**Fix.** Drop the eval; call tar directly with quoted arguments and use 'git archive HEAD' (the tree is a git repo) so the file list can never drift from the actual sources; fail loudly if tar reports missing files.

<sub>Verification: **confirmed**. Author confidence: high.</sub>

<sub>Verifier: Quote verbatim at archive.sh lines 47-49: the command string embeds $BACKUP_FILE unquoted and is eval'd, so ARCHIVE_DEFAULT/-backup paths with spaces or metacharacters split or get shell-interpreted; the hardcoded glob list (src/*.cpp, src/include/*.h, ...) silently omits any future subdirectory or file type not matching the patterns. Note src/include/*.h currently excludes the gitignored generated version.h harmlessly, but also would miss e.g. any new nested include dir. The upstream date-prefix handling (lines 41-45) also concatenates unquoted into BACKUP_FILE. Info severity appropriate.</sub>

