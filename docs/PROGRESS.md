# Progress

One line per item: status, PR, date. Every PR updates this file. The
milestone detail lives in docs/milestone-13.md (and the earlier
milestone-N.md files); this table is the index.

| Item | Status | PR | Date |
| --- | --- | --- | --- |
| Project setup — public files, CI workflow | merged | #1 | 2026-11-15 |
| m11a — Navigation v2 (swap-orientation stall fix) | merged | #2 | 2026-11-16 |
| m11b — Player orders (army-level commands) | merged | #3 | 2026-11-16 |
| m11c — UI framework (widgets, layout, pure reducer, --ui-script) | merged | #4 | 2026-11-16 |
| m11d — Sandbox screens (menu, scenario picker, battle controls) | merged | #5 | 2026-11-16 |
| m12 audit — ordered armies fight again; parser hardening | merged | #6 | 2026-11-16 |
| m13 item 0 — state check | report only, in #7 | #7 | 2026-11-17 |
| m13 item 1 — window wiring for the sandbox screens | merged | #7 | 2026-11-17 |
| m13 item 2 — availability and termination v2 (--ai-avail / --end) | merged | #8 | 2026-11-17 |
| m13 item 3 — combat depth (--combat v2) + item 2 review | merged | #9 | 2026-11-17 |
| m13 item 3b — adversarial audit (13 findings fixed, 13 regression tests) | merged | #10 | 2026-11-17 |
| m13 item 4 — deterministic threads (--threads, worker pool, --profile-phases) | merged | #11 | 2026-11-18 |
| m13 item P — performance regression (routed-ally probe memo, byte-identical) | in this PR | #12 | 2026-11-18 |
| m13 item H(a) — hold arm kill attribution + Search analysis (report, tool) | in this PR | #13 | 2026-11-18 |
| m13 item H(b) — cost field + budget armies + 3 measured tuning rounds (balanced, not cyclic — honest miss on the strict RPS cycle) | in this PR | #14 | 2026-11-18 |
| m13 item 5a — battle view: minimap, squad selection, orders UI, markers | in this PR | #15 | 2026-11-18 |
| m13 item 5b — end screen (result, curves, save replay/report, rematch) + replay viewer (list, verify, scrub) | merged via #16 | #16 | 2026-11-18 |
| m13 item 5c — setup editor (anchor spawn, steppers, save) + settings + scenario writer | in PR #18 (stranded by merge order; rides the fuzzing PR) | #18 | 2026-11-18 |
| m13 item 6 — fuzzing (13 libFuzzer harnesses, seed corpus, CI 60s job, nightly 10min) | in PR #18 | #18 | 2026-11-18 |
| m13 item 7 — packaging (CPack/install + Release workflow), macOS/Windows CI, README/architecture refresh | in PR #19 | #19 | 2026-11-18 |
| m14 item 0 — type counters (validated combat.json list, capped product, v2-only; RPS acceptance PASS 0.640/0.695/0.585) | merged | #20 | 2026-11-19 |
| m14 item 1 — scale: query shadow + O(1) remove + faction-scoped query + fused termination sweeps (byte-identical, 487/487; perf-20000 +42% t1, perf-20000-squads +34% t2) | merged | #23 | 2026-11-19 |
| m14 item 2 — multi-faction battles (2-8 factions, fixed indices, sector spawn, any-enemy semantics, last-standing termination; 2-faction byte-identity pinned 498/498 + 13/13 export sweep) | merged | #23 | 2026-11-20 |
| m14 item 4 — cross-platform: sector directions as hex literals, Windows export-md5 pins (default + three-way), the cross-platform report (IEEE-op inventory, atan2 exposure, evidence matrix) | merged | #24 | 2026-11-20 |
| m14 item 3 — scripted AI generals (sandboxed Lua 5.4: decide(g) snapshot API -> m11b order pipeline; determinism pinned; 7 tests + docs/generals.md) | merged | #25 | 2026-11-20 |
| m15 S1 — invariant checker (--check-invariants[=N], default off; 24 checks over units/squads/orders/grid/counts; checker-on test builds; 26 new tests + CI job over all shipped scenarios, both orientations, threads 1/4, generals on/off) | merged | #26 | 2026-11-20 |
| m15 S2 — differential & metamorphic tests (6 DifferentialTest cases: side-swap fairness + spawn-translation pins, replay verify representatives + threads-4, grid==brute, era-flag composition, script==interventions bijection; CI differential job: all-19 replay sweep, 50-seed both-orientation fairness pin 49/50/1, grid==brute 7; finding: per-seed side-swap symmetry structurally absent — root-caused, documented, fix options in milestone-15.md) | merged | #27 | 2026-11-20 |
| m15 flee clamp — the S1 finding fixed (--flee-clamp on default; single choke-point clamp over all ten unclamped move paths; kSimVersion 13.2, closing the m14 counters gap; checker position-bounds strict again; 4 FleeClampTest + 2 InvariantsTest cases; OFF byte-identical to 13.1 on 26/26 battle pairs cross-binary, 2 alps battles changed, re-pin list empty, suite 546/546) | in this PR | #28 | 2026-11-21 |
