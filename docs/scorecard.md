# Scorecard (2026-10-14 self-assessment)

Moved from the README. Evidence for each line is in
[AUDIT_2026-10-14.md](AUDIT_2026-10-14.md) and
[REQUIREMENTS_MATRIX.md](REQUIREMENTS_MATRIX.md).

| Area | Score | Why not higher |
|---|---|---|
| Fresher-portfolio strength | 8/10 | Strong, tested end-to-end stack, but a single author's evidence; no external users. |
| Engineering maturity | 7/10 | Good differential/fuzz/sanitizer coverage; native backend has no GC and is not run under ASan. |
| Deployment readiness | 5/10 | CI is green, but there are no releases, packages or install docs beyond a source build. |
| Completion of intended scope | 7/10 | Backend, opt-equivalence, GC stress and fuzzing done; native GC, AOT, reference-language benchmarks missing. |
