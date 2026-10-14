# Sprint 5 — HLE guest pointer fail-closed (host synthetic)

The laboratory NID handlers in `core/hle.cpp` now reject unreadable names/attributes, invalid or read-only output ID slots, and invalid/read-only timeout pointers before entering blocking synchronization operations. ID creation rolls back on output-store failure; direct-memory allocation unmaps on output-store failure. Synthetic host cases in `tests/test_hle.cpp` exercise these rejection paths and the memory budget after rollback.

Scope: these are the project's laboratory NIDs and simplified 3-argument calling convention, not verified PS4 ABI bindings. No claim is made that the game imports or calls these handlers. Tests use only generated guest memory on Windows; there is no private game data, device execution, concurrency race proof, or real-game boot validation. Concurrent guest remapping of an output slot between validation and store is not made atomic; the post-operation rollback is best-effort for this race.
