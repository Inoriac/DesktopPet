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
- No ONNX inference or ONNX tests were run.

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
  embedding and persisted backoff. MemoryStrategyTests now has 104 passing cases.

- No production background scheduler or Daydream completion wiring yet. Worker
  methods must still be called on a background thread with its own SQLite connection.
  Producer-side jobs still begin with empty model/hash for backward compatibility;
  the worker fills these after successful processing.
- Clear/import and direct repository mutation coverage still require auditing.
- Full generation/SQLite consistency, every crash point, cross-thread provider
  ownership and eligibility filtering across every recall API remain unaccepted.
- Tombstone threshold detection exists; automatic idle-time compaction is not wired.
- Recall@32, latency benchmarks, migration coverage and shadow retirement gates
  have not been measured or enabled. Invalid persisted vectors have no repair queue yet.
