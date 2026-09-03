# Private presentation notes — TimePixFly Codex audit

**Private working document. Do not include this file in the author handover unless you intentionally
choose to share it.**

## The core message

The report is not an argument that the author built a poor system. The architecture is recognizable
and performance-oriented, and several good safeguards are already present. The message is that the
software has moved from specialist experimental code toward production infrastructure, so its
control, validation, concurrency, and deployment contracts now need to become explicit and tested.

The most productive framing is:

> “I reviewed the current `dev` commit independently as production software. The main data-flow
> design is clear, but I found four boundary issues where normal configuration, HTTP concurrency, or
> deployment behavior can escape the assumptions made by the fast path. I would like us to agree on
> the production boundaries first, then fix the smallest high-impact items together.”

## Suggested 45-minute agenda

1. **5 minutes — establish shared context**
   - Confirm the deployed commit, systemd unit/override, configuration file, network binding, and
     whether a proxy/firewall protects port 8452.
   - State explicitly that the review used commit `ee7ee05` and did not use the prior audit findings.

2. **10 minutes — acknowledge the design**
   - Reader → per-chip analyzers → aggregation writer is understandable.
   - Loopback defaults, RAII, smart pointers, atomics, and map validation are positive foundations.
   - Ask which throughput and latency requirements are non-negotiable.

3. **15 minutes — discuss the four high-priority findings**
   - Control-plane reachability and intended trust boundary.
   - `TimeRoi` Boolean error and checked arithmetic.
   - Memory budgets for histogram/map configuration.
   - Atomic configuration/state transition and error locking.

4. **10 minutes — agree on remediation sequence**
   - Immediate deployment containment.
   - Small correctness patch with tests.
   - Concurrency/lifecycle refactor.
   - Transport and build hardening.

5. **5 minutes — owners and evidence**
   - Assign owners and target dates.
   - Agree on the tests/evidence required to close each item.

## How to present the severities

- Explain that **High** combines consequence and reachability in the repository's supplied deployment,
  not a judgment of coding ability.
- The unauthenticated control finding depends strongly on the actual network boundary. If production
  has a firewall or authenticated proxy, record that as a compensating control and adjust severity
  only after verifying it.
- The ROI and allocation findings do not require an attacker. Emphasize configuration mistakes and
  production recovery rather than hostile scenarios.
- The concurrency finding is a language-level correctness issue: an unsynchronized `std::string` or
  `std::string_view` access is undefined behavior even if it “usually works.”

## Recommended wording for sensitive findings

### Control API

Use:

> “The code assumes the control network is trusted, but the repository does not define or enforce
> that boundary. The supplied service expands the bind address beyond the safe loopback default. Can
> we document the actual boundary and add a technical enforcement point?”

Evidence to show:

- `src/include/global.h:73-75` — the application defaults the control interface to loopback.
- `unit_files/tpx3app.service:11` — the supplied service changes the control bind address to
  `%H.psi.ch:8452`.
- `src/include/rest_callbacks.h:65-109` — request dispatch selects callbacks by HTTP method and path,
  without an identity or authorization check.
- `src/include/rest_callbacks.h:320-373` — reachable callbacks stop, restart, stop collection, or
  terminate the process.
- `src/include/rest_callbacks.h:475-506` and `src/include/rest_callbacks.h:543-560` — other callbacks
  replace the pixel map and measurement/output configuration.

Avoid opening with “anyone can hack the detector.” That is broader than the evidence and likely to
make the discussion defensive.

### ROI validation

Use:

> “This is a small predicate defect with a large downstream effect. Changing `&&` to `||` is the
> immediate correction, but the durable fix also needs checked conversion, overflow checks, and
> upper bounds.”

Evidence to show:

- `src/include/time_roi.h:32-44` — the validation rejects values only when both `TRoiStep` and
  `TRoiN` are non-positive, then calculates the endpoint with unchecked signed arithmetic.
- `src/include/rest_callbacks.h:543-560` — the control API supplies all three ROI values to that
  setter.
- `src/include/analysis.h:68-72` and `src/include/analysis.h:120-127` — analysis computes the
  reciprocal of `TRoiStep` during construction and reset.
- `src/include/xes_data.h:45-48` — the histogram allocation multiplies `TRoiN` by the number of
  energy points without a checked size calculation.
- `src/include/analysis.h:35-41` — the resulting time point is used in the histogram index.

This is the easiest finding to reproduce and should be the first code fix.

### Concurrency

Use:

> “There are several mutexes and atomics already, but they do not form one ownership protocol. The
> issue is less ‘missing locks’ than the lack of an atomic config-to-setup transition.”

Evidence to show:

- `src/include/global.h:105-114` — the lifecycle state is a plain `std::string_view`, with a source
  comment acknowledging that locking may be needed.
- `src/include/rest_callbacks.h:209-221` — state writes are protected by the WebSocket mutex only in
  server mode.
- `src/include/rest_callbacks.h:401-404` and `src/include/rest_callbacks.h:426-432` — REST callbacks
  read the same state without that mutex.
- `src/include/rest_callbacks.h:475-506` and `src/include/rest_callbacks.h:543-560` — configuration
  mutations use `configLock()` and separately test the lifecycle state.
- `src/main.cpp:698-705` — the main thread changes the state to `setup` and consumes configuration
  without holding `configLock()`, so the state transition and configuration snapshot are not one
  atomic operation.
- `src/global.cpp:18-40` — `set_error()` and `get_error()` lock `error_lock`, while `error_empty()`
  reads the same string without the lock; use this as a smaller, concrete example of inconsistent
  ownership.

This keeps the proposed solution focused on lifecycle design instead of adding scattered locks.

### Systemd mismatch

Ask whether `unit_files/tpx3app.service` is historical. If it is not deployed, downgrade its urgency
but request removal or an explicit archive label. If it is deployed, ask to see the effective unit
from `systemctl cat tpx3app` and the journal from startup.

Recommended wording:

> “The supplied unit starts the application with two options that are not registered by this version
> of the program. Before treating this as a production outage finding, can we confirm whether this is
> the effective deployed unit or a historical artifact?”

Evidence to show:

- `unit_files/tpx3app.service:11` — `ExecStart` passes `--initial-period=7633` and
  `--max-period-queues=6`.
- `src/main.cpp:171-298` — the complete application option-registration block does not define either
  option.
- `src/main.cpp:204-209` — `--control`, which appears on the same service command line, is a defined
  option; this helps distinguish the unsupported options from valid ones.

## Questions likely to come from the author

### “The control port is only on an internal network. Why is this High?”

Response: internal segmentation is valuable, but the API changes measurement configuration and
process lifecycle without knowing who called it. Internal networks still contain multiple hosts and
failure sources. Verify the firewall/proxy and treat it as a compensating control, not an implicit
assumption.

### “Why treat configuration limits as a safety issue? Operators know valid values.”

Response: the current failure mode is host-wide memory pressure rather than a rejected request. A
production service should turn operator mistakes into bounded, descriptive errors. Limits also make
capacity and performance predictable.

### “Have these races actually crashed production?”

Response: the review does not claim an observed crash. It identifies undefined behavior and a
specific interleaving that can mix configuration snapshots. ThreadSanitizer and transition tests are
the right way to determine observed frequency while the locking model is corrected.

### “Why mention containers if native systemd is production?”

Response: keep it as a separate deployment-artifact finding. If containers are only development
tools, label them accordingly; if they are a supported deployment path, reproducibility and
least-privilege expectations apply.

### “Did Codex copy the earlier report?”

Response: no. The `dev` commit was exported to an isolated snapshot that did not contain the Claude
files. Only filenames and Git ancestry were inspected in the working checkout.

## Recommended remediation order

### Same day

1. Verify effective production bind address and firewall.
2. Restrict port 8452 to the minimum required clients.
3. Disable remote `kill` access.
4. Fix the systemd command line or confirm the repository unit is not deployed.

### First patch

1. Correct ROI validation.
2. Add checked arithmetic and explicit maxima.
3. Add focused tests for invalid ROI and excessive pixel-map dimensions.
4. Lock `error_empty`.
5. Move REST startup after handler/callback construction.

### Next production iteration

1. Introduce an immutable measurement configuration snapshot.
2. Make `config -> setup` atomic with respect to HTTP PUT operations.
3. Repair copy-mode cancellation/error propagation.
4. Add transport deadlines and bounded output queues.
5. Redesign WebSocket subscriber ownership.

### Engineering follow-up

1. CI with optimized generic/AVX builds and sanitizers.
2. Reproducible native/container builds.
3. Versioned deployment configuration and operational requirements.

## Decisions that require the author

- Which clients legitimately need control-plane access?
- Is port 8452 protected by a verified external control today?
- What are the accepted maxima for chips, energy points, ROI bins, mapping entries, and memory?
- Should a slow output block acquisition, fail the measurement, or drop output?
- Is multi-client WebSocket support required, or should extra clients be rejected explicitly?
- Are Redis credentials required in production?
- Which deployment path—systemd, container, or both—is supported?
- What constitutes a successful raw copy and how should partial output be marked?

## Evidence to bring to the meeting

- The report open at TPF-CX-001 through TPF-CX-004.
- The effective production service unit and bind/listen output.
- The deployed binary version/commit.
- One proposed table of configuration maxima and memory calculation.
- A minimal patch/test for the `TimeRoi` predicate.
- A sketch of an immutable `MeasurementConfig` and atomic transition, if implementation discussion
  is expected.

## Closing language

> “The immediate fixes are bounded and should reduce operational uncertainty quickly. The larger
> concurrency change can be planned around the existing high-throughput design rather than replacing
> it. If we agree on the trust boundary, configuration limits, and failure semantics, the remaining
> implementation choices become much simpler.”
