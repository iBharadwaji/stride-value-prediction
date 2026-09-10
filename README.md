# Stride value prediction with a squash-triggered selective filter

Value prediction for an out-of-order RISC-V core: a stride predictor, a value prediction queue that carries predictions through the pipeline in program order, and an extension of my own that suppresses predictions from a predictor entry already known to be wrong.

Full write-up, including results across 15 SPEC benchmarks and the reasoning behind the extension: **[bharadwajsudula.com/projects/out-of-order-value-prediction](https://bharadwajsudula.com/projects/out-of-order-value-prediction/)**

## This is a code exhibit, not a buildable project

`src/svp_vpq.h` contains only the value-prediction code I wrote. It will not compile on its own. The surrounding out-of-order simulator — fetch, decode, rename, dispatch, issue, execute, retire, the load/store unit, the branch predictors, and the Spike-based architectural checker — was course-provided infrastructure and is **not** included here, nor is the register-renaming implementation those methods sit alongside.

If you want to see the design rather than the plumbing, that file is the whole of it: about 290 lines, verbatim apart from normalised indentation.

## How it works

An SVP entry is indexed by PC and holds a tag, a saturating confidence counter, the last retired value, the observed stride, and a count of in-flight instances. The prediction is

```
pred = retired_value + (instance + 1) * stride
```

**On the `+1`:** `svp_prediction_theesko()` computes `retired_value + instance * stride`, and looks like it is missing the increment. It is not — `rename.cc` calls `svp_instance_plus_1()` immediately before it, so `instance` already includes the current instruction by the time the prediction is read. Splitting it this way keeps the queue bookkeeping and the arithmetic in separate methods.

Confidence is what keeps this from being reckless. At retire, if the newly computed stride matches the stored one the counter increments; on any mismatch it resets to zero. Only entries at maximum confidence may predict, so an instruction whose result moves unpredictably trains its entry into silence rather than into noise.

The VPQ carries each prediction in program order:

| Stage | Method | Action |
|---|---|---|
| Rename | `svp_anveshana`, `vpq_ketayimpu` | Look up the PC; on a confident hit, push onto the queue and bump the instance count |
| Rename | `svp_pred_confident`, `svp_prediction_theesko` | Decide whether to predict, and compute the value |
| Execute | `vpq_write_value`, `svp_record_squash` | Record the computed value; arm the filter if the prediction was wrong |
| Retire | `vpq_head_rdy`, `vpq_pop`, `svp_seekshana` | Pop in order and train from the **computed** value, never from the prediction |

Training from the computed value rather than the prediction matters more than it looks. Train from your own guess and the predictor becomes a feedback loop confirming whatever it already believed.

## The extension

The specified recovery scheme detects a value misprediction at **retire**. But the misprediction is known at **execute**, roughly 45 cycles earlier. During that window the entry is still saturated at maximum confidence, so every subsequent fetch of the same PC predicts again — and predicts wrong again. A tight loop can issue a storm of wrong predictions from one entry before the first reaches retire and resets the counter.

The asymmetry is the whole argument. A *missed* prediction costs one lost opportunity. A *wrong* prediction costs a full pipeline squash, roughly an order of magnitude more. So the baseline spends the entire execute-to-retire gap manufacturing expensive mistakes it already knows about.

The fix is one byte per entry:

- `svp_record_squash()` arms `penalty = 64` when execute detects a confident misprediction for that PC
- `svp_pred_confident()` decrements `penalty` on every confidence check and refuses to predict while it is non-zero
- allocation resets it to zero

Because the counter drains on lookups rather than on a clock, the cooldown scales with how hot the PC is — one in a tight loop burns through it quickly, a rarely fetched one holds it far longer. An entry that stops misbehaving heals on its own, so there is no reward path to maintain. The value 64 is empirical.

It intervenes at the point of *use*, never at training. Every specified rule survives untouched: the five specified fields, retire-time in-order queue-driven training, training from computed values, confidence reset on stride mismatch, unchanged eligibility logic, and full-pipeline squash recovery. To the specification, a filtered prediction is indistinguishable from an entry that chose not to predict.

The whole extension sits behind `RESEARCH_ENABLE` (`--research-mode=1`) and is a complete no-op by default, so the default build reproduces the specified design exactly.

## Not included here

**Recovery integration.** Predictions and instance counters are speculative state and must roll back with everything else. On a branch squash the VPQ tail is restored from the branch checkpoint, entries belonging to squashed instructions are invalidated, and per-entry instance counters are decremented so the predictor does not believe it observed values that never architecturally happened. That logic lives inside the framework's `checkpoint()`, `resolve()` and `squash()` methods, interleaved with register-renaming recovery, so it is described here rather than extracted. It is the part that is genuinely hard to get right.

**Storage accounting.** 160 bits per entry across 1024 entries, 20 KB against a 32 KB cap, of which the specified five fields are 19 KB and the filter adds 1 KB. Analytical, not synthesised — this is a simulation study.

## Provenance

Written for ECE 721 (Advanced Microprocessor Architecture) at NC State University, taught by Prof. Eric Rotenberg, as a value-prediction competition entry. The out-of-order framework and its reference-model checker were provided by the course; the value prediction subsystem and the selective filter are mine, and are the only things published here.

Method names are a mix of English and transliterated Telugu — `anveshana` (search), `ketayimpu` (allocate), `theesko` (fetch/take), `seekshana` (training), `nindindha` (is it full).
