# HNSW / Outbox Implementation Status

Related design: design.md sections 4, 5, 14, 15, 16.

## Checkpoints

- `10178e2`: initial HNSW wrapper, label table and outbox worker scaffold.
- `d65aa65`: basic outbox consumption test.
- Current durability fix: persist SQLite vectors and HNSW files before completing
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
- testTransactionRollbackRevertsWrites and testTransactionCommitRetainsWrites
  fail on both the current working tree and a separately built d65aa65 worktree.
  These failures remain unresolved; this is not a full-suite pass.
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
