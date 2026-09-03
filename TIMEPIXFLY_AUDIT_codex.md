# TimePixFly Independent Production Audit — Codex

**Repository:** TimePixFly  
**Reviewed branch:** `dev`  
**Reviewed commit:** `ee7ee054b553bf2ea81feb1d108e356e54a03a57`  
**Review date:** 2026-09-02  
**Reviewer:** Codex  
**Review type:** Independent source, architecture, deployment, and production-readiness assessment

> This assessment was performed independently. The existing Claude/Fable audit report was not
> read or used as a source. The checked-out `audit` branch was verified to contain exactly the
> reviewed `dev` commit plus the two Claude report files; the analysis itself used an isolated
> `dev` snapshot in which those report files were absent.

## Executive summary

TimePixFly has a clear high-throughput architecture: one reader, per-chip analysis workers, and a
single aggregation/output writer. The code also makes good use of RAII, smart pointers, atomics for
several lifecycle flags, explicit state names, and validation for several detector-map invariants.

The current `dev` revision is nevertheless not ready to be treated as a hardened production
baseline without remediation. Four high-severity findings affect operational control, memory
safety/resource use, and cross-thread correctness:

1. The HTTP control plane has no authentication or authorization, yet the supplied production
   service binds it to the host's PSI network name and exposes stop, restart, kill, configuration,
   file-selection, and output-selection operations.
2. `TimeRoi::SetTimeROI` uses `&&` where `||` is required. It accepts a zero or negative step/count
   whenever the other field is positive, then converts signed inputs to unsigned values.
3. REST-supplied ROI and pixel-map values can drive effectively unbounded histogram allocations.
   A configuration typo is sufficient to exhaust memory; no hostile intent is required.
4. Program state, configuration transitions, the last-error value, and startup callback
   registration are not synchronized consistently. The code contains C++ data races and a window
   in which an HTTP request can observe or mutate partially initialized control state.

The most urgent action is to restrict the control interface at deployment level immediately, then
correct and bound all configuration validation before addressing the concurrency design. The
remaining findings concern WebSocket ownership, blocking output transports, Redis URI handling,
copy-mode error propagation, deployment artifacts, reproducible builds, and undefined behavior in
low-level helpers.

## Overall assessment

| Area | Assessment | Rationale |
|---|---|---|
| Control-plane exposure | **High risk** | Powerful HTTP operations have no caller verification; shipped service binds beyond loopback. |
| Configuration validation | **High risk** | Invalid ROI values are accepted and allocation-driving values have no upper bounds. |
| Concurrency | **High risk** | Shared non-atomic state is read and written from HTTP and main/worker threads without one locking protocol. |
| Data pipeline | **Moderate risk** | Architecture is understandable, but shutdown/error and release-only invariant handling need strengthening. |
| Output transports | **Moderate risk** | Synchronous TCP/Redis writes lack explicit time bounds and Redis authentication is not implemented. |
| Deployment | **Moderate risk** | The service unit contains unsupported flags; container builds are floating and run as root by default. |
| Verification | **Limited assurance** | A test program exists, but there is no repository CI/build manifest and the local dependency set was incomplete. |

## Finding summary

| ID | Severity | Confidence | Finding |
|---|---:|---:|---|
| TPF-CX-001 | High | High | Unauthenticated HTTP control plane is externally bindable in the supplied service |
| TPF-CX-002 | High | High | ROI validation accepts invalid values and feeds unsafe arithmetic/indexing |
| TPF-CX-003 | High | High | Configuration can trigger unbounded histogram and parser memory consumption |
| TPF-CX-004 | High | High | Global state and configuration transitions contain C++ data races |
| TPF-CX-005 | Medium | High | REST service starts before stop handlers are registered |
| TPF-CX-006 | Medium | High | WebSocket singleton has unsafe concurrent ownership and access |
| TPF-CX-007 | Medium | Medium | Synchronous TCP and Redis output can block acquisition and shutdown indefinitely |
| TPF-CX-008 | Medium | High | Redis credential syntax is documented but ignored; scan IDs bypass JSON encoding |
| TPF-CX-009 | Medium | High | Copy mode can ignore stop requests and reports I/O failures as successful runs |
| TPF-CX-010 | Medium | High | Supplied systemd unit invokes unsupported command-line options |
| TPF-CX-011 | Medium | High | Container builds are not reproducible and default to a root, host-network runtime |
| TPF-CX-012 | Medium | Medium | Low-level helpers rely on undefined behavior and release-disabled assertions |
| TPF-CX-013 | Low | High | Automated verification and dependency/build metadata are insufficient for a production branch |

## Detailed findings

### TPF-CX-001 — Unauthenticated HTTP control plane is externally bindable

**Severity:** High  
**Confidence:** High  
**Affected components:** REST service, systemd deployment, configuration callbacks

#### Evidence

- `src/include/rest_callbacks.h:65-109` dispatches requests solely by method, path, and query
  parameter. There is no authentication, authorization, client identity, origin restriction, or
  per-operation policy check.
- `src/include/rest_callbacks.h:273-275` creates a plain HTTP server directly on the configured
  `SocketAddress`.
- `src/include/rest_callbacks.h:320-373` exposes stop, restart, stop-collection, and immediate
  process-exit operations.
- `src/include/rest_callbacks.h:475-506` accepts pixel maps directly or reads a caller-selected file.
- `src/include/rest_callbacks.h:543-560` lets a caller select the output URI, save interval, and
  histogram ROI.
- `src/xes_data_writer.cpp:41-46` creates output files from the configured base path.
- `unit_files/tpx3app.service:11` binds the control endpoint to `%H.psi.ch:8452`, rather than the
  application's safer loopback default.

#### Impact

Any host that can reach the configured control port can stop or repeatedly restart the service,
force an immediate process exit, replace measurement configuration, select files for parsing, and
redirect output to a file, TCP endpoint, or Redis endpoint accessible to the `asi` account. This can
interrupt experiments, corrupt measurement provenance, exhaust resources, or create/overwrite
period-suffixed files within the service account's filesystem permissions.

The default address in `src/include/global.h:75` is loopback, which is a useful safe default, but the
supplied service overrides it with a network-visible hostname. Network segmentation may reduce
reachability, but no such boundary is defined or enforced by the repository.

#### Recommendation

1. Immediately bind production deployments to loopback or a dedicated management interface with a
   host firewall allowlist.
2. Put the service behind an authenticated, encrypted reverse proxy or implement authenticated
   requests with distinct read/control/configure privileges.
3. Remove `kill` from the remotely reachable API. Make destructive operations non-GET and require
   explicit authorization plus request auditing.
4. Restrict file inputs and file outputs to configured base directories using canonicalized paths;
   do not accept arbitrary filesystem paths.
5. Add deployment tests that fail when a control endpoint is externally bound without the required
   protection.

---

### TPF-CX-002 — ROI validation accepts invalid values and feeds unsafe arithmetic/indexing

**Severity:** High  
**Confidence:** High  
**Affected components:** `TimeRoi`, analysis indexing, histogram allocation, REST configuration

#### Evidence

- `src/include/time_roi.h:37-38` rejects input only when **both** `tRoiStep <= 0` and `tRoiN <= 0`:

  ```cpp
  if ((tRoiStep <= 0) && (tRoiN <= 0))
      throw std::invalid_argument("TRoiStep and TRoiN must be positive");
  ```

  The condition must use `||` to enforce that each value is positive.
- `src/include/time_roi.h:40-44` assigns signed `int` inputs to unsigned 64-bit fields and computes
  `TRoiStart + TRoiStep * TRoiN` without overflow checks.
- `src/include/analysis.h:72` and `src/include/analysis.h:126` compute `1.f / TRoiStep`.
- `src/include/analysis.h:59-60` converts the resulting floating-point expression to an `int` and
  uses it as part of the histogram index at `src/include/analysis.h:40`.
- `src/include/xes_data.h:46-48` uses `TRoiN` in the histogram allocation size.
- `src/include/rest_callbacks.h:556-560` passes caller-supplied JSON values directly to this method.

#### Impact

Examples accepted by the current condition include `(TRoiStep=0, TRoiN=5000)`,
`(TRoiStep=1, TRoiN=0)`, and negative values when the other field is positive. Consequences include:

- division by zero and non-finite `TRoiStep_inv`;
- signed-to-unsigned conversion to very large values;
- overflow in `TRoiEnd` and histogram-size calculations;
- zero-length histograms followed by `TDSpectra[0]` in TCP/Redis writers;
- out-of-range writes in `Analysis::Register`.

This is reachable through ordinary configuration and therefore can occur through a typo as well as
through an untrusted caller.

#### Recommendation

- Change the predicate to reject each non-positive field independently.
- Define valid minimum and maximum values for start, step, count, and the computed end.
- Perform checked unsigned conversion and checked multiplication/addition before committing state.
- Compute and validate the complete proposed configuration in local temporaries, then assign it
  atomically only after every check succeeds.
- Add boundary tests for zero, negative, maximum, overflow, and one-invalid-field cases.

---

### TPF-CX-003 — Configuration can trigger unbounded memory consumption

**Severity:** High  
**Confidence:** High  
**Affected components:** REST request parsing, pixel-map parsing, histogram pool allocation

#### Evidence

- `src/include/rest_callbacks.h:78-81` parses an HTTP request stream into a full JSON object without
  an application-level request-size limit.
- `src/energy_points.cpp:87-96` accepts arbitrary unsigned energy-point identifiers and derives
  `npoints` from the largest value plus one; no maximum is enforced.
- `src/include/xes_data.h:46-48` allocates `TRoiN * npoints` floating-point elements per histogram.
- `src/include/xes_data_manager.h:94-107` maintains at least five histograms per chip and may create
  additional histograms when the writer falls behind.
- `src/include/rest_callbacks.h:475-505` accepts a full pixel map or reads one from an arbitrary
  selected file; `src/include/rest_callbacks.h:543-560` accepts the ROI and save interval.

#### Impact

A single excessive `TRoiN` or energy-point index can request gigabytes or more per histogram, then
multiply that cost across chips and pool entries. Even valid-but-large requests can consume CPU
during JSON parsing before allocation. A writer slowdown can grow the pool further. The likely
result is process termination, host memory pressure, or disruption of other beamline services.

#### Recommendation

- Establish a documented memory budget and derive hard maxima for chips, mapped pixels, parts per
  pixel, energy points, ROI bins, JSON body size, and total histogram bytes.
- Reject a proposed configuration if checked multiplication exceeds the per-run memory budget.
- Limit HTTP content length before parsing and cap nesting/collection sizes.
- Validate weights as finite and within the domain expected by the analysis.
- Consider preallocating a bounded pool and applying explicit backpressure rather than expanding it
  without a cap.

---

### TPF-CX-004 — Global state and configuration transitions contain C++ data races

**Severity:** High  
**Confidence:** High  
**Affected components:** global error state, program state, REST configuration, measurement startup

#### Evidence

- `src/global.cpp:18-34` locks `error_lock` in `set_error` and `get_error`, but
  `src/global.cpp:37-40` reads the same `std::string last_error` without that lock.
- `src/include/global.h:114` declares `state` as a non-atomic `std::string_view` and explicitly notes
  that protection may be required.
- `src/include/rest_callbacks.h:209-221` writes `state` while holding `ws_mutex` only in server mode.
  Other readers, including `src/include/rest_callbacks.h:401-404` and
  `src/include/rest_callbacks.h:426-432`, do not take that mutex.
- Configuration PUT handlers take `global::configLock()` and check `state`, for example
  `src/include/rest_callbacks.h:543-560`.
- The main thread changes state and reads/copies configuration without `configLock()`, for example
  `src/main.cpp:698-705` and the calls through `processing::init()`/`Analysis::Reset()`.

#### Impact

Concurrent access to `last_error` and `state` is undefined behavior under the C++ memory model. More
importantly, the configuration lock does not make the `config -> setup` transition atomic: a PUT can
observe `config`, pause, and then modify ROI/output/map state after the main thread has begun setup.
This can leave the writer, `Analysis`, and global configuration using different dimensions or
pointers, resulting in wrong data, invalid JSON/file output, out-of-range access, or a crash.

#### Recommendation

- Define one ownership and synchronization model for global lifecycle and configuration state.
- Protect `last_error` in every accessor, including `error_empty`.
- Do not use a WebSocket mutex as the state mutex. Introduce a dedicated state/configuration mutex
  or a small state-machine object.
- Under the same lock, transition `config -> setup`, snapshot an immutable validated configuration,
  and make subsequent processing depend only on that snapshot.
- Have REST PUT operations construct and validate a new configuration before replacing the active
  one under the lock.
- Add ThreadSanitizer integration tests covering concurrent `/state`, `/other-config`, start, stop,
  and error reporting.

---

### TPF-CX-005 — REST service starts before stop handlers are registered

**Severity:** Medium  
**Confidence:** High  
**Affected components:** startup ordering, stop/restart callbacks

#### Evidence

- `src/main.cpp:651-652` initializes callbacks and starts the HTTP service.
- Only afterward, `src/main.cpp:654-671` creates `CopyHandler` or `DataHandler` and appends a lambda to
  `global::stop_handlers`.
- HTTP callbacks iterate that vector at `src/include/rest_callbacks.h:324-325`,
  `src/include/rest_callbacks.h:342-343`, and `src/include/rest_callbacks.h:359-360`.

#### Impact

An early stop/restart request can race with `std::vector::emplace_back`. Concurrent vector mutation
and iteration is undefined behavior. A request arriving slightly earlier simply sees an empty list
and fails to stop the data pipeline cleanly.

#### Recommendation

Construct the selected data/copy handler and register immutable callbacks before calling
`start_service`. Prefer a single lifecycle coordinator over a mutable public vector. If runtime
registration remains necessary, protect the collection and invoke a stable copied callback list
outside the lock.

---

### TPF-CX-006 — WebSocket singleton has unsafe concurrent ownership and access

**Severity:** Medium  
**Confidence:** High  
**Affected components:** `/ws` state notifications

#### Evidence

- `src/include/rest_callbacks.h:134-136` stores one static `std::unique_ptr<WebSocket>` shared by all
  request handlers.
- `src/include/rest_callbacks.h:156-162` replaces it when a new `/ws` request arrives.
- `src/include/rest_callbacks.h:168-186` repeatedly reads and dereferences the shared pointer without
  holding `ws_mutex`.
- `src/include/rest_callbacks.h:194-199` can shut down and reset the same pointer from another
  handler, while `set_state` also sends through it under the mutex at lines 209-218.

#### Impact

Two concurrent WebSocket clients can cause one handler to operate on another client's socket or to
race with pointer replacement/reset. Outcomes include incorrect delivery, connection loss, use
after free, or process instability.

#### Recommendation

Give each handler exclusive ownership of its own socket. Maintain a mutex-protected subscriber
collection containing stable shared/weak handles, copy the active handles before broadcasting, and
remove a client on disconnect. Add a two-client concurrency test and repeated connect/disconnect
test under ThreadSanitizer.

---

### TPF-CX-007 — Synchronous TCP and Redis output can block acquisition and shutdown

**Severity:** Medium  
**Confidence:** Medium  
**Affected components:** `TcpWriter`, `RedisWriter`, aggregation writer thread

#### Evidence

- `src/xes_data_writer.cpp:86-97` performs a blocking TCP connect with no explicit connect timeout.
- `src/xes_data_writer.cpp:125-138`, `160-175`, and `191-202` write and flush through
  `SocketStream` without an explicit send timeout.
- `src/xes_data_writer.cpp:234-239`, `349-364`, and `400-418` issue synchronous Redis commands with
  no application deadline visible in the implementation.
- `src/include/xes_data_manager.h:224-226` performs output on the only aggregation/writer thread;
  histograms are returned to the pool only after the write.

#### Impact

A connected but non-reading TCP peer, a stalled Redis server, or a network failure can pin the sole
writer. Analysis workers then retain or allocate additional histograms, stop/shutdown may wait for
the writer, and memory use can grow. Actual OS/library defaults require runtime confirmation, hence
the medium confidence rating for indefinite duration; the code itself establishes no deadline.

#### Recommendation

Set and document finite connect, send, receive, and command timeouts. Make cancellation/shutdown
close the transport so blocking operations are interrupted. Define a bounded output queue and a
policy for backpressure, dropped output, retry, and measurement failure. Test a peer that connects
but never reads and a Redis endpoint that stalls mid-run.

---

### TPF-CX-008 — Redis credential syntax is ignored; scan IDs bypass JSON encoding

**Severity:** Medium  
**Confidence:** High  
**Affected components:** Redis output writer and protocol documentation

#### Evidence

- `src/xes_data_writer.cpp:429` documents `redis://user:pwd@host:port/key?...`.
- `src/xes_data_writer.cpp:433` leaves `getUserInfo()` commented out, and lines 449-450 contain only
  comments where authentication should occur.
- `src/xes_data_writer.cpp:438-439` derives the channel and scan ID with raw substrings and does not
  validate the query key or decode policy.
- `src/xes_data_writer.cpp:284-292` inserts `scan` into JSON by string concatenation, unlike the
  `Poco::JSON::PrintHandler` used for start/end frames.

#### Impact

Deployments may believe credentials in the documented URI are used when the client actually makes
an unauthenticated connection. A scan ID containing quotes, backslashes, or control characters can
produce invalid or structurally altered JSON data messages, breaking downstream consumers and
measurement association.

#### Recommendation

Either implement Redis authentication securely or reject URIs with user information and correct the
documentation. Parse query parameters by name, require exactly the supported keys, validate channel
and scan identifiers, and construct every frame through the JSON encoder. Avoid logging or returning
credentials in destination strings.

---

### TPF-CX-009 — Copy mode can ignore stop requests and report I/O failures as success

**Severity:** Medium  
**Confidence:** High  
**Affected components:** `CopyHandler`, read-only stream mode, application exit status

#### Evidence

- `src/include/copy_handler.h:182-185` treats path `none` as read-only copy mode.
- `src/include/copy_handler.h:97-100` checks the queue stop flag indirectly only when **not** in
  read-only mode; read-only mode reuses the same reservation and continues at lines 83-103.
- `src/include/copy_handler.h:201-204` implements `stopNow` solely by setting the queue stop flag.
  With a continuously producing source, read-only mode does not consult it.
- `src/include/copy_handler.h:105-110` and `161-166` catch read/write failures and log them but do not
  call `global::set_error` or rethrow.
- `src/include/copy_handler.h:178-184` does not validate that the output file opened successfully at
  construction time.
- `src/main.cpp:813-815` returns success when the global error remains empty.

#### Impact

`stop_collect` can fail to stop a continuous read-only stream. File-open, read, and write failures can
end the copy threads while the application reports a successful run. For raw detector capture this
creates a material risk of incomplete data being mistaken for a valid acquisition.

#### Recommendation

Use one explicit cancellation token checked in every read loop, and close/shutdown the socket on
cancellation. Validate the output stream immediately. Propagate every copy-thread failure to the
global run result and include bytes expected/written plus a clean end-of-stream marker in success
criteria. Add tests for disk-full, permission denied, peer timeout, and continuous-stream stop.

---

### TPF-CX-010 — Supplied systemd unit invokes unsupported options

**Severity:** Medium  
**Confidence:** High  
**Affected components:** `unit_files/tpx3app.service`

#### Evidence

- `unit_files/tpx3app.service:11` passes `--initial-period=7633` and
  `--max-period-queues=6`.
- `src/main.cpp:171-298` defines the current option set and contains neither option.
- A repository-wide search found those names only in the unit file.

#### Impact

The shipped service definition does not match the current executable interface and should fail with
unknown-option handling. With `Restart=always`, this can create a restart loop. If production works,
it implies that the deployed unit differs from the repository, which is itself a configuration and
traceability gap.

#### Recommendation

Update or remove obsolete options, add `After=`/dependency ordering appropriate to the actual
services, and validate the exact unit in CI using the built executable's option parser. Store the
production unit and configuration in version control and record the deployed commit.

---

### TPF-CX-011 — Container builds are not reproducible and default to root/host networking

**Severity:** Medium  
**Confidence:** High  
**Affected components:** both container Dockerfiles and container runbook

#### Evidence

- `container/tpx3app-container.docker:8-19` and `container/server-container.docker:8-19` clone a
  mutable branch during the build rather than building the checked-out source or a required commit.
- Base images and apt packages are not digest/version pinned.
- Both runtime stages install development packages at lines 27-30.
- Neither runtime stage declares a non-root `USER`.
- `container/README.md:15-21` and `44-47` recommend host networking.

#### Impact

The same build command can produce different binaries over time and cannot be tied reliably to the
reviewed commit. Network access during build expands the supply-chain boundary. Running as root with
host networking increases the consequence of a process-level defect and makes the control services
directly share the host network namespace.

#### Recommendation

Build from the supplied context at a required commit, pin base-image digests and dependency versions,
install only runtime libraries in the final stage, run as an unprivileged dedicated user, and use a
dedicated container network with explicit published ports. Generate an SBOM and record image digest,
source commit, compiler, and dependency versions with each deployment.

---

### TPF-CX-012 — Low-level helpers rely on undefined behavior and release-disabled assertions

**Severity:** Medium  
**Confidence:** Medium  
**Affected components:** event representation, pixel-map ranges, queue/parser invariants, build flags

#### Evidence

- `src/include/event_type.h:30` and `src/include/event_type.h:58` inspect `event_t` by casting its
  address to `u64*`, which violates strict-aliasing rules.
- `src/include/pixel_map.h:71-82` forms `begin()` with `&mapping[start]`. For an empty range at
  `start == mapping.size()`, this evaluates `operator[]` one past the vector before taking the
  address; `data() + start` is the valid representation.
- Critical invariants throughout `io_buf.h`, `subreservation.h`, `data_handler.h`, and
  `xes_data_manager.h` are enforced only with `assert`.
- `src/include/xes_data_manager.h:356-360` asserts that returned periods are nonzero, but
  `src/include/data_handler.h:434` can perform a final purge with the initial period value when no
  valid TDC establishes a period.
- `compile.sh:20-22` builds production with `-O3`, `-ffast-math`, and `-DNDEBUG`, removing those
  assertions and increasing sensitivity to undefined behavior.

#### Impact

The current compiler and architecture may produce the intended result, but the C++ standard does not
guarantee it. Release builds silently discard queue and parser checks that debug/tests rely on. A
malformed or incomplete stream can therefore generate period-zero output or continue through an
invalid state instead of producing a controlled measurement failure.

#### Recommendation

Replace type punning with `std::memcpy` in C++17 (or `std::bit_cast` after moving to C++20), return
`mapping.data() + start`, and convert externally influenced/integrity-critical assertions into
runtime checks with explicit error propagation. Keep assertions for internal diagnostics in
addition to—not instead of—production validation. Evaluate `-ffast-math` against scientific
accuracy requirements and test both optimized generic and AVX builds under ASan/UBSan.

---

### TPF-CX-013 — Automated verification and build metadata are insufficient

**Severity:** Low  
**Confidence:** High  
**Affected components:** repository-level engineering controls

#### Evidence

- No CI workflow, CMake/Meson project, dependency lock, or package manifest is present in the
  reviewed revision.
- `compile.sh` discovers dependencies from the host and records only branch, short commit, and date
  in the generated version string.
- `src/test.cpp` provides a useful custom test executable, but the repository does not define an
  automated matrix for release/generic/AVX/sanitizer configurations.
- `README.md:5-8` explicitly records missing logging and exception-handling requirements.

#### Impact

Regressions in configuration boundaries, concurrency, optimized builds, and deployment artifacts
can reach `dev` without a repeatable gate. Reviewers cannot reproduce a production binary from the
repository alone.

#### Recommendation

Introduce a repeatable build definition and CI that covers compilation with warnings-as-errors,
unit tests, REST integration tests, representative raw-stream tests, ASan/UBSan, ThreadSanitizer,
generic and AVX builds, container builds, and service-file smoke tests. Pin or record dependency
versions and publish test evidence for every production commit.

## Cross-cutting remediation roadmap

### Immediate containment — before further production exposure

1. Bind the control API to loopback or a management-only interface and firewall it to named clients.
2. Remove or block the remote `kill` endpoint.
3. Fix `TimeRoi` validation and apply conservative hard limits to ROI and map dimensions.
4. Correct the systemd unit and confirm exactly which unit/configuration is deployed.

### First corrective release

1. Implement an immutable validated measurement-configuration snapshot.
2. Serialize the `config -> setup` transition with REST configuration updates.
3. Lock all last-error access and replace the current state synchronization with one dedicated
   state-machine lock/ownership model.
4. Start the HTTP service only after all handlers and stop callbacks are fully constructed.
5. Propagate copy-mode failures and make cancellation close active sockets.
6. Add targeted regression tests for these defects.

### Hardening release

1. Add authenticated, authorized, audited control operations or place the API behind an equivalent
   managed control gateway.
2. Bound transport operations and queues; define retry/backpressure/failure behavior.
3. Redesign WebSocket subscriber ownership.
4. Correct Redis authentication/URI/query/JSON handling.
5. Replace release-critical assertions and remove undefined behavior.

### Engineering assurance

1. Add reproducible builds, CI, sanitizers, dependency recording, and deployment smoke tests.
2. Build containers from pinned source and run them unprivileged on a dedicated network.
3. Define logging, exception, measurement-integrity, and recovery requirements and test them.

## Positive observations

- Control and raw-data endpoints default to loopback in `src/include/global.h:73-75`.
- The main processing architecture has identifiable reader, per-chip analyzer, and writer ownership.
- Several lifecycle signals are atomic, and the error value already has a mutex in two of its three
  accessors.
- Pixel-map parsing verifies detector chip count, pixel indices, and point/fraction list lengths.
- Output writer selection uses an explicit scheme allowlist.
- The application generally uses RAII, `std::unique_ptr`, scoped locks, and exception boundaries.
- The supplied systemd unit runs the native service as the unprivileged `asi` user.
- The test and analysis scripts show attention to event order, histograms, TDC clocks, and readout
  behavior; these are good foundations for automated regression tests.

## Methodology

The review used the following independent steps:

1. Verified branch ancestry and file differences. `audit` is `dev` commit
   `ee7ee054b553bf2ea81feb1d108e356e54a03a57` plus the Claude HTML/Markdown files only.
2. Exported `dev` into an isolated temporary snapshot containing no prior audit report.
3. Built a structural/semantic repository graph: 56 supported files, 863 final nodes, 1,335 final
   undirected edges, and 42 functional communities.
4. Traced control-plane, configuration/allocation, data pipeline, output, shutdown, build, service,
   and container paths manually against source line numbers.
5. Searched for synchronization primitives, exception boundaries, assertions, raw memory access,
   network operations, process exits, and configuration/file handling.
6. Inspected recent `dev` history and deployment artifacts.
7. Attempted a local test build using the source-defined libraries.

Every reported finding was manually verified in the source. Graph extraction reported 359 dangling
endpoint edges and edge collapse caused by unresolved external/library symbols and the undirected
projection; therefore the graph was used only as a navigation aid, not as evidence by itself.

## Validation performed and limitations

### Completed

- Confirmed reviewed commit and exact relationship between `dev` and `audit`.
- Confirmed that prior audit content was absent from the review snapshot.
- Verified every cited path and line against commit `ee7ee05`.
- Performed repository-wide structural searches and architecture tracing.
- Checked current option definitions against the supplied systemd unit.
- Checked deployment/container configuration against runtime defaults.

### Not completed

- The test binary could not be compiled locally because Poco headers/libraries were not installed in
  the available environment (`Poco/Exception.h` and related headers were missing).
- No detector/Serval service or Redis instance was started.
- No dynamic ASan, UBSan, ThreadSanitizer, fuzzing, load, or network-behavior test was run.
- Dependency CVE/version analysis was not possible because the build does not pin the effective Poco,
  jemalloc, compiler, base-image, or OS-package versions.
- The actual production firewall, reverse proxy, service override, and deployed binary/configuration
  were not available; exposure findings are based on repository artifacts.

## Independent-comparison note

Finding IDs in this document are Codex-specific. Because prior audit content was deliberately not
read, this report does not label findings as fixed, regressed, or previously known. Compare by the
affected code and recommended action rather than by finding number or wording.

## Recommended acceptance criteria

The next production candidate should not be accepted until:

- the control endpoint is demonstrably restricted and caller-authorized;
- invalid and excessive ROI/map configurations are rejected before any global state changes;
- concurrency tests show no races for state, error, configuration, startup, WebSocket, or shutdown;
- copy/output failures produce a failed measurement and bounded shutdown;
- the checked-in systemd/container artifacts run the exact reviewed commit;
- the unit/integration suite passes in optimized generic and AVX builds with sanitizer evidence.

