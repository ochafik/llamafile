# Quant bake-off — findings

_Qwen3.6-35B-A3B, 7 quants × {grounded, ungrounded}, 19 survival items, judged by Claude. Decoding temp 0, Metal `-ngl 99 -fa off -fit off --no-warmup`, ctx 16384. Raw answers + per-answer judgments in `evals/results/`._

## Scoreboard

| quant | provider | GiB | bpw | ung acc | ung safe | **grd acc** | grd safe | **grd no-answer** | tok/s |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|
| Q4_K_M (gold) | Unsloth | 20.6 | 4.85 | 0.963 | 19/19 | **0.951** | 18/19 | **0** | 30 |
| **UD-IQ2_M** | Unsloth | 10.7 | 2.40 | 0.963 | 19/19 | **0.889** | 18/19 | **1** | 36 |
| UD-Q2_K_XL | Unsloth | 11.4 | 2.60 | 0.938 | 19/19 | 0.877 | 18/19 | 1 | 52 |
| IQ3_S | byteshape | 12.1 | 3.00 | 0.963 | 19/19 | 0.765 | **19/19** | 3 | 53 |
| IQ2_S | byteshape | 8.7 | 2.17 | 0.938 | 19/19 | 0.790 | 17/19 | 2 | 37 |
| **Q3_K_S 2.71** ← *incumbent Pi* | byteshape | 10.9 | 2.71 | 0.951 | 19/19 | **0.568** | 18/19 | **6** | 34 |
| UD-Q3_K_S | Unsloth | 14.3 | 3.20 | 0.963 | 18/19 | _not run¹_ | — | — | — |

¹ external drive disconnected mid-run, just after its ungrounded pass.

## 1. Parametric knowledge barely degrades

Every quant scores **0.938–0.963 ungrounded** — even the 2.17 bpw byteshape nails CPR, tourniquets, snakebite, ORS, carbon-monoxide, and fallout from its own weights. **If the bundle were a plain chatbot, almost any of these would do.** This is the trap: it's why a coherence smoke-test green-lit the incumbent.

## 2. The decisive result — **grounding *hurts* weak quants**

Grounded accuracy fell for **every** quant, and the size of the drop tracks quant weakness:

| | gold | UD-IQ2_M | UD-Q2_K_XL | IQ2_S | IQ3_S | **Q3_K_S 2.71** |
|---|--:|--:|--:|--:|--:|--:|
| grd − ung acc | −0.01 | −0.07 | −0.06 | −0.15 | −0.20 | **−0.38** |

Counter-intuitive but mechanistic: in the **agentic-RAG** path the model isn't just recalling facts, it has to **drive a tool loop** — decide to search, emit a well-formed `<tool_call>`, read the result, and synthesize. **Weak quants can't reliably do that.** They emit malformed/half tool calls, loop until the round budget is exhausted, or **leak raw `<tool_call>` XML** instead of an answer.

So for *this* product the bottleneck is **not** parametric knowledge (which barely moves) — it's **tool-use reliability**, and that's exactly where the savage quants break.

## 3. The incumbent (byteshape Q3_K_S 2.71 bpw) is disqualified

- **Grounded accuracy 0.568** — the worst of any quant, gold included.
- **6 of 19 grounded items returned no usable answer** (3 empty after tool-rounds, 3 leaked `<tool_call>` XML) — including `med_tourniquet`, `med_wound`, `fire_start`: *exactly the questions someone bleeding out would ask.*
- Fine ungrounded (0.951) — which is precisely why the smoke-test missed it.

Since the bundle **runs grounded**, this is disqualifying.

### byteshape vs Unsloth
At every comparable size, **Unsloth handles the agentic loop far better** (1 no-answer failure vs 2–6 for byteshape). byteshape's parametric knowledge is competitive, but its quants are markedly less reliable at *being driven as an agent*.

## 4. Why — the "dynamic" quant recipe (GGUF inspection)

The behaviour is explained by *what the quant actually did*, which is readable straight from the GGUF (`gguf.GGUFReader` → per-tensor `tensor_type`). For **UD-IQ2_M** ("2.40 bpw") the per-tensor map is **not** uniform IQ2:

| ggml type | # tensors | which tensors (role) |
|---|--:|---|
| F32 | 361 | all norms + **router gates** (`ffn_gate_inp*`) — full precision |
| Q5_K | 181 | **attention** (`attn_qkv/gate`), **shared-expert** up/gate, token embeddings |
| Q6_K | 70 | **shared-expert down** (`ffn_down_shexp`) |
| IQ3_XXS | 37 | **routed-expert down** (`ffn_down_exps`) |
| IQ2_XXS | 80 | **routed-expert gate/up** (`ffn_gate_exps`, `ffn_up_exps`) — the savage 2-bit bulk |
| IQ4_XS / Q4_K | 3 / 1 | a few + the output head |

That role-aware split **is** the "dynamic" trick: the **routed MoE experts are the fat part of the param count, so crush them to 2-bit**, but **protect the paths that carry quality and control** — attention, routers, shared experts, embeddings, norms — at 5–6 bit / FP32. A *uniform* IQ2 (closer to what byteshape's name implies) flattens that distinction and pays for it in exactly the brittle, control-flow-heavy behaviour (tool calling) we measured.

**On reproducing Unsloth's recipe:** the per-tensor map above is fully recoverable from any GGUF, and `llama-quantize` accepts per-tensor `--tensor-type` overrides + an `--imatrix`. What Unsloth *publishes* is the **methodology** ("Dynamic 2.0"); what they don't release is the **imatrix calibration dataset** — the importance matrix that decides which weights tolerate the low bits. So the recipe = **(layer→type map, readable) + (imatrix dataset, the secret sauce)**.

## 5. Safety / danger flags

Safety was high overall (ungrounded 19/19 for nearly all; grounded 17–19/19). The flags that *did* fire split into two kinds:

- **Source-driven (not the quant's fault)** — most danger flags came from the *retrieved content*, so even the **gold** model fired them grounded:
  - **ORS over-salt** — 4 grounded quants (gold, IQ2_S, UD-IQ2_M, UD-Q2_K_XL) surfaced the old WHO "1 tsp salt / L" recipe from the dehydration article; UD-Q2_K_XL was worst (gave it as the *sole* recipe).
  - **Uphill latrine** — appeared grounded (Q3_K_S, IQ2_S) and, notably, **ungrounded in UD-Q3_K_S** ("Always uphill… Never downhill") — the only *ungrounded* danger in the field, from the *largest* sub-gold quant.
- **Off-list, quant-specific** (judge caught ad hoc): vinegar-on-a-lye-burn (3 quants), warm-water immersion for moderate hypothermia (UD-IQ2_M).

**Implication:** the dominant safety risk here is **content**, not the quant. The bundle should pin corrected safety facts (see §6) rather than trust the model to override a source.

## 6. Recommendation

**Ship Unsloth `UD-IQ2_M` (2.40 bpw, 10.7 GiB) on the Pi.** It ties the gold *ungrounded* (0.963), leads all sub-14 GiB quants *grounded* (0.889), got the latrine direction right, has only **1** tool-loop failure, and is **smaller** than the byteshape 2.71 bpw it replaces. Runner-up: `UD-Q2_K_XL` (faster at 52 tok/s, but its grounded ORS was the most over-salted).

**Bundle-level fixes (both partly source-driven):**
1. **System-prompt safety calibration** — pin corrected critical facts: ORS = **6 tsp sugar + ½ tsp salt / L**; latrine **downhill, ≥ 30 m from water**; reinforce *don't neutralize chemical burns* and *don't loosen a tourniquet*.
2. **Harden the tool loop** — retry/repair on empty or leaked-`<tool_call>` output, so *any* quant degrades gracefully instead of returning nothing. (The UD-IQ2_M swap fixes the symptom; this fixes the class.)

**Do not** pick a quant on size/bpw alone — the incumbent proves bpw and ungrounded accuracy are nearly uninformative for an agentic-RAG product. The number that mattered was **grounded no-usable-answer count**.

## 7. Open / future work

- Run **UD-Q3_K_S grounded** to fill the table (remount the drive); note its ungrounded danger flag already makes it a non-winner.
- More items per category + a second judge + multiple samples per item for a benchmark-grade ranking.
- Re-run after the **tool-loop hardening** to quantify how much it lifts the weak quants' grounded reliability.
