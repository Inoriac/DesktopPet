# HNSW / Outbox Implementation Status

Related design: design.md sections 4, 5, 14, 15, 16.

## Checkpoints

- `10178e2`: initial HNSW wrapper, label table and outbox worker scaffold.
- `d65aa65`: basic outbox consumption test.
- `142e12e`: persist SQLite vectors and HNSW files before completing
  a job; failed saves leave jobs retryable. Delete also removes stored vectors.
  Binary and metadata files use QSaveFile individually; SHA-256 detects a torn
  pair. A failed load clears readiness instead of exposing a partial index.

## Verification

- Desktop_Pet and memory_strategy_tests build successfully on macOS.
- Six focused test methods pass: HNSW search/persistence, basic consumption,
  durable completion/Processing replay, failed-save retry, durable deletion,
  and mismatched/corrupt file rejection.
- The durable-completion and failed-save tests both failed before this fix.
- Existing SQLite embedding and entry-update tests pass.
- The two transaction regressions are now resolved: nested BEGIN calls in
  Daydream -> addEntry -> persistMutationBatch failed in SQLite. Repository
  transaction scopes now use a savepoint stack, including inside external
  QSqlDatabase transactions. Closing the repository clears scope bookkeeping.
- New tests cover inner rollback/outer commit, outer rollback of entries,
  relations, tags and outbox, injected outbox failures, external transactions,
  and rollback/retry of an entire two-item Daydream batch.
- MemoryStrategyTests, SleepCycleTests, MemoryRecallTests,
  MemoryRecallPhase2Tests, MemoryRecallPhase3Tests and HybridGraphBuilderTests
  all pass three consecutive CTest runs. Desktop_Pet builds successfully.
  This verifies these six suites, not every project test target.
- Two stale test fixtures were corrected: mentionCount=3 selects Semantic,
  not Episodic; Phase 3 now initializes its schema via the real repository.
  A staged-memory assertion now looks up the entry by ID instead of relying
  on database row order.
- No ONNX inference or ONNX tests were run.

## Remaining Acceptance Gaps

The earlier checklist overstated completion. Do not treat the scaffold commits
as completion of design sections 4 and 5 or Phase 4.3.

- Rebuild must read authoritative memory_embeddings, not re-embed memory_items.
  Automatic recovery from a rejected index is not implemented. The consumer
  currently refuses to replace an unavailable index when Completed jobs exist;
  it requires recovery before continuing.
- Label allocation/update semantics, changed-content hashes, capacity growth,
  and tombstone accounting/compaction need dedicated tests and fixes.
- Outbox currently covers persistMutationBatch, not every repository update,
  status transition or physical deletion. Model/content version checks and
  bounded backoff still need implementation.
- No production background scheduler or Daydream completion wiring yet.
- GUI/provider thread ownership, eligibility filtering across all APIs, index
  generation/SQLite consistency and full crash-point replay remain unaccepted.
- Recall@32, latency benchmarks, migration coverage and shadow retirement gates
  have not been measured or enabled.
