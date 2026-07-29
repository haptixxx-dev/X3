# Render tests

Golden-image regression tests for the renderer. `X3RenderTest` renders a table
of scenarios, reads the frames back off the GPU, and diffs them against
committed reference images.

```
./scripts/render-test.sh                    compare against tests/golden
./scripts/render-test.sh --update-goldens   re-record them
./scripts/render-test.sh --filter phong     only matching scenarios
./scripts/render-test.sh --tolerance 0.005  loosen the RMSE gate
```

It builds before it runs, deliberately: a golden-image run against a stale
binary is worse than no run at all, because it reports green for code that is
not what is on disk.

**Exit code 0 only if every scenario passed.** A scenario with no golden counts
as a failure, not a pass — otherwise an empty `tests/golden/` would look like a
clean run.

## Why this exists

Every gate this engine had before it answered *"does it crash, and does Vulkan
complain"*. None of them looked at the image.

Phases 2 through 5 all passed `verify.sh` with completely clean validation while
the committed fixture rendered an untextured model. A human eyeballing a
screenshot is what caught it, two phases after it was introduced. That class of
bug — the renderer is healthy, the image is wrong — is invisible to every other
check in the repo.

It also unblocks two gates `ENGINE_PLAN.md` asks for and that were previously
unrunnable:

- **Phase 3** wanted the post-Slang image diffed against the pre-Slang one.
- **Phase 7** wants every raster pass diffed against the path-traced reference,
  which is the entire reason for keeping a reference renderer. Building
  Forward+ without image diffing means eyeballing fifteen passes by hand.

## What it is not

**It is not part of `verify.sh`.** That one is the per-commit gate: it builds,
runs both binaries, and fails on any validation message. This is the thing you
run when you want to see where the renderer actually stands.

**It is not headless.** A display connection is required. The window is created
unmapped (`WindowProps::Hidden`) so nothing flashes across the desktop per
scenario, but the frame lifecycle still goes through swapchain acquire, so this
will not run over a bare SSH session or in CI without one. Making it truly
surfaceless means an offscreen path through `beginFrame`/`endFrame` and is
deliberately out of scope.

## Output

| Path | What |
|---|---|
| `build/render-test/<name>.png` | this run's render |
| `build/render-test/diff/<name>.png` | where it differs from the golden — **only written on failure** |
| `build/render-test/contact.png` | every scenario tiled into one image |
| `tests/golden/<name>.png` | the committed reference |

The **contact sheet** is the thing to actually look at. It is the whole
renderer's state at a glance rather than N file opens.

The `diff/` directory is **wiped at the start of every run**. A diff only means
something relative to the run that produced it, and a passing scenario writes
none — so without the wipe, an old failure sits there looking current. This
matters more than it sounds: a diff is black wherever the two images agree, so a
stale diff of a small regression is a mostly-black frame that reads as *"the
renderer is drawing nothing"*. It was misread exactly that way within minutes of
the harness existing.

## Determinism

Runs are **bit-exact**, including the path tracer — a second run reports
`rmse 0.0000` on every scenario. That is not luck:

- The path tracer's RNG is seeded from `(pixel, accumulated frame index, sample
  index)` (`InitRngState` in `PathTracing.slang`), so a given frame count
  reproduces exactly.
- Every scenario fixes its frame count, sample count and resolution. Bounds are
  never time- or convergence-based, which would make the golden depend on how
  fast the machine is.
- Scenarios always use the **scene camera**, never the editor camera. The editor
  camera starts at the origin looking at empty sky — that is what made the first
  manual screenshot of this fixture show nothing but clouds — and it is not a
  reproducible starting point.
- vSync is forced off, so nothing waits on a refresh rate.

## Sensitivity

Verified by deliberately breaking a shader: changing Phong's ambient term from
`0.1` to `0.13` is caught as `rmse 0.0041, max delta 3`. Three levels out of 255
is not something anyone spots in a screenshot.

The default tolerance is `rmse <= 0.002`. Diff images are **normalised to their
own maximum** rather than amplified by a fixed factor — at any fixed gain a
max-3 regression renders as near-black, which reads as "nothing changed" exactly
when something did. The magnitude is what the reported `rmse` and `max` are for;
the image only has to answer *where*.

## The fixtures

| Project | What it covers |
|---|---|
| `TestProject/` | An imported `.glb` with embedded textures. Also `verify.sh`'s smoke test. |
| `tests/scenes/materials/` | 5x5 sphere grid: metal / dielectric / clearcoat / sheen / anisotropy, each row sweeping its parameter. |
| `tests/scenes/lights/` | One object per light type, separated, each casting its own shadow. |

The two under `tests/scenes/` are **primitive-only** and depend on nothing in
`SampleModels`, so they work on any checkout. Regenerate them with:

```
./build/debug/Debug/X3FixtureGen --fixture materials --out tests/scenes/materials --force
./build/debug/Debug/X3FixtureGen --fixture lights    --out tests/scenes/lights    --force
```

Rows in the materials grid are deliberately separated by lobe so a regression
moves one row and leaves the others alone. That is what makes a failing diff
tell you *which* lobe broke rather than only *that* something did.

## Adding a scenario

Add an entry to `tests/scenarios.yaml`, then record its golden:

```yaml
  - name: my-scenario
    project: TestProject/TestProject.lrproj
    shader: pathtracing        # pathtracing | pbr | phong
    width: 640
    height: 360
    raysPerPixel: 2
    bouncesPerRay: 4
    accumulate: true
    debugMode: 0               # 0 shaded, 1 AABB heatmap, 2 triangle heatmap
    frames: 32
```

```
./scripts/render-test.sh --filter my-scenario --update-goldens
```

Keep resolutions small. These are regression images, not wallpapers, and they
live in git.

The heatmap scenarios exist to cover geometry and the BVH independently of
shading: if the mesh format or the accelerator regresses, they move even when
the shaded images do not.

## Re-recording goldens

`--update-goldens` overwrites without comparing. **Look at the contact sheet
before committing the result** — the whole point of the goldens is lost if a
regression is recorded as the new truth. A golden should only change when the
output changed for a reason you can name in the commit message.

Bumping the pinned Slang version (`scripts/fetch-slang.sh`) counts as a reason
to re-verify: a compiler upgrade is a shader change.

## How it works

`VulkanContext::readbackImage` copies the render target into a host-visible
staging buffer through `beginSingleTimeCommands`/`endSingleTimeCommands`. It
**blocks and is out-of-frame only** (asserts `!frameActive()`), which is the
right shape for its only caller: the harness renders N frames, closes the frame,
and then reads. It preserves the image's tracked layout by transitioning back to
whatever it was, so `VulkanImage::m_Layout` stays true and the next frame's
barrier still derives from a correct starting point.

The encoder **flips vertically**, and that is required rather than cosmetic. The
target is stored with Vulkan's top-left origin while the shaders build NDC with
`+y` up, so row 0 is the bottom of the picture. Both display paths already
compensate — `blitImageToSwapchain` carries an explicit Y-flip and the editor
samples through flipped ImGui UVs — so a PNG written from the raw rows comes out
upside down and disagrees with every screenshot of the same scene.

No tonemap or gamma is applied on encode. PBR and Phong already tonemap and
gamma-correct internally and the path tracer deliberately does not, so anything
applied here would make the PNG disagree with what the editor shows.
