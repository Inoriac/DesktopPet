# HNSW / Outbox Implementation Status

Related design: design.md sections 4, 5, 14, 15, 16.

## Checkpoints

- `10178e2`, `d65aa65`: initial HNSW wrapper, label table and outbox worker scaffold/tests.
- `142e12e`: SQLite vectors and HNSW files persist before a job completes.
  Binary and metadata use QSaveFile individually; SHA-256 detects a torn pair.
- `1334859`: repository savepoints fix nested Daydream/outbox transactions.
  The old transaction failures were not a macOS-specific SQLite limitation.
- `5430715`: rebuild reads memory_embeddings without model inference;
  content changes allocate a new label and capacity grows when full.
- `efc34dc`: major MemoryStore update/status/reinforcement/delete paths enqueue jobs.

## Automatic Recovery

- Worker loads an unavailable index, then rebuilds from SQLite on load failure.
  processPending also attempts recovery with an empty queue; limit<=0 does nothing.
- Rebuild reactivates only eligible labels in a SQLite savepoint, retaining
  numeric assignments for other rows. SQL failures abort instead of being
  silently skipped. File save and label synchronization run under one write lock.
- Failed rebuilds roll back label changes and clear the partial in-memory index.
  A pending job remains retryable and no later jobs in that batch are consumed.
- Rebuild filters Sensitive/non-active/Hippocampus/Working/ShortTerm/TaskShadow,
  expired items, wrong-model or wrong-dimension rows, malformed blobs and
  zero/non-finite vectors. Direct Worker consumption applies the same memory
  eligibility checks. Invalid vector rows remain in SQLite for later repair.
- Vector-level APIs reject invalid dimensions and zero/non-finite values.
  Reinsertions of a deleted label use hnswlib's actual tombstone count.

## Verification

- 2026-09-14 Windows / Qt 6.5.3 / MinGW / native ORT 1.28.0: Desktop_Pet
  and all nine related test targets build. Final CTest run passes 9/9 suites
  (18.50 s): MemoryStrategyTests, ChatPreparationExecutorTests, MemoryRecallTests,
  MemoryRecallPhase2Tests, MemoryRecallPhase3Tests, HybridGraphBuilderTests,
  SleepCycleTests, IdentityStateTests and StreamingDialogueTests. This includes
  the real ONNX chat path, clear/reimport activation regression and multi-channel
  seed-budget priority; it is a scoped regression run, not full-project CTest.
- Reproduce: `ctest --test-dir cmake-build-release-mingw_qt -R "^(MemoryStrategyTests|ChatPreparationExecutorTests|MemoryRecallTests|MemoryRecallPhase2Tests|MemoryRecallPhase3Tests|HybridGraphBuilderTests|SleepCycleTests|IdentityStateTests|StreamingDialogueTests)$" --output-on-failure`.
- Desktop_Pet and memory_strategy_tests build successfully on macOS.
- MemoryStrategyTests reports 100 passes (QtTest totals include init/cleanup).
- New data-driven tests cover binary corruption, missing metadata and missing
  labels, each with and without pending work; SQL read/write failures; metadata
  save failure; retry after failed recovery; invalid/filtered vectors; empty
  rebuilt indexes; deleted-label reinsertion. Ten new cases failed before fixes.
- Recovery tests count provider calls: existing vectors are rebuilt with zero
  inference calls, while a new pending upsert generates its own vector.
- Capacity test now inserts 40 extra nodes, exceeding the actual minimum of 16.
- MemoryStrategyTests, SleepCycleTests, MemoryRecallTests,
  MemoryRecallPhase2Tests, MemoryRecallPhase3Tests and HybridGraphBuilderTests
  pass three consecutive CTest runs. This is not every project test target.
- Windows Python validation now passes with the bundled `.venv` (`onnxruntime 1.20.1`):
  the quantized graph loads on `CPUExecutionProvider`, has the expected three
  `int64` inputs and `[batch, seq, 512]` hidden output, and produces normalized
  vectors with the semantic smoke-test thresholds. The reproducible command is
  `\.venv\Scripts\python.exe tools\validate_onnx_windows.py`.
- Native Windows C++ ONNX validation is now complete with the official
  `onnxruntime-win-x64-1.28.0` SDK unpacked under `third_party/onnxruntime`.
  `MemoryStrategyTests::testOnnxEmbeddingProviderLoadsAndEmbeds` passes with
  the 512-dimensional normalized output and semantic similarity assertions.
  CMake detects the SDK and deploys `onnxruntime.dll` beside the executables.

## Production Wiring (simplified)

- `ChatPreparationExecutor::Worker` owns `SemanticIndexService`, its ONNX
  provider, HnswEmbeddingIndex, MemoryIndexWorker and SQLite connection. The
  actual chat recall passes this index into graph/ACT-R retrieval. AIBrain
  supplies the assets path and queues a maintenance kick on `daydreamFinished`.
- Model loading, inference, index recovery and maintenance all run on the chat
  Worker thread. Startup initialization is queued, keeping the GUI responsive.
  Maintenance and recall serialize on that event loop (4 jobs / 30 s tick,
  200 ms follow-up while backlog remains). A long rebuild can still delay chat
  preparation; maintenance does not run concurrently with a recall.
- Compaction: when tombstone ratio >= 30% and idle, `rebuildFromRepository()`.
- Provider: `OnnxEmbeddingProvider::tryCreateFromAssets(<root>/assets)` only when
  `DESKTOP_PET_HAS_ORT`; Desktop_Pet now also compiles/links ORT when found.
  Without ORT the service stays disabled and recall uses keywords. Windows
  runtime loading and native embedding inference are verified in the test suite.
- hnswlib include dir is global (`include_directories`) because the chat Worker
  pulls the service into AGENT_RUNTIME_TEST_SUPPORT_SOURCES targets.

## Recall@32 / Latency Benchmark

- Added manual target `memory_hnsw_benchmark` (not registered in CTest) for
  synthetic vector regression. It builds 10,000 512-dimensional normalized
  vectors by default, compares HNSW Top-32 against exact cosine search, and
  prints compact JSON. Environment overrides:
  `DESKTOP_PET_HNSW_BENCH_COUNT`, `DESKTOP_PET_HNSW_BENCH_DIM`,
  `DESKTOP_PET_HNSW_BENCH_QUERIES`, `DESKTOP_PET_HNSW_BENCH_TOPK`,
  `DESKTOP_PET_HNSW_BENCH_M`, `DESKTOP_PET_HNSW_BENCH_EF_CONSTRUCTION`,
  `DESKTOP_PET_HNSW_BENCH_EF_SEARCH`, `DESKTOP_PET_HNSW_BENCH_SEED`.
- macOS synthetic baseline (no ONNX):
  - default M=16/efConstruction=200/efSearch=50: Recall@32 mean 0.6841,
    HNSW P95 2.94 ms, exact P95 76.5 ms, mean speedup 28.3x.
  - efSearch=200: Recall@32 mean 0.8334, HNSW P95 8.29 ms,
    exact P95 77.4 ms, mean speedup 9.47x.
- The synthetic random 512D corpus has very small margins among non-top1
  neighbors, so these values are regression baselines, not final product gates.
  A production gate still needs real/provider-backed samples and shadow Top-K
  comparison.

## Active Memory Pool Persistence

- The chat Worker is the sole owner of the production activation pool. It
  restores `memory_activation_snapshots` with offline decay on cache refresh,
  then saves the pool after selecting and forget-filtering prompt memories.
  The previous unused AIBrain pool and synchronous GUI recall are removed.
- Snapshot reads and writes join `memory_items` and filter inactive, sensitive,
  deleted and expired memories. Cache invalidation uses SQLite `data_version`,
  so external commits (including WAL commits) refresh the bounded working set.
- `MemoryStrategyTests::testActiveMemoryPoolPersistsAcrossRestart` covers save,
  restart/load, one-hour session half-life decay and deleted-memory filtering.
- `ChatPreparationExecutorTests::activePoolSurvivesWorkerRestartAndClear` covers
  activation through actual prompt preparation, restart retrieval outside the
  recent window, clearing both live recall and persisted activation, and importing
  the same ID without restoring its cleared activation. Snapshot refresh replaces
  the live pool and its decay baseline rather than merging stale cached entries.

## Recall Reads and Daydream Evidence (Windows, 2026-09-14)

- Semantic/activation/graph candidates hydrate through SQLite primary-key reads,
  including rows outside the 256-row recent window. Authoritative eligibility is
  checked before seed propagation and final ranking. Seed/graph budgets bound
  candidate counts; expression indexes cover recent/inbox ordering and evidence
  lookup. An empty SQL inbox no longer falls back to stale in-memory rows.
- Daydream retains source content, tags and evidence: successful consolidation
  marks the source `Consolidated`, discard marks it `Archived`. Result-to-source
  `DerivedFrom` edges and consolidation metadata preserve provenance. Result,
  source status, relation and outbox writes share the existing transaction.
  Archived sources are excluded from normal recall and subsequent Daydream
  batches; explicit user delete/clear remains physical.
- Native ONNX is enabled for the chat executor integration target as well as
  Desktop_Pet and memory_strategy_tests. Regression cases cover old-memory
  paraphrase recall with the real model, stale sensitive/deleted index hits,
  Worker-owned provider construction/use/destruction, nonblocking initialization,
  indexed SQL reads and archive-failure transaction rollback.

## Legacy Backfill / Coverage

- `MemoryIndexWorker::enqueueBackfillJobs()` now queues bounded upsert jobs for
  eligible legacy long-term memories that lack an embedding row for the current
  provider model. It skips existing Pending/Processing jobs and does not enqueue
  rows that already have current-model vectors.
- `MemoryIndexWorker::coverageStats()` reports eligible/indexed/pending counts
  over a bounded diagnostic scan. `SemanticIndexService::runOnce()` calls the
  backfill enqueuer before normal consumption, so startup idle ticks gradually
  index old databases without blocking startup.
- `MemoryStrategyTests::testSemanticIndexServiceBackfillsLegacyMemories` covers
  a legacy database with Active long-term rows but no outbox/embedding state.

## Shadow Retirement Gate

- Added `memory_index_health_daily` and `MemoryIndexWorker::recordHealthSample()`
  to persist daily model-specific coverage/health samples.
- `MemoryIndexWorker::retirementGateStatus()` implements the design gate:
  legacy full-scan retirement is allowed only when the requested window has all
  required days present, every day is healthy, and the worst daily coverage is
  at least the configured threshold (default 7 days, 95%). A failure sample keeps
  that day unhealthy even if a later same-day sample succeeds.
- `SemanticIndexService::runOnce()` records healthy samples after normal idle
  maintenance and records unhealthy samples on compaction rebuild failure. The
  gate is observability-only for now; it does not remove legacy paths.
- `MemoryStrategyTests::testIndexRetirementGateRequiresCoverageAndHealthyDays`
  covers the 6-day false case, 7-day true case, coverage regression and failure
  regression.

## Repair and Coverage Hardening (design §§4, 5, 14)

- Backfill inspects a bounded batch using an ID cursor. Pending jobs are excluded
  before LIMIT; foreign-model jobs are excluded before the consumer's LIMIT.
  Missing/stale vectors, dimension/blob mismatches, zero/non-finite vectors and
  missing/inactive labels enter the existing retryable outbox. Label loss is
  repaired before consumption; existing retry deadlines are preserved.
- Coverage validates vector shape/norm, current content hash and Active label.
  `valid` distinguishes SQL failure; `complete` distinguishes a partial scan.
  Incomplete/failed scans cannot pass retirement (the diagnostic cap is 4096).
  Larger databases require a future complete incremental coverage audit.
- Load/recovery failures persist unhealthy samples even after successful repair.
  Daily samples preserve worst coverage and failure errors. Retirement also
  checks current ready/coverage/pending state. This is still an observational
  gate, not proof of shadow result equivalence or permission to remove fallback.
- Regression: four tests first reproduced bad-vector coverage, backfill starvation,
  foreign-model starvation and SQL-failure false health; all pass after fixes.
  Additional checks cover partial coverage and corruption recovery health.
  Desktop_Pet builds and seven related CTest suites pass without ONNX.
- Retry delay now correctly caps milliseconds at 600000 (previous code applied
  the cap before converting seconds to milliseconds).

## Clear / Import Synchronization (design §§5, 14)

- Physical deletion retains stable HNSW label assignments until the consumer
  marks them deleted. This fixes failed deletion in a still-running index.
- Repository clear atomically removes memory/vector/snapshot/health state and
  replaces pending work with model-specific deletes for Active labels. Labels
  remain allocated, preventing reuse while live or on-disk HNSW holds them.
- Legacy JSON import inserts its index jobs in the same transaction as memories.
  Outbox failure rolls back import. Clear also rolls back if delete jobs fail.
- Tests cover live physical deletion, clear/import/reinsert, clear replay after
  restart, and outbox failure rollback. Desktop_Pet and seven suites pass.
  Deletion is asynchronous: consumers must run before raw HNSW search reflects
  clear; normal recall must continue checking authoritative memory eligibility.

## Remaining Acceptance Gaps

The earlier checklist overstated completion. Do not treat the scaffold commits
or automatic recovery tests as completion of design sections 4/5 or Phase 4.3.

- Outbox jobs now bind to the provider model after processing and persist the
  current content hash. A non-empty model mismatch is skipped without incrementing
  attempts. Current memory eligibility and content hash are rechecked after
  embedding, so stale upserts/deletes reconcile current state instead of applying
  obsolete commands. Existing empty model/hash jobs remain backward compatible.
- `next_attempt_at` and `last_error` were added through idempotent schema migration.
  Failed jobs use bounded exponential backoff (8 seconds initially, capped at
  10 minutes), and a failed job no longer prevents later jobs from running.
- Added tests for stale command reconciliation, model isolation, mutation during
  embedding, persisted backoff, and all-tombstone compaction. MemoryStrategyTests
  now has 106 passing cases. Embedding similarities now contribute to ACT-R
  semantic cue scoring, and batch selection follows CreatedTask/DerivedFrom
  causal edges in both directions.

- Background scheduling now runs on the chat Worker with its own SQLite
  connection (see Production Wiring); a separately scheduled maintenance thread
  is not required for GUI isolation. Producer-side jobs still begin with empty model/hash for backward compatibility;
  the worker fills these after successful processing.
- Clear/import through MemoryStore are covered above. Arbitrary direct repository
  updates still require a wider audit; callers must retain outbox synchronization.
- Full generation/SQLite consistency, every crash point and eligibility filtering
  across every legacy recall API remain unaccepted. Production provider ownership
  is now confined to the chat Worker and covered by lifecycle tests.
- Tombstone threshold detection and automatic idle-time compaction are wired in
  `SemanticIndexService`; compaction runs during an idle tick after pending
  index jobs drain, including the all-tombstone (`activeCount()==0`) case.
- Synthetic Recall@32 and latency benchmark harness exists (`memory_hnsw_benchmark`),
  with initial macOS baselines recorded above. Bounded legacy backfill and coverage
  counters are implemented for missing current-model vectors. Shadow retirement
  gate state/decision logic is implemented but not wired to disable legacy recall
  paths. Real-data/provider-backed gates and actual shadow Top-K comparison remain
  unaccepted. Invalid persisted vectors now use the repair queue above.
