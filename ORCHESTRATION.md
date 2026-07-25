# X3 — Subagent Orchestration Strategy

How to execute `ENGINE_PLAN.md` with multi-agent workflows. Written 2026-07-25.

---

## Model assignment

Cost and capability differ by roughly an order of magnitude across tiers, and most engine work is not architecture work. Assign by *what the task actually requires*, not by how important the phase feels.

| Tier | Use for | Do NOT use for |
|---|---|---|
| **Haiku** | Single-file mechanical edits. Deleting dead blocks, `.gitignore` rules, constant changes, renaming, doc stubs, format-preserving tweaks. Anything where the correct output is fully determined by the instruction. | Anything requiring a judgement call. Haiku will confidently do the wrong thing rather than stop and ask. |
| **Sonnet** | Multi-file mechanical refactors with pattern matching. Removing `#ifdef` branches across 20 files, call-site migrations, inventories, greps, CMake logic, verification passes. The workhorse tier — most implementation work lives here. | Novel design. Sync/barrier correctness reasoning. Anything where being subtly wrong is expensive. |
| **Opus / Fable** (inherit) | Design specs, API surface design, synchronization correctness, adversarial verification of correctness claims, cross-spec critique, anything touching Vulkan memory/barrier semantics. | Bulk edits. It is wasteful and no more accurate than Sonnet at mechanical work. |

**Effort dial matters as much as tier.** `effort: 'low'` on Haiku for mechanical edits; `effort: 'medium'` for Sonnet verification; `effort: 'high'` reserved for design and correctness reasoning. A Sonnet agent at high effort often beats an Opus agent at low effort for the same cost.

---

## The pattern that fits this project

Most phases decompose the same way:

```
Recon (sonnet, parallel by subsystem)
  → Design spec (opus, parallel by area, high effort)
    → Critique for gaps and contradictions (opus)
      → Implement (sonnet, pipelined, worktree-isolated if agents share files)
        → Adversarially verify (sonnet, 1-3 skeptics per change)
          → Build + validation-layer gate (deterministic, not an agent)
```

Three things make this work rather than produce plausible garbage:

**Specs are written by agents that cannot edit.** Separating "decide what to do" from "do it" stops an implementer from quietly redefining the task when it hits friction.

**Verification is adversarial and separate.** A verifier prompted to *refute* catches things a verifier prompted to *check* waves through. For correctness-critical work (barriers, descriptor lifetimes), run 3 skeptics with distinct lenses — spec-validity, does-it-actually-reproduce, and what-breaks-on-a-different-GPU — and require a majority to clear it.

**The build is the ground truth, not an agent.** Every implementation phase ends with a real compile and a real validation-layer run. Agents report success optimistically; compilers do not.

---

## Per-phase orchestration

### P0 — Hygiene · ~6 agents · mostly Haiku
Group edits **by file**, not by task, so agents cannot collide. One agent owns all CMake changes because three separate tasks touch `X3/CMakeLists.txt`. Verify each edit with a Sonnet skeptic checking specifically for *unrequested* changes — the most common failure mode on cleanup work is an agent "helpfully" fixing things nearby.

Delicate irreversible git operations (submodule removal, branch surgery) stay with the orchestrator. Do not delegate them.

### P1 — Vulkan-only + correctness · ~20 agents · mixed
Two very different halves, orchestrated differently.

*Deleting OpenGL* is mechanical and wide — Sonnet, pipelined over the file inventory, with worktree isolation off since each agent owns distinct files. Cheap.

*The correctness fixes* are the opposite: narrow, deep, and expensive to get wrong. Opus for the specs, Sonnet to implement against them, then a 3-skeptic adversarial panel per fix. This is the one phase where over-verifying is the right call — a descriptor lifetime bug that survives review will cost more to find later than the entire review costs now.

Gate: clean validation layers under load, not just "it compiles."

### P2 — Mesh attributes · ~12 agents · Sonnet-heavy
Touches assimp import, `AssetTypes`, `BVHAccel`, `Renderer`, and the shaders — a cross-cutting change where struct layout must stay byte-identical across C++ and GLSL. Fan out by subsystem, then a single Opus agent verifies layout consistency end to end. Add `static_assert`s on `sizeof`/`offsetof` as the machine-checkable guard until Slang reflection replaces them in P3.

Worktree isolation **on** — these agents genuinely share files.

### P3 — Slang migration · ~10 agents · Opus for the shader port
Small file count but high subtlety. The GLSL→Slang matrix-convention and function-rename traps are exactly the kind of thing that produces silently wrong images rather than compile errors, so this is not Sonnet work despite its size.

The reflection→C++ codegen tool is a separate, well-bounded Sonnet task.

Gate: rendered output pixel-identical to pre-migration. Diff the images; do not eyeball them.

### P4 — Job system · ~6 agents · Sonnet
Integration is straightforward. The thread-safety audit of `AssetManager`, `Log`, and `Profiler` is the real work — run it as parallel per-subsystem Sonnet agents, each asked to find races rather than confirm safety.

### P5 — Render graph · ~8 agents · Opus for design
New subsystem with no existing code to constrain it, which makes it the phase most vulnerable to an agent inventing something baroque. Use a **judge panel**: three independent Opus agents design a minimal render graph from the same brief, three more score them, then synthesise from the winner. Keep the v1 scope aggressively minimal (linear order, no reordering, no async compute).

### P6 — Material model + BSDF library · ~12 agents · Opus for the BSDF
Shading math is unforgiving and wrong-looking-but-plausible is the default failure. Validate every BSDF against Adobe's `openpbr-bsdf` reference numerically — furnace tests and white-furnace energy conservation — not visually. That validation harness is itself a good Sonnet task.

### P7 — Forward+ rasterizer · ~25 agents · the big one
The only phase large enough to need real decomposition. Split by pass: depth prepass, cluster assignment, light culling, opaque forward, transparent forward, velocity. Each pass gets a spec agent, an implementation agent, and a verification agent. Pipeline them so the depth prepass is being verified while light culling is still being written.

Gate per pass: renders correctly *and* the path-traced reference agrees on direct lighting. This is where keeping the reference renderer pays for itself — you have a ground truth no other small engine has.

### P8 — Shadows · ~10 agents · Sonnet
CSM is well-trodden; agents have seen thousands of implementations. The BVH shadow-ray path reuses the existing intersector, so it is integration rather than invention.

### P9 — Asset cook · ~15 agents · Sonnet
Wide and mechanical: BC7 compression, meshoptimizer integration, BVH serialization, binary scene format. Fan out by asset type. Each needs a round-trip test — cook then load then compare — which is a natural per-type verification agent.

### P10 — GI · ~20 agents · Opus for DDGI
Lightmap baking is mostly integration (xatlas, atlas packing, dilate) — Sonnet. DDGI probe update, octahedral encoding, and the Chebyshev visibility test for leak mitigation are subtle enough to warrant Opus plus adversarial verification. Light leaking is the classic failure and it will not be obvious from a single screenshot; build a leak test scene early.

### P11 — Post stack · ~10 agents · Sonnet
TAA, AgX tonemapping, bloom, XeSS integration. Well-documented territory. One Opus agent on TAA history rejection, which is where temporal artifacts actually come from.

### P12 — OpenPBR importer · ~10 agents · Sonnet
Offline tooling, no runtime risk, failures are visible and cheap. The parameter reduction mapping is the one part needing judgement — one Opus agent to define it, validated against the reference BSDF.

### P13 — Editor catch-up · ~8 agents · Sonnet/Haiku
UI work, interleaved throughout. Haiku handles panel boilerplate fine.

---

## Rules that keep this from going wrong

**Never run a mutating workflow without a working build.** If the tree does not compile before the workflow starts, agents cannot verify their own work and errors compound silently. This bit us immediately — `vulkan-headers` was missing, so nothing built at all.

**Branch per phase, commit per verified sub-step.** Recovery from a bad agent run should be `git reset`, not archaeology.

**Read `journal.jsonl` before trusting a workflow result.** Agents that die on a terminal error return `null`, and a `.filter(Boolean)` will silently shrink your work list. An empty result is not the same as "nothing to do."

**Cap the blast radius.** Tell agents explicitly what they may not touch — `X3/libs/`, git history, unrelated files. The instruction "make only the edits listed" is load-bearing and gets ignored without it.

**Spec agents must not edit; implementation agents must not redesign.** When an implementer hits friction and starts making architectural decisions, the spec was wrong — send it back rather than letting the implementer improvise.

**Scale verification to consequence.** A `.gitignore` edit needs one skeptic. A descriptor lifetime change needs three, with different lenses. Uniform verification effort wastes budget on trivia and under-checks the dangerous parts.
