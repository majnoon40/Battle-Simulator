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
| m13 item 5b — end screen (result, curves, save replay/report, rematch) + replay viewer (list, verify, scrub) | on branch m13-end-replay (PR follows when #14/#15 merge) | — | 2026-11-18 |
| m13 item 5c — setup editor (anchor spawn, steppers, save) + settings + scenario writer | on branch m13-setup-settings (PR follows) | — | 2026-11-18 |
| m13 item 6 — fuzzing (13 libFuzzer harnesses, seed corpus, CI 60s job, nightly 10min) | in this PR | — | 2026-11-18 |
| m13 item 7 — packaging, cross-platform CI, docs | pending | — | — |
