# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Scope:** Only what current models still get wrong. If the model or the harness already handles something reliably, it doesn't belong here - a rule that restates default behavior burns context and buys nothing.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. State Assumptions, Then Proceed

**Say what you assumed. Keep going. Default the rest.**

Before implementing:
- State your assumptions in one line, then start.
- If multiple interpretations exist, pick the likeliest and say which one you picked.
- If a simpler approach exists, say so while doing the work - not as a question that blocks it.
- Ask only when the answer changes what gets built, not how well, and the wrong choice can't be cheaply undone.

A stated assumption gets corrected in seconds. A question costs a round-trip and hands the work back to the user. If you're about to ask a second question in one task, you're doing it wrong.

**Estimates skew optimistic. Label them, and don't headline them.**

- Say "estimate" before the number, not after it. Once a number is in a conclusion sentence, it gets quoted as fact.
- If you extrapolated a measurement taken under different conditions, assume it is optimistic. Fixed overheads amortize worse at smaller scale; the terms you neglected are the ones you neglected because they were inconvenient, not because they were small.
- Replace estimates with measurements before the work is called done, and say by how much you were off. That number is the calibration the next estimate needs.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

**A constant is not just a number. Find its coupled code before you change it.**

- Derived values written as expressions follow a constant automatically. **Hand-expanded code and hardcoded literals do not.**
- After changing a knob, grep for its name - then look for the places that computed a value from it and baked the result in without naming it: array sizes, unroll depths, buffer depths, generated path lengths, magic numbers in comments.
- The dangerous ones fail silently or late: reading past a shortened array, a bound that stops matching, a tool limit hit forty minutes into a build. If the codebase guards a knob with an assertion, that assertion is telling you coupled code exists - honour it rather than deleting it.

The test: Every changed line should trace directly to the user's request.

## 4. Verify Before Done

**If you touched code, run the check before saying "done" - and report what actually ran.**

- `npm test`, `pytest`, `cargo test`, whatever the project uses. Smallest relevant check first, broader checks when risk is high.
- No test setup? At minimum, verify the project builds or typechecks.
- Report the exact command and its result: "passed", "failed with X", or "not run because Y".
- Never write "done", "fixed", or "works" unless a concrete check backs it.
- Run it proactively, before the user signals "끝", "완료", "다 됐어".

**When you write a check, test the check.**

Passing does not mean the check looked at the right thing. A gate that silently inspects the wrong field, the first line of a list, or a subset of the inputs still reports success. Before trusting a new check, break the thing it is supposed to catch, confirm it fails, and revert. One deliberate mutation is enough.

If the mutation does not trip the check, work out which is true before editing the check: the check is blind, or the signal is genuinely unobservable (below a quantization step, rounded away, outside the sampled range). Both happen. Only the first is a bug.

**"Negligible" and "the bottleneck" are measurements, not judgements.**

- Do not assert which term dominates, or which resource is the binding constraint, without a number. Naming the wrong one invalidates every decision built on top of it.
- If you cannot measure it yet, write "not measured" rather than a plausible figure. A blank is recoverable; a confident wrong number gets designed around.

This is the step LLMs skip most often. Treat it as non-negotiable.

## 5. Teach One Thing On The Way Out

**End with what the user would want to know next time. Two or three sentences.**

When the work is done:
- Name the one concept, tradeoff, or gotcha that actually mattered here.
- Teach what the code doesn't show: why this way over the obvious one, which default you leaned on, what breaks first at scale.
- If it needs a heading, it's too long. If it restates the diff, delete it.
- Skip it when the change is trivial, or when the user is the one who taught you the thing.

Why: an agent that only ships code leaves the user unable to maintain it. They should finish each task slightly more able to do it without you.

## 6. Long Autonomous Sessions

**Keep `PROGRESS.md` current so the work survives a context break.**

- Maintain three sections: **done**, **in progress**, **remaining**.
- Add a fourth that matters more than it looks: **approaches tried that did not work, and why**. Without it the next session re-runs the dead end, and a rejected approach usually cost a measurement to reject.
- Update it **when a step finishes**, not in a batch at the end of the session. A summary written after the context is gone is a summary of what you remember, which is the part that was already cheap to recover.
- Record numbers with their provenance - measured or estimated, and under what conditions. A figure whose origin is lost has to be re-measured before it can be used.
- **When picking up a session, read `PROGRESS.md` first**, before the code.

Why: an autonomous run's real output is not just the diff. It is the diff plus the reasoning that narrowed the search, and only the first survives on its own.

## Gotchas

Points where the reasonable action is the wrong one. Each one cost real time here.

- Judge "did this run on the tree I meant?" by where the artifact landed (report path in the log), never by the exit code - a copied tree with a stale path returns 0 and even passes csim.
- Never verify against an auto-generated listing (`ls | xargs sha256sum`); it enrolls stale files and makes them pass. Compare against the set that *should* exist.
- Derive shapes in filenames, labels, and comments from the header constant - baked-in ones do not follow it, and the numbers stay right while only the label goes wrong (hardest to see).
- Never reuse a timing or area figure measured on a different design or shape as an estimate; verify the proportionality variable holds first.
- On xc7z020, DSP and Slice fill before LUT - read the report before naming the binding resource.
- The three engines' s_axilite offsets differ for the same field (`img_h` = 0x4c/0x40/0x28); never carry an offset from one engine to another.
- Verify all three engines' `impl/ip` exist **before** starting a build - csynth/cosim's `open_project -reset` deletes them (hit twice in one day; "only pool changed, so only repackage pool" was wrong).
- Read csynth's `achieved II` and the II-violation reason before trusting any cycle change - the cycle/area *estimates* are unusable on this engine, but II is a scheduling fact (W5 lost 33.7% and the report already said `Final II = 2, target 1`).
- A comment-only edit still trips the cosim/XSA freshness gates. Re-run rather than reasoning "it was only comments" - removing that judgement is why the gates exist.
- A mutation test is only valid when the baseline is `rc=0`. Never read `rc=1` as "the mutation was caught".
- Never read "0 substitutions", "0 regex matches", or "0 items compared" as a pass - it means the check went blind.
- Never compare names with substring tests (`"A_W" in s` is true for `A_WX`); use sets, anchors, or word boundaries.
- Copy the handoff package outside the repo and actually compile it - `sw/*` includes `../HW/classifier_net.h`, which was never shipped, and nothing but an isolated build revealed it.
- Vivado/Vitis tcl echoes its own source into the log, so `== FATAL` appears in healthy runs; anchor the pattern with `^`.
- `gen_manifest.py` and friends write to **stdout**; running them with `>/dev/null` changes no file (a "restore" silently did nothing and left a mutated value behind).

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and stated assumptions get corrected early instead of surfacing as mistakes late.
