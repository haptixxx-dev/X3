# TODO_VULKAN.md — superseded

Every item here is either done or absorbed into `ENGINE_PLAN.md`, which is the
authoritative plan. `PROMPT_CONTINUE.md` says where work stopped.

Phases 0-5 are complete as of 2026-07-29. Two gates were not met and are
recorded at the top of `PROMPT_CONTINUE.md` rather than dropped.

One item from the original file is worth keeping, because it was WRONG and
someone will otherwise re-raise it: "add a `-DVULKAN` define when compiling
shaders" was never necessary. `glslc` predefines `VULKAN=100` when targeting
Vulkan, verified two ways. The point is moot now anyway -- the shaders are
Slang, compiled by `slangc` (see `scripts/fetch-slang.sh`).
