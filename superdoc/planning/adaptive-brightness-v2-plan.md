# Adaptive Brightness v2 — a visibility-first local tone operator

## Decisions (2026-09-14)

**The plan below proposed REPLACING Adaptive Brightness, Adaptive Gamma and the
Brightness Map with one operator (§0, §5.3, §6, §8.1). The user overrode that, verbatim:**

> Call it "Adaptive brightness V2" in the GUI. Implement it fully, so I can test it
> later. DO NOT REMOVE THE ORIGINAL!

So: **v2 is a NEW, ADDITIVE effect.** Adaptive Brightness, Adaptive Gamma and the dark
floor (added the same day, `1a40b9e`) stay **exactly as they are** — untouched code,
untouched config, untouched panel rows. Every place below that says "replaces" or
"removed" for those three describes the ORIGINAL proposal, not what was built; §5.3's
table and §8.1's "Recommended decisions" are **superseded** in full, and §5's own
schema-bump/migration (§8.1's "Config keys" row) was **not executed** — there is no
`kCurrentSchemaVersion` bump, because v2's keys are purely additive
(`reshade.adaptive_brightness_v2`), the same shape every other additive effect in this
pipeline (Bloom, Adaptive Gamma, the dark floor) was added in.

**Open questions (§8.2), answered by the lead, as defaults a later session can revisit:**

1. **Toe or knee?** Toe is the default; **Knee is offered as a `Shape` Choice row**, not
   dropped — one formula each (see `shader-effects.md`'s own section for the knee
   construction actually shipped, which is NOT the plan's unspecified placeholder).
2. **Adaptation default:** **Scene** (not Off) — a fresh profile deepens the lift on a
   genuinely dark map by default; a competitive player who wants zero exposure movement
   switches to Off.
3. **Max lift ceiling:** **1..8**, default **4** — wider than Adaptive Gamma's 1..4,
   since a bounded slope makes a harder ceiling safe (the plan's own §0 argument for why
   binarisation cannot recur here).
4. **Colour:** v2 **preserves chroma ratios** (`RGB′ = RGB · Y′/Y`), as the plan's §4.7
   specifies. Adaptive Brightness and Adaptive Gamma's own per-channel convention is
   **unchanged** — this pass does not touch either.
5. **HUD/crosshair exclusion:** **not built** in this pass. The measure pass's statistics
   (and v2's own content anchor) read the whole graded frame, HUD and crosshair included,
   exactly as Adaptive Brightness/Gamma already do.
6. **Stage 3 Clarity:** **will be built**, as the next task, not this one. `kParamBudget`
   has one row spare on v2's own Switch for it.

**Status update (2026-09-14, second pass): Stage 0 (GPU timestamp) and Stage 3 (Clarity)
are now BOTH built**, on top of the Stage 1+2 pass below. Stage 0: a `vkCmdWriteTimestamp`
pair, double-buffered across four queries, brackets the whole pre-pass; exposed as the
`effects_timing` ConCommand and the Shaders area's Diagnostics "pre-pass" fact. Stage 3:
the silhouette band, as a SECOND guided-filter coefficient pair at `r₂ = r/4` rather than
the plan's own "widen v2A to 2× the width" alternative (§5.1) — a second sampler slot
needed no change to any existing kernel loop, where the widened-buffer alternative would
have needed every box filter's loop (three shader files) to stop reading across a half-
boundary seam. `kParamBudget`'s 8 is now fully spent on this row. §7's new harness scenes
(`silhouette`, `skyfore`, `flash`, `--image`) and its thirteen checks are also built, in
`scripts/effects-regression.sh` / `effects_regression_sample.py` — see
`shader-effects.md`'s own "Measured" section for the numbers and every deliberate
deviation (the `flash` scene's own `--flash` timer is built but the automated `abv2-cut`
check uses a plain `silhouette → bright` SIGUSR1 hop instead, since a screenshot-polling
harness cannot land on a frame-counted phase boundary deterministically — the plan's own
"driven by SIGUSR1 or a --flash timer" wording sanctions either). §6's shadow-chroma and
knee-variant extras remain not built; nothing else in this plan changed.

**Measured vs. predicted cost (§5.2's table).** Stage 1+2's own predicted range was
"≈ 0" (Stage 1) plus 0.2–0.3 ms (Stage 2); Stage 3 added another predicted +0.1 ms. The
FIRST real GPU measurement (this pass, harness resolution 1280×720, desktop GPU) was
**mean 179.2 µs (0.18 ms) at 1280×720, 0.22 ms at 1920×1080** for the WHOLE pre-pass
block (measure + Bloom + V2's own dispatches + apply), not V2's cost alone — see
`shader-effects.md` for the full table this number came from, how it was captured, and
the one number that changed once the pre-pass was measured with V2 subtracted out
(2026-09-14 QC): at 1920×1080 on an RX 7900 XTX, V2's OWN cost over the 0.10 ms
pre-pass baseline (Saturation only, V2 off) is **+0.08 ms at defaults, +0.10 ms at
Clarity 0.5** (AB Dynamic, for comparison, is 0.14 ms for the whole pre-pass); the
harness's own 1280×720 mean settled at **181.6 µs**. The halo deviation the same QC
pass found is not a bound violation any more — see `shader-effects.md`'s "Measured"
section for the ruling (the plan's own guarantee 1 already allows a 32× amplification
at that stretch setting, and a CPU reproduction of the whole pipeline matches the GPU
number to within a code).

**Stage 1 + Stage 2 are both implemented in this pass** (the curve, the content anchor,
the scene-cut snap, AND the guided-filter base/detail split) — not staged across separate
commits the way §6 below sketches; see `shader-effects.md` for two DOCUMENTED
simplifications made to fit the existing pipeline without widening the shared history
texture or adding a fifth new shader file: the scene-cut histogram is 16 bins (grouping
the measure pass's existing 64), not the plan's own 64-column widening, and the guided
filter's two box passes are non-separable 2D boxes (one dispatch each) rather than the
plan's separable h/v pairs. Neither changes the underlying maths, only the dispatch
shape; both are cheap at the resolution the guided filter runs at (quarter of the base
layer).

**Fixed 2026-09-14 (V2 QC), superseding the paragraph this one replaces:** §4.3's
worked-numbers table (`g = 0.566, t = 0.043`) did not match evaluating
`f(x;g,S) = x·((1+t)/(x+t))^(1−g)` at that `g` and `S = 4` — the table's own `t = 0.043`
was correct (verified: `t = 1/(S^(1/(1−g))−1) ≈ 0.0428`, and `f'(0) = ((1+t)/t)^(1−g) =
4.0` exactly, both algebraically and numerically), but the table's OTHER entries (code 4
→ 15, code 128 → 158, etc.) did not follow from that `t`/`g`/formula on a direct
evaluation (a from-scratch check gives 14/170 instead). The closed-form property this
plan actually needs — `f'(0) = S` exactly, `f` concave, bounded slope everywhere — holds
and is what `tests/test_effects_curve.cpp` pins; the table below has been recomputed
directly from the formula with a short Python check (`t_of(g,S) =
1/(S**(1/(1-g))-1)`, `f(x,g,S,t) = x*((1+t)/(x+t))**(1-g)`) and matches the QC commit's
own CPU reproduction exactly. **The original rows were a prototype artefact** — a
rounding or transcription slip in the numpy script that produced them, not a defect in
the formula itself, which the shipped code has always followed. No code changed; only
the table below did.

**2026-09-14, design plan. Nothing under `src/`, `tests/` or `scripts/` was changed for
this page.** The user's brief, verbatim: *"create a plan for a really good adaptive
brightness shader. He should be creative and search the web for state of the art
technology on achieving this. It should prioritize visual clarity in the sense of being
able to see enemies in pvp games."*

This page is meant to be actionable by a worker without re-researching: every formula,
parameter, pass, texture, test and pass criterion is stated. The prototype used for the
numbers below is a ~120-line numpy script run on the user's own capture
(`/home/mo/Pictures/Screenshots/2026-09-14-053516_hyprshot.png`, CS2, a zombie-escape
map) and on synthetic scenes; the numbers are reproducible from the formulas in §4 and
should be re-measured on the GPU by the harness in §7 before anything is claimed.

Sibling pages: [`../features/shader-effects.md`](../features/shader-effects.md) is the
reference for what exists today (Adaptive Brightness, Adaptive Gamma, Brightness Map,
Bloom, the measure pass, the history texture); this page proposes what replaces the first
three. Where the two disagree, this page is the *plan* and that one is the *present*.

---

## 0. The one-paragraph answer

**Replace the two adaptive effects and the Brightness Map with one operator whose lift
has a bounded slope.** Every operator shipped so far lifts with `x^g`, and `x^g` for
`g < 1` has an *infinite* slope at zero — so on a frame that is 73 % literal black with a
median of 0.09 codes (the user's capture), the exponent pins to its floor and code 4
lands on 90, code 16 on 128: binarisation is *structural* to gamma-based lifting on a
near-black scene, not a tuning error, and the darkness-floor stopgap being added today
fixes it only by switching the effect off on exactly the scenes it exists for. v2's base
curve is a **toe-gamma** — `x^g` far from black, but bending into a straight line of
slope `S` at black — so the largest contrast amplification anywhere in the picture is the
user's own **Max lift** `S`, black stays black, and nothing can clip. On top of that
curve, in order of payoff: an anchor statistic that ignores literal black (the median of
the *void* is not the median of the *content*); a **static lift floor** so a dark player
on a bright world is lifted even when the scene statistics say "bright" (no global
statistic can find a 1 %-of-frame object — measured); temporal filtering on the one
scalar that adapts, with a **scene-cut detector that snaps instead of sliding** (safe
only now that the operator is bounded); and, as a measured Stage 2, an edge-aware
**base/detail split** (fast guided filter at ¼ resolution, ~1.5 % of frame height) so
lifted regions keep their Weber contrast — which is where a silhouette lives — instead
of the flattened texture a global curve leaves. Stage 3 adds a **silhouette band** (a
bounded, edge-aware mid-scale contrast term at 4..16 px) — the one thing in the
competitive ReShade/driver toolbox ("Clarity") that targets detection rather than
exposure. Expected cost at 1440p on a mid-range GPU: Stage 1 adds nothing measurable;
Stages 2–3 add ~0.2–0.35 ms, and the first deliverable is the GPU timestamp the
pre-pass has never had, so that estimate becomes a number.

---

## 1. What "see enemies" means here, and the line this plan draws

**The compositor sees pixels, and only pixels.** gamescope-ritz processes the game's
*presented* frame after the game is done with it, in the compositor's own process. It
never reads the game's memory, depth, entity list, or network traffic, and never injects
into the game. That is the same category as a monitor's "Black eQualizer", a GPU driver's
colour controls, or an OBS filter: it changes what the *display* shows, not what the
*game* knows. This plan stays entirely on that side of the line and says so explicitly:

- Anything that would need the game's depth buffer, motion vectors, entity positions or
  an outline drawn from a model ID is **out of scope, permanently**, not "later". An
  "enemy highlight" that knows where enemies *are* is a cheat; an operator that makes a
  dark shape on a dark wall readable is a display setting.
- Whether a *tournament* allows display-side processing is the tournament's rule, not a
  technical fact; the user should know that the injected-DLL class (ReShade, Freestyle
  in-process hooks) is what CS2's Trusted Mode and Riot's Vanguard refuse, and that plain
  overlay/compositor processing is not what those systems target — see §2.5.

"Visual clarity for seeing enemies" then decomposes into four measurable things
(§2.6): **local contrast at object scale** (a 20–40 px silhouette against its immediate
surround), **enough absolute luminance** for the eye's achromatic contrast sensitivity to
be near its peak (it rises steeply out of the near-black range), **colour contrast kept
or increased** (chroma survives lifting if the operator preserves ratios), and
**temporal stability** (a wandering exposure is an attention sink and a flicker at
8–15 Hz is worse than no effect).

---

## 2. Research: the state of the art, and what each thing teaches this operator

### 2.1 Local tone mapping proper (base/detail operators)

| Technique | What it does | Halo / artefact behaviour | Real-time cost | Verdict here |
| --- | --- | --- | --- | --- |
| **Durand & Dorsey 2002**, bilateral base/detail ([paper](https://people.csail.mit.edu/fredo/PUBLI/Siggraph2002/DurandBilateral.pdf)) | Bilateral-filter the log-luminance into a *base*; compress only the base by a scale factor in the log domain; add the *detail* back unchanged. The canonical "compress the base, keep the detail" | The bilateral's own failure: **gradient reversal / edge ringing** at smooth edges (the filter treats one side of a soft edge as an outlier) — the detail layer then carries a rim | The piecewise-linear/subsampled acceleration is what made it real-time; a bilateral grid is still the expensive way | The *structure* (base compressed, detail preserved multiplicatively) is adopted. The bilateral is not — see the guided filter |
| **Guided filter**, He et al. 2010/2013, and **Fast Guided Filter** 2015 ([arXiv 1505.00996](https://arxiv.org/abs/1505.00996)) | An edge-aware smoother that is a *local linear model* of the guide: `q = a·I + b` with `a = var/(var+ε)`, `b = (1−a)·mean`, coefficients box-filtered. Self-guided (`I = p`) it is an edge-preserving low-pass with **no gradient reversal by construction** (the output is locally a scaled copy of the input, slope `a ≤ 1`) | The known good citizen among edge-aware filters for base/detail: halos are bounded because a step edge with contrast² ≫ ε keeps `a ≈ 1`; below ε it is smoothed like a Gaussian. The fast variant subsamples by `s` (paper uses 4), runs the box filters at `1/s²` cost, and **upsamples only the coefficient maps** — the full-res guide restores the edges | Two separable box filters at ¼ res and one bilinear fetch per pixel: the cheapest edge-aware base there is | **Adopted** as the Stage 2 base. `ε` is the one knob, and it is the *contrast an object needs to count as its own region* — set from the player-vs-world contrasts measured on the capture |
| **Eilertsen, Mantiuk, Unger 2015**, real-time noise-aware video TMO ([pdf](https://www.cl.cam.ac.uk/~rkm38/pdfs/eilertsen2015rt_na_tmo.pdf), [project](https://computergraphics.on.liu.se/rntm/)) | The closest published design to what we need: base/detail with a purpose-built **isotropic diffusion filter with a Tukey edge-stop** (chosen over the bilateral precisely because the detail layer "is highly sensitive to filtering artefacts, where the behaviour along edges is extremely critical"); **local tone curves per ~5° tile** (≈230 px at 1080p), blended 10 % global / 90 % local, **whose slopes never exceed 1**; and temporal filtering applied to the *tone-curve nodes* with a 3-tap IIR at 0.5 Hz, with an optional temporal edge-stop for "extreme temporal variations" | Their central lesson: the spatial filter is deterministic per frame and cannot flicker; **only the statistics-driven curve can**, so that is the only thing to filter over time. Also: histogram-driven curves over-enhance large uniform regions unless the histogram is weighted by local contrast | 46.5 fps at 1080p on a GTX 980 for the *whole* operator (noise model, diffusion, curves) — an order of magnitude over our budget as published, but the pieces we take are the cheap ones | The temporal design (filter the statistics, not the pixels; edge-stop = scene cut) and the "slope ≤ 1 / bounded slope" principle are **adopted**. Per-tile curves are not: the base layer already carries locality, and a tile grid is the 16×16 mistake at a finer scale |
| **Exposure fusion / Mertens** for real-time rendering, Wronski 2022 ([post](https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/)) | Three synthetic exposures blended through Laplacian pyramids built from mip chains; "definitely under 1 ms" with most work at ¼ res | The author's own caveats: Gaussian blending "produces pretty bad halos"; bilateral blending "gradient reversals and edge ringing"; the result "depends on the image frequency content" so it needs per-scene tuning | Fits the budget | **Not adopted**: three exposures and a pyramid to get what a bounded curve on an edge-aware base gets in one pass, plus a content-dependent look the user could not reason about from a slider |
| **Local Laplacian filters** (Paris/Hasinoff/Kautz; Aubry's fast version) | Per-pixel remapping through a pyramid; the gold standard for halo-free detail manipulation | Halo-free, but O(levels × pyramid) per pixel; the fast version needs tens of remapped pyramids | Too expensive at 144 Hz for a sub-ms budget | Not adopted; the guided filter is the poor man's version at 1/50 the cost |
| **Reinhard local / "dodge and burn"** | Per-pixel adaptation from the largest Gaussian scale without an edge | The classic halo generator (this is the family the Brightness Map belonged to) | Cheap | Not adopted — its failure was measured on our own harness (+37.5 codes rim at the shipped default, §3.4) |
| **ACES / filmic S-curves** | Global curves with a toe and a shoulder | No halos (global), no locality | Free | The **toe** idea is adopted: v2's base curve is exactly "a gamma with a film toe", and the toe is what bounds the slope |

### 2.2 Histogram-based contrast (CLAHE) — and why not

CLAHE ([overview](https://www.emergentmind.com/topics/contrast-limited-adaptive-histogram-equalization-clahe),
[MathWorks HDL reference](https://www.mathworks.com/help/visionhdl/ug/contrast-adaptive-histogram-equalization.html))
equalises per tile with a clip limit and bilinearly blends the per-tile LUTs; the clip
limit is what stops it amplifying noise in a flat tile, and the blend is what hides tile
seams — mostly: "the clipping is carried out on each local region independently, which
can still lead to tile boundary artifacts". Two reasons it is the wrong tool for this job:
equalisation *maximises* slope where the histogram is dense (Eilertsen's Fig. 6: slope
"exponentially related to the probability values"), i.e. it spends the display's range
on the big flat wall, not on the small dark figure; and the per-tile LUT is the 16×16
grid again with 8×8 tiles, i.e. a 240-px neighbourhood that averages a player into its
wall. The one idea worth keeping is the **clip limit** as a guarantee: "no bin may be
amplified beyond X" is the histogram-side statement of v2's bounded slope.

### 2.3 Retinex and divide-by-blur — the Brightness Map's lineage

Single- and multi-scale Retinex divide the picture by a Gaussian low-pass of itself
(MSRCR adds colour restoration). The literature is unanimous that "halo artifacts occur
unnaturally in the boundary of regions with large gradient values" and that MSR's
multiple scales exist "in order to eliminate the visible halo artifacts near strong
edges" ([halo-free real-time Retinex design](https://www.researchgate.net/publication/255970426_Halo-Free_Design_for_Retinex_based_Real-Time_Video_Enhancement_System),
[NLHD survey intro](https://arxiv.org/pdf/2106.06971)). The Brightness Map was a
single-scale Retinex in the log-log domain with a target; its own measurements
(§3.4) are the textbook result. The fix the field converged on is the same in every
paper: **make the illumination estimate edge-aware** (bilateral → guided), which is
§2.1's conclusion from the other direction.

### 2.4 What monitors and GPU drivers ship for "see in the dark"

- **BenQ Black eQualizer, ASUS Shadow Boost, MSI Night Vision / Dark Boost** —
  all "selectively lift the gamma of dark and near-black pixel values without changing
  how bright areas render" ([ProSettings](https://prosettings.net/blog/what-is-black-equalizer/),
  [Evetech tested settings](https://evezone.evetech.co.za/performance-pulse/best-shadow-boost-settings-gaming-tested/)).
  Two facts from the tests matter: at moderate settings they raise "dark shadow detail
  by roughly 20 % to 30 % luminance while leaving bright areas largely untouched", and at
  high settings "the curve flattens the shadow region so aggressively that all dark areas
  approach a single grey level" — i.e. a shadow-only lift **must** compress the tones
  just above the knee (a monotone curve that lifts the bottom and pins the top has slope
  < 1 somewhere in between), and the flattening is that compression made visible. v2
  puts the compression at the *top* instead (film toe, §4.3) and offers the knee
  placement as an open question (§8). These are global per-pixel curves; none is local.
- **NVIDIA Freestyle "Details"** (Sharpen / Clarity / HDR toning / Bloom) and
  **"Brightness/Contrast"** (Exposure, Contrast, Highlights, Shadows, Gamma) — Clarity is
  a large-radius local-contrast term (the same operator as ReShade's Clarity, §2.5), the
  rest are global. **RTX Dynamic Vibrance** ([NVIDIA](https://www.nvidia.com/en-us/geforce/news/nvidia-app-beta-download/),
  [TweakTown](https://www.tweaktown.com/news/96343/rtx-hdr-and-dynamic-vibrance-use-ai-to-dramatically-improve-the-look-of-thousands-games/index.html))
  is an AI (tensor-core) saturation boost designed "to increase color separation while
  attempting to avoid excessive color crushing" — the interesting part is the *goal*:
  colour separation as a detection aid, not exposure. NVIDIA's own footnote is that
  filters "are not a guaranteed improvement to reaction time, visibility in every
  situation, or competitive performance".
- **AMD CAS / RIS** ([GPUOpen](https://gpuopen.com/fidelityfx-cas/)) — contrast-*adaptive*
  sharpening: sharpens less where local contrast is already high, and its lobe is
  limited by the local min/max ring so it "does not introduce ringing artifacts". This
  repo already uses its sibling RCAS for Pre-Sharpen. The design lesson transfers
  directly: **bound the boost by what the neighbourhood can absorb**, which is what v2's
  soft shoulder on positive detail does (§4.5).

### 2.5 What competitive players actually run, and the anti-cheat line

- The competitive ReShade stack is small and consistent across presets: **Clarity**
  (a large-radius blur subtracted from the image and blended back — Soft Light / Overlay
  / etc.; the AstrayFX version offers a bilateral blur "so it won't create halos around
  the edges of objects" — [Clarity.fx](https://github.com/BlueSkyDefender/AstrayFX/blob/master/Shaders/Clarity.fx)),
  **LumaSharpen**, **Levels** (black/white point), **FakeHDR** (a two-radius local
  contrast term marketed as HDR), **Vibrance/Colourfulness**, and **Curves/LiftGammaGain**.
  Every one of them is either a global curve or a *mid-scale local contrast* term. The
  pattern is the finding: **players use local contrast at ~10–50 px, not exposure**, to
  make models pop, and they accept mild halos from the unsharp-mask family to get it.
  v2's Stage 3 silhouette band is that term, edge-aware and bounded.
- **CS2 blocks ReShade** — "the game is refusing third-party injection by design"
  ([Steam guide](https://steamcommunity.com/sharedfiles/filedetails/?id=3662582248)),
  and Trusted Mode "blocks DLLs injected into the game; it has never targeted windows
  drawn over the game" ([overlay compatibility summary](https://backgrind.com/overlay-anticheat-checker/)).
  **Riot's Vanguard** treats any DLL injection as hostile and bans for ReShade. So for
  the user's own game the *only* lawful place to do any of this is the display side —
  exactly where gamescope-ritz sits. That is the practical reason this feature is worth
  building well rather than pointing the user at a ReShade preset.
- Riot's own design article ([VALORANT shaders and gameplay clarity](https://www.riotgames.com/en/news/valorant-shaders-and-gameplay-clarity))
  is worth reading for what a *game* does about visibility: no dynamic shadows in the
  playable space, distance-dependent brightening and outlines on characters, identical
  visual information at every quality setting. A compositor cannot do the second thing
  (it does not know what is a character), and must not try; it can do the analogue of
  the first — never let the scene's lighting hide a shape.

### 2.6 The perceptual side: what makes a low-contrast dark silhouette detectable

- **Contrast metric.** For a shape on a locally uniform surround the right measure is
  **Weber contrast** `(L_obj − L_bg) / L_bg`; Michelson is for gratings, RMS for textures
  ([Measuring contrast sensitivity](https://www.sciencedirect.com/science/article/pii/S0042698913001132)).
  So the operator's detection-relevant invariant is: **Weber contrast of a region against
  its surround must not decrease**, and the harness measures exactly that (§7).
- **Absolute level matters.** Achromatic contrast sensitivity "increases with higher
  background luminance up to 200 cd/m²" while chromatic sensitivity "does not show a
  significant sensitivity drop" ([spatio-chromatic CSF, mesopic/photopic](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7405764/);
  [visual search in low mesopic environments](https://link.springer.com/article/10.3758/s13414-018-1512-0)).
  Two consequences: lifting a near-black surround to a low-grey *increases the eye's
  sensitivity* to the same Weber contrast — which is why a lift helps at all — and
  **colour contrast is a second, independent channel** that survives darkness better
  than luminance contrast, so an operator that preserves chroma ratios (v2 scales RGB by
  `Y′/Y`) keeps a cue a per-channel exponent partly destroys.
- **Scale.** The CSF peaks around 2–5 cycles/degree; at a 24″ 1080p display at ~60 cm
  one degree is ~38 px, so the eye is most sensitive to structure with a period of
  8–20 px — a player silhouette at 1–3 % of frame height (11–32 px at 1080p) sits right
  on it. That is the scale the base filter (Stage 2) and the silhouette band (Stage 3)
  are tuned to, and why the 16×16 grid (a 565-px kernel) could never contribute.
- **Time.** Flicker sensitivity peaks at ~8–15 Hz; any statistic that wanders at those
  rates (the 2026-09-07 pulse) is *worse* for detection than a static picture, because
  a global luminance modulation captures attention and masks small changes. Hence:
  temporal filtering on the statistics only, slow when sliding, instant when the scene
  actually cuts (§4.8) — a single step is one transient, a slide is a second of noise.

### 2.7 How games and competitive guides handle dark maps

CS2's own controls are a **global** brightness slider and `r_fullscreen_gamma` (default
2.2; guides recommend 2.3–2.5 for dark maps and 80–100 % brightness — [bo3.gg](https://bo3.gg/articles/cs2-gamma-command-settings),
[skin.land](https://skin.land/blog/cs2-gamma-settings-guide/)) plus NVIDIA digital
vibrance at 60–80 % ([XPFEED](https://www.xp-feed.com/en/guides/config-grafica-cs2-fps-visibilidad-competitivo)).
All global: a gamma that reveals the corridor washes the lit room. The user's capture is
the extreme case those guides cannot handle — the map itself renders 73 % of the frame
at literal 0, which no gamma reaches. That is a limit v2 must state, not hide: **code 0
carries no information and stays 0**; the lift concentrates on codes 1..40, where the
darkest *information-bearing* pixels are, and the slope bound `S` is exactly "how hard
those codes may be pulled up" (code 3 → at most `3S`).

---

## 3. Diagnosis of the current operators against the capture

### 3.1 The frame, measured

`2026-09-14-053516_hyprshot.png`, raw, Rec.601 luma on encoded values, 1920×1080:

| statistic | value |
| --- | --- |
| pixels at code 0..1 | **73.5 %** |
| mean | 14.1 codes |
| median (p50) | **0.0** |
| p75 / p90 / p98 | 4.8 / 58.5 / 109.6 |
| the shader's own `p50` rank-window mean (25..75 %) | **0.089 codes = 0.00035 encoded** |
| the shader's `p98` window | 110.6 codes |
| left zombie (x 660..780, y 520..760) vs the floor beside it | **8.7 vs 33.4** codes — the player is *darker* than its background |
| pixels ≥ 128 | 1.1 % |

And the captured output (`…053519`, the effect on): 71 % still at 0..1, but **21 % of
pixels ≥ 128** (16 % in 128..192) versus 1.1 % raw, and 0.18 % ≥ 250 — the picture is two
populations, black and light grey, with the 4..60-code band that held every surface in
the raw frame emptied out (raw 4.3 % in 6..24; output 0.7 %). A numpy re-application of
`x^0.25` to the raw frame reproduces that output to within a percent (21.9 % ≥ 128), so
the capture is the exponent at its floor, as the dark-floor note in `effects_curve.h`
already concluded.

### 3.2 Why a gamma aimed at the median binarises a near-black scene — structurally

Both Adaptive Gamma and Adaptive Brightness Dynamic fit `g = ln(target)/ln(p50)`. With
`p50 = 0.00035`, `ln(p50) = −8`, `g` wants to be `0.087` and is clamped to the floor
`0.25` (`1/max_lift`). The clamp is honoured correctly — and is irrelevant, because the
damage is in the *shape* of `x^g`, not in `g`:

```
d/dx (x^g) = g · x^(g−1)  →  ∞ as x → 0   for every g < 1
```

Any power curve below 1 has **unbounded slope at black**. At `g = 0.25`:

| input code | 1 | 2 | 4 | 8 | 16 | 32 | 64 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `x^0.25`, codes | 64 | 76 | **90** | 107 | **128** | 152 | 181 |
| local slope (× amplification) | 63 | 38 | 22 | 13 | 8 | 4.7 | 2.8 |

Everything that was 1..16 codes is now 64..128: the whole 4-bit range of dark surfaces
is compressed into 64 codes *and* moved next to white, while 0 stays 0. That is the
binarised picture. Adaptive Brightness Dynamic is the same with a `×4` gain in front
(`(4x)^0.25`): 9.9 % of the frame clips to ≥ 250 (numpy, same frame) before the
shoulder catches it. **No setting of Max gain / Max lift fixes it** — lowering the lift
to `g = 0.5` moves the table to 16 / 23 / 32 / 45 / 64 / 90 / 128, still a 16× slope at
code 1 — and the floor `AB_DYN_GAMMA_MIN = 0.25` was chosen in exactly these words
("below a fourth-root lift a single code of near-black lands above 90"): the problem was
known at the floor and is merely smaller above it.

**The dark-floor stopgap** (`u_darkFloor`, `dark_weight()` — fade both effects toward
the identity when the smoothed median is below ~0.03) restores the raw picture on this
frame. It is the right emergency brake and the wrong design: the scene it switches off
on is the scene a "see enemies in the dark" feature exists for, and on the scenes just
above its ramp (the `texdark` stand-in, p50 = 0.078) the exponent still has slope 8 at
code 16. v2 removes it (§8) because a bounded-slope curve makes it unnecessary.

### 3.3 Why the median is the wrong anchor on this frame

73 % of the frame is the *void*: unlit geometry, the map's own black. The median is a
statistic of that void, and it says "this scene is at 0.00035", which no curve can act
on. The content — floor, pipes, players, the lamp — is the other 27 %, whose own median
is ~40 codes (the numpy prototype's "content median of the base above the black floor":
0.156). An anchor computed over **pixels above a black floor** (§4.4) describes the
picture the operator can actually change; on this frame it asks for `g ≈ 0.57` at
Target 0.35 rather than the floor, and the picture is lifted, not binarised, before any
other part of v2 is even involved.

### 3.4 Why the 16×16 local grid cannot resolve a player, and why the Brightness Map haloed

Both are documented in `shader-effects.md` and the numbers are only summarised:

- **The grid.** 16×16 cells blurred to σ ≈ 4.7 cells is a ~565 × 317 px kernel at 1080p,
  chosen *because* a narrower one rang (+57 codes around a 320-px box at σ 1.4 cells,
  +15 at 4.7). It is "deliberately incapable of resolving an object's outline". A 30-px
  player is averaged into its wall and gets the wall's correction. Fine for
  half-sky/half-ground; useless for the request.
- **The Brightness Map** divided the picture by a *Gaussian* low-pass of itself at
  σ 4..96 px. A Gaussian is not edge-aware: at a dark object on a bright field the map
  straddles the edge, the object's side of the map is too bright (so the object is
  under-lifted) and the field's side too dark (so a rim of the field is over-lifted).
  Measured on `halobox`: **+37.5 codes** rim at the shipped Strength 0.5, scaling
  linearly with Strength, and the rim's *amplitude* barely changed with Radius (+21.6 at
  0.00, +38.6 at 2.00) while its *width* went 16 → 256 px — at Radius 2 "the object it
  surrounds is no longer lifted at all". Add its second failure — the target
  *flattens* (`x = map ⇒ out = target`, so a fully-strength map is a grey field) — and
  the user's removal request is the right call. The lessons carried forward: the
  illumination estimate must be edge-aware (§2.1, §2.3); "divide by the map" must
  become "compress the base, keep the detail"; and a spatial operator's halo must be
  gated on the harness, not on taste (§7).

### 3.5 Why no global statistic can fix "dark player on a bright world" — measured

The prototype run on a synthetic bright world (field 200 ± 10, one 48-px object at
30 ± 6) with the anchor-driven `g`: the content median is the field, `g` clamps to
**1.0**, and the object is returned at **30.0 → 30.0**. That is the AG complaint
reproduced by construction: a scene-level statistic says "bright, do nothing", and the
1 %-of-frame object is invisible to any percentile. The only operator that lifts it is
one whose *curve* lifts dark bases regardless of the scene — a **static lift** — and the
locality comes from the fact that the curve is applied to each pixel's own (base) level.
v2 therefore has a static lift floor (`Lift`) that adaptation can *deepen* on a dark
scene but never *remove* on a bright one (§4.4). With `g = 0.55, S = 4` the same object
reads **30 → 66** while the field reads **200 → 221** and the rim 4 px outside the object
is **+1.2 codes** over the far field (versus the Brightness Map's +37.5).

### 3.6 What the EMA time constants do to doors and flashes

Both effects smooth every statistic with `tau = 1 s` in each direction, i.e. ~3 s to
settle. Walk from a lit room into a dark corridor and the lift arrives over three
seconds; get flashed and the lift that suited the corridor is still applied to a white
frame for the first second — with `x^0.25` that is a *fully* blown frame, which is why
the constants could never be made short: a fast EMA on an unbounded operator slams.
Under v2 the operator is bounded (§4.3), so a snap is safe, and the plan is the
Eilertsen arrangement: an EMA for drift, and a **scene-cut detector** that snaps the
anchor in one frame when the histogram actually changed (§4.8). Time constants become
"how fast may it follow a gradual change", and a flash or a door is a cut, not a slide.

---

## 4. The v2 operator

### 4.1 Five guarantees, stated first

Everything below is designed so these hold at every setting, and each has a test (§7):

1. **Bounded amplification.** No local contrast anywhere in the frame is amplified by
   more than the user's own **Max lift** `S` (× **Detail**, Stage 2). A step of `d` codes
   becomes at most `S·d` codes. This is the anti-binarisation guarantee.
2. **Black stays black, white stays white.** Base curve `f(0) = 0`, `f(1) = 1`, and the
   output is in `[0, 1]` without a final clamp doing any work.
3. **Monotone, no inversion.** `f` is strictly increasing; the detail term is scaled by
   a positive factor; a brighter input pixel is never darker than its darker neighbour
   after the operator.
4. **Halo-bounded.** On the `halobox`/`haloinv`/`models` scenes the field 4 px outside an
   object differs from the far field by ≤ 4 codes at the defaults and ≤ 8 at any setting.
5. **Still frame ⇒ still output.** Peak-to-peak zero on a static scene; a scene cut
   settles in ≤ 2 frames; a gradual change follows the EMA with no overshoot.

### 4.2 Overview

```
layer0.tex (game, source res, encoded)            [Stage 1: only ← measure → apply]
   │
   ├─► cs_effects_v2_down      Y4 = 4×4 box mean of graded luma          (¼ res, R in RGBA8)
   │       │
   │       ├─► cs_effects_measure   histogram of Y4 above the black floor → anchor,
   │       │                        scene-cut test, EMA → history row 0   (one workgroup)
   │       │
   │       └─► cs_effects_v2_box1h/v   box(Y4), box(Y4²) → a = var/(var+ε), b = (1−a)·mean
   │           cs_effects_v2_box2h/v   box(a), box(b)                       (¼ res, RG16 packed in RGBA8)
   ▼
cs_effects_layer0  per pixel:  B = ā·Y + b̄  (bilinear ā,b̄ at pos/4)   [Stage 1: B = Y]
                               f(B) toe-gamma with slope cap S, g from history
                               Y′ = f(B) + (Y − B)·secant, soft-shouldered above
                               RGB′ = RGB · Y′/Y, hue-preserving compress
```

Stage 1 is the bottom box alone with `B = Y` (a per-pixel curve) plus the new anchor and
scene-cut in the measure pass; Stage 2 adds the ¼-res passes; Stage 3 adds a second,
finer coefficient pair for the silhouette band.

### 4.3 The base curve: a toe-gamma with a bounded slope

```
f(x; g, S) = x · ((1 + t) / (x + t))^(1 − g)        with   t = 1 / (S^(1/(1−g)) − 1)
```

for `0 < g < 1`; `f(x; 1, S) = x` (the identity). Properties, all closed-form:

- `f(0) = 0`, `f(1) = 1`.
- `f′(0) = ((1 + t)/t)^(1−g) = S` exactly — `t` is *defined* by the slope cap.
- `f` is concave on `[0, 1]`, so `f′` is decreasing: **the slope is at most `S`
  everywhere** and, since `f(0) = 0`, the secant `f(x)/x ≤ S` everywhere too.
- For `x ≫ t` it is `x^g` up to a constant: the same "gamma" the user already knows.
- `f′(1) = (g + t)/(1 + t) ≈ g`: the highlights are compressed by about `g`, never
  clipped — this is the **highlight guard**, and it is the price of lifting with a
  monotone curve that pins white (§2.4).

Worked values (codes in, codes out), `S = 4`:

| `g` | `t` | 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 200 | max slope |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0.566 (Target 0.35 on the capture) | 0.043 | 4 | 7 | 14 | 25 | 43 | 71 | 111 | 170 | 221 | 4.0 |
| 0.374 (Target 0.5) | 0.123 | 4 | 8 | 15 | 28 | 49 | 82 | 127 | 185 | 229 | 4.0 |
| 0.25 (the old floor, for comparison) | 0.187 | 4 | 8 | 15 | 28 | 52 | 87 | 135 | 192 | 232 | 4.0 |
| `x^0.25` today | — | 64 | 76 | 90 | 107 | 128 | 152 | 181 | 215 | 240 | ∞ |

The last two rows have the same exponent; the difference is the toe. Code 4 lands on
14..15, not 90, and codes 1..16 keep close to their 4-bit separation ×4 instead of being
folded into 64..128.

`Why this family and not a piecewise linear+gamma:` a tangent from the origin to a
concave curve does not exist (the join cannot be C¹), and a spline needs three or four
numbers where this needs two and is analytic — `t` is one `pow` per frame, `f` is one
`pow` per pixel, the same cost as today. `Why not a "shadows only" bump with a knee:`
a bump that returns to the identity above a knee must have slope `< 1` in between and
above `A ≈ 3` inverts — the monitor tests' "all dark areas approach a single grey level"
is that compression; the toe-gamma spreads the same compression over the whole upper
range where it is least visible. A knee variant remains an open question (§8).

### 4.4 Choosing `g`: a static floor, deepened by adaptation

```
g_static  = 1 − 0.6 · Lift                                  // Lift 0..1 → 1.0 .. 0.4
g_adapt   = ln(Target) / ln(anchor_smoothed)                // < 1 when the content is darker than Target
g         = clamp( min(g_static, g_adapt), G_MIN, 1 )       // Adaptation "Scene"
g         = clamp( g_static, G_MIN, 1 )                     // Adaptation "Off"
G_MIN     = 0.2
```

- **`anchor`** = the rank-window mean over ranks 25..75 % of the **base** texels whose
  value is above the black floor `BLACK = 2/255` (Stage 1: over the 128×128 graded-luma
  taps the measure pass already takes, with the same exclusion). The histogram already
  exists; the exclusion is one `if` before the two `atomicAdd`s plus a count of the
  excluded taps so the ranks are over the content. If fewer than 1 % of the taps are
  above the floor, `anchor` holds its previous value (an all-black frame — a loading
  screen — must not swing the adaptation).
- `Why min(g_static, g_adapt):` §3.5. Adaptation can only make the lift *stronger* on a
  dark scene; on a bright scene the static curve is what lifts the one dark thing in it,
  and the static curve's own shape leaves bright bases nearly alone (`f(0.78) = 0.87` at
  `g = 0.57`).
- `Why G_MIN = 0.2 and not 0.25:` with the toe, small `g` is safe (slope is still `S`);
  the floor exists only so `t` stays finite and the highlight compression `≈ g` does
  not go below a fifth. It is not a user-facing bound and needs no readout code.
- **Adaptation "Off"** is a legitimate, fully static shadow-lift mode with zero temporal
  behaviour — the mode a competitive player who hates *any* exposure movement should
  use, and the mode the harness measures halos and contrast in.

### 4.5 Base/detail (Stage 2): the fast guided filter and a secant detail term

**The base**, self-guided fast guided filter at ¼ resolution ([§2.1](#21-local-tone-mapping-proper-basedetail-operators)):

```
Y4      = box4×4(Y)                                   // the down pass; Y is the graded luma
mean    = box_r(Y4);   corr = box_r(Y4²)
var     = corr − mean²
a       = var / (var + ε);   b = (1 − a) · mean
ā, b̄   = box_r(a), box_r(b)                          // second box pass, same radius
B(p)    = ā(p/4) · Y(p) + b̄(p/4)                       // per pixel, ā/b̄ sampled bilinearly
```

- `r` (texels at ¼ res) `= round(Scale % · H / 4)`; **Scale** default 1.5 % → `r = 4`
  at 1080p (16 px), `r = 5` at 1440p. Bounded to 2..12 texels.
- `ε = EDGE²` with **`EDGE = 0.06`** encoded (≈ 15 codes). `Why 0.06:` the guided
  filter keeps an edge whose contrast² is large against ε and smooths texture whose
  variance is small against it. The capture's player-vs-floor contrast is 0.095 (25
  codes) and the pipes' texture std is 0.12 — so 0.06 preserves the player's outline as
  its own region (its var across the edge ≈ 0.0023 vs ε 0.0036 keeps `a ≈ 0.4`, half
  preserved; a 40-code silhouette is fully preserved) while smoothing sub-15-code
  texture into the detail layer where the secant boosts it. It is a constant, not a
  param: it trades against Detail and Scale, and three interacting spatial knobs is
  what made the Brightness Map untunable.
- `Why self-guided luma and not RGB:` the base is an illumination estimate, one channel;
  chroma is restored by ratio (§4.7). `Why encoded and not log:` the whole pre-pass is
  encoded on purpose (shader-effects.md, "Encoded space, on purpose"), an encoded value
  is already perceptually spaced, and the guided filter is a linear model — it does not
  care which monotone space it runs in, only that ε is expressed in it.

**The detail term**, multiplicative with a soft shoulder:

```
D    = Y − B
sec  = min( f(B) / B, S ) · Detail                 // the secant of the base curve: Weber-preserving
u    = D · sec
Y′   = f(B) + u                                    for D ≤ 0     (≥ 0 since sec ≤ f(B)/B)
Y′   = f(B) + u · h / (h + u),  h = 1 − f(B)        for D > 0     (< 1 for any u; slope 1 at u = 0)
```

- `Why the secant and not the tangent:` for a concave `f` with `f(0) = 0`, the secant
  `f(B)/B` exceeds the tangent `f′(B)` — at `B = 0.1, g = 0.37, S = 4` it is 2.75 vs 1.98.
  A per-pixel curve scales a region's texture by the *tangent* and flattens it; the
  base/detail form scales it by the *secant* instead. Measured (synthetic bright world,
  textured object): field texture std **10.0 → 9.3** with the split vs **10.0 → 6.4**
  with the global curve at the same `g`; the object's Weber contrast 0.200 → 0.154
  (split) vs 0.139 (global), and **0.194 at Detail 1.5**.
- `Why a soft shoulder only on positive detail:` the range guarantee. Negative detail
  cannot go below 0 because `sec ≤ f(B)/B`; positive detail could exceed 1, and the
  Reinhard-shaped `u·h/(h+u)` maps any `u` into the headroom `h` with unit slope at 0
  (invisible where nothing would clip) — the same shape Dynamic's shoulder uses, applied
  per pixel to the detail alone rather than to the whole curve. A hard `min(sec,
  (1−f(B))/(1−B))` was tried first and capped positive detail at 0.84× across every
  lifted region (measured), i.e. it flattened exactly what the split exists to keep.
- **Where "exactly Weber-preserving" holds and where it doesn't (2026-09-14, V2 QC).**
  Exactly, for **negative** detail (`D/B` in, `u/f(B) = D/B` out — the shoulder never
  runs there) and for **Clarity's own contribution against Stage 2** (`abv2_clarity_combine`
  is a plain sum before the one shoulder, so `M` inherits whatever Stage 2 already
  guarantees rather than adding a second approximation). For **positive** detail, the
  Reinhard shoulder is a *bounded* approximation, not an exact one: it lowers the
  region's Weber contrast versus the raw, unshouldered secant by a factor
  `h/(h + u)` (0.87 in the `B = 0.1, g = 0.37, S = 4`-style 10-on-15 example above) while
  the *absolute* step still grows roughly **2.7×** over raw at the same setting — the
  shoulder trades a little contrast for the range guarantee (§4.6's void aside, nothing
  may exceed 1), and that trade is the honest content of guarantee 4, not a defect.
  §7.2's `v2-silhouette` check is written to this: it asserts Weber contrast ≥ raw minus
  a **0.02** tolerance, not `≥ raw` unconditionally, precisely because the shoulder can
  cost a couple of hundredths of Weber contrast on a positive-detail figure while still
  making it far more visible in absolute terms.
- `Detail` above 1 breaks guarantee 1's factor to `S · Detail`; that is why its range
  stops at 2 and the guarantee is stated with the product.

### 4.6 Black floor and the void

`B ≤ BLACK (2/255)` ⇒ `Y′ = Y` (the toe already keeps these at ≤ `S·2` codes; the hard
floor makes literal black, letterbox bars, the map's own void and 1-code dither
*exactly* untouched, so a noise floor never becomes a visible grey). Constant, not a
param — it is 2 codes because that is below every game's dither amplitude and above
nothing a player could see.

### 4.7 Colour

```
k     = Y′ / max(Y, 1e−4)                        // Y′ ≥ 0, Y > 0 ⇒ k ≥ 0
RGB′  = RGB · k
if max(RGB′) > 1:  RGB′ = mix(RGB′, vec3(Y′), (max(RGB′) − 1) / (max(RGB′) − Y′))   // hue-preserving
```

Chroma ratios are preserved: a lifted dark red is a brighter red, not a pink. This is
the one place v2 departs from the pre-pass's per-channel convention (Shadow Control,
Saturation, AG and the Brightness Map are all per channel) — `Why:` per-channel
exponents desaturate lifted shadows (the exponent pulls each channel toward the same
value), and colour is the detection channel that survives darkness best (§2.6).
Saturation/Vibrancy still run before this in `grade()` and are unaffected. **Shadow
chroma** (a small saturation boost only where `f(B)/B` is large) is a Stage 3 candidate,
not a Stage 1 param.

### 4.8 Temporal: statistics only, asymmetric, with a scene cut

Only **one scalar** adapts: `anchor`. The base layer is spatial and deterministic per
frame (it cannot flicker — Eilertsen's observation, §2.1), `g_static` is a setting, and
the curve's parameters are derived from `anchor` on the fly. So the temporal design is:

```
raw       = this frame's content-median anchor
cut       = |ln(raw / smoothed)| > ln 2                       // a full stop
            && L1(hist_now, hist_prev) > 0.35                 // ≥ 35 % of taps changed bin
smoothed′ = cut ? raw                                         // snap
                : mix(smoothed, raw, ema_alpha(dt, raw > smoothed ? tau_up : tau_down))
tau_up    = Adapt speed         // scene got brighter → lift comes OFF at this rate
tau_down  = Adapt speed × 2     // scene got darker   → lift comes ON at this rate
```

- `Why snap on a cut:` the operator is bounded, so an instant change of `g` produces one
  bounded transient, whereas sliding produces a second of visible exposure motion (§2.6)
  — and a flash is *exactly* a cut. Both tests must agree so a bimodal scene under a pan
  (the 2026-09-07 pulse scenario) cannot trigger it: the pan moves the rank-window mean
  by a few codes and the histogram L1 by a few percent; a door or a flash moves both by
  a lot. `hist_prev` is 64 texels in a new history row.
- `Why asymmetric, and why brighten-off is the faster one:` the picture blowing out is
  the failure with a cost (you cannot see anything), the picture being briefly under-
  lifted is not; and the eye's own light adaptation is faster than its dark adaptation.
  One **Adapt speed** param with a fixed 1:2 ratio, not two — the two-slider pair was
  needed when the constants had to hide slams; a bounded operator needs one number.
- `dt` handling (wall time, clamped 0.25 s), reset-on-resume, and the history-texture
  contract are kept exactly as documented in `shader-effects.md`.

### 4.9 Stage 3: the silhouette band, and the two other extras

- **Silhouette band** ("Clarity", §2.5, edge-aware and bounded). A second guided
  coefficient pair at `r₂ = r/4` (≈ 4 px at 1080p) gives a finer base `B₂`; the band
  `M = B₂ − B` is the 4..16-px structure — the scale of limbs, heads and weapon outlines
  against a wall — and it is added back with gain `Clarity` (0..1 → ×1..×2), scaled by
  the same secant and shoulder as `D`. Edge-awareness is what keeps this from being an
  unsharp mask: a hard, already-visible edge has `a ≈ 1` in *both* filters, so `M ≈ 0`
  there and no rim is added; only low-contrast mid-scale structure (below `EDGE`) is
  boosted. Guarantee 1 becomes `S · Detail · (1 + Clarity)`; guarantee 4 is re-measured.
- **Shadow chroma**: `sat′ = sat · (1 + ShadowChroma · (sec − 1)/(S − 1))` — more colour
  where more lift was applied; off by default.
- **Preview strip**: `EffectPreview.cpp` re-runs the shared header on the 256×144 copy;
  v2's curve functions live in `effects_curve.h` as today, and the guided filter at
  64×36 is ~2 k texels of box filtering on the CPU — cheap enough to keep the strip
  honest. Stage 2 deliverable.

### 4.10 Parameters (≤ `kParamBudget` 8; seven are used, one in reserve for Clarity)

| Param (label) | key | range / default | what it does for seeing an enemy |
| --- | --- | --- | --- |
| **Adaptation** (Choice) | `mode` | `off` \| `scene`, default `scene` | `off` is a purely static shadow lift with zero temporal behaviour; `scene` lets a dark scene deepen the lift toward Target |
| **Lift** | `lift` | 0..1, default 0.5 (`g_static` 0.7) | The lift that is *always* there: how much a dark shape on a bright world is raised. The one control that fixes "player models turn almost black" |
| **Target brightness** | `target_luminance` | 0.1..0.9, default 0.35 | Where the *content* median (black excluded) is put on a dark scene. Lower than the old 0.5 because the anchor is now the content, not the void, and 0.5 reads milky on a corridor |
| **Max lift** | `max_lift` | 1..8, default 4 | The slope cap `S`: the hardest any dark step may be amplified. 1 = "do not lift at all"; 8 makes 3-code shapes readable at the cost of visible dither. **The anti-binarisation guarantee is this number** |
| **Detail** | `detail` | 0..2, default 1.0 (Stage 2; hidden until then) | Texture and outline contrast inside lifted regions relative to Weber-preserving: 1 keeps it, > 1 boosts, 0 flattens |
| **Scale** | `scale` | 0.5..4 % of frame height, default 1.5 (Stage 2) | The size of "a region": what counts as an object's own level vs its surround. ~ player size at typical engagement distance |
| **Adapt speed** | `adapt_speed` | 0.1..5 s, default 0.5 s (lift-off); lift-on is 2× | How fast a *gradual* change is followed. Cuts snap regardless |
| **Clarity** | `clarity` | 0..1, default 0 (Stage 3) | Mid-scale (4..16 px) contrast at silhouette scale, edge-aware, bounded |

Dropped from the old rows, and why: **Strength** (Lift is the strength; a dry/wet mix
over a bounded operator is a second, weaker Lift); **Min gain / Max darken** (v2 never
darkens — a bright scene gets `g = 1`, the identity, and highlight compression is
`≈ g`, bounded by Lift itself); **Local adaptation** (the base layer *is* the locality);
**Adapt to brighter / darker** as two sliders (§4.8); **Mode Whole image / Dynamic**
(there is one curve).

### 4.11 The binding readout

`v2_binding()` in the shared header, wording on the C++ side, same pattern as
`ab_dyn_binding()`: `NONE` (the content median is on Target), `LIFT_FLOOR` (Lift's own
`g_static` is what limits — Target would want less lift, i.e. the static floor is the
stronger of the two), `G_MIN`, `MAX_LIFT` (Target is reachable only through a steeper
toe than Max lift allows: the toe is doing the work and raising Target changes only the
upper range), `VOID` (fewer than 1 % of taps above the black floor — the anchor is
holding its last value), `CUT` (a scene cut snapped this frame; informational).

### 4.12 Darkening (2026-09-14 addendum) — the two-sided curve

The user's own request, verbatim: *"Make it able to make the image darker (both full and
on parts of the image)"*. Everything in §4.3–4.11 above describes ONE direction only
(`f(x) >= x`, concave, `S` the lift's own slope cap) — this addendum is the mirror,
shipped the same day, and it answers §4.3's own open question — *"A knee variant remains
an open question (§8)"* — for the DARKEN direction specifically: **toe, unconditionally**;
no darkening-knee construction was built (see below for why, and why that is the honest
answer rather than a deferral).

**The closed form is exact, not approximate.** `abv2_toe(x; g, t)`'s general derivative
has `f''(x)` sharing the sign of `(g - 1)` at every `x` — concave (lift) for `g < 1`,
CONVEX for `g > 1`, one sign flip, same family. Writing the convex branch with a positive
exponent gives `f_dark(x; g, t) = x * ((x+t)/(1+t))^(g-1)`, solved so `f_dark'(0) = 1/D`
exactly (`abv2_solve_t_dark()`, literally `abv2_solve_t()`'s own derivation with the
target reciprocated). Convexity plus `f(0)=0, f(1)=1` gives `f(x) <= x` by Jensen's
inequality directly; the tangent-line inequality at the origin gives the secant
`f(x)/x` non-decreasing, so its floor over `(0,1]` is its own limit at `x -> 0`, which is
`1/D` by construction — the EXACT mirror of the toe's own `S` proof.

**The pivot (local, per-pixel, in one frame).** The per-pixel base `B` already varies
spatially; making its curve two-sided — `F(x) = L(x)` for `x <= Target`, a rescaled
`f_dark` above it — is what lets a bright region darken while a dark region lifts, in the
same frame, without a second global statistic or a second EMA. The rescale composes on
`L(x)`'s own continuation (not on raw `x`), which is what makes the WHOLE curve telescope
back to `L(x)` exactly at Max darken 1 — the byte-identical guarantee that keeps §4.3's
lift half completely untouched, algebraically, not merely by measurement.

**Why toe-only for darken, and why that answers the open question honestly.** A
darkening-knee mirror (leave shadows exactly untouched, compress only the mid-tones just
above the pivot) is structurally a different curve family than the toe mirror above — it
would need its own rescale-into-a-box construction the way §4.3's knee variant does for
lift, doubled for the opposite direction. It was not built: the plan's own escape hatch
for exactly this case (*"if that is genuinely awkward, make Knee use the toe-mirror for
darkening and say so"*) is what shipped, and this note is the "say so". `Detail`'s secant
bound widens to `max(S, D) * Detail` (a SAFE, not tight, bound — the composite mixes the
lift curve's own continuation with the darken mirror's rescale, so no single closed form
bounds it as tightly as `S` alone bounds the toe's own secant). Two new Params, **Max
darken** (1..4, default 1 = off) and **Darken** (0..1, default 0), took the row's own
`kParamBudget` from 8 to 10 the same day Stage 3 (Clarity) had just spent the last of the
old 8 — see `superdoc/features/shader-effects.md`'s own "Darkening" section for the
row-budget reasoning, the measured tables, and the harness checks
(`abv2-darken-bright`/`abv2-darken-sky`).

### 4.13 Darkening REDESIGNED (2026-09-15) — a true S-curve fixed at Target

§4.12's own pivot — composing the darken mirror on `L(x)`'s CONTINUATION above Target,
at height `L(Target)` rather than `Target` itself — was QC'd the day after it shipped and
found to be the wrong pivot, not merely an approximation of the right one. Two
consequences, both measured: `F(x) >= x` for EVERY `x` above Target (so "darkening" only
ever undid the lift curve's own highlight raise — a sky band read 225 raw, 233.7
lift-only, 225.0 with Darken 0.5 / Max darken 2, i.e. back to raw, never below it), and
`L(Target)` (code 122 against Target's own code 89 at the shipped defaults) was a FLOOR
nothing above Target could be darkened past, whatever `D` said. §4.12 is left in place
above as the historical record of what shipped first and why; this section supersedes its
pivot construction, not its closed form (`f_dark` itself — the convex mirror, the `1/D`
secant proof — is untouched, verified byte-for-byte).

**The fix: rescale BOTH halves against Target itself, not against `L`'s continuation.**

```
x <= Target:  F(x) = Target · L(x / Target)
x >  Target:  F(x) = Target + (1 - Target) · K( (x - Target) / (1 - Target) ; gDark, D )
```

`L(1) = 1` by construction, so `F(Target) = Target` EXACTLY — the fixed point the plan's
own §4.3 prose ("`f(Target) = Target` literally") asked for, closed rather than merely
approached. `K(0) = 0` for any `g, D`, so the two branches agree at `x = Target` too:
C0 everywhere, not merely at the defaults. And now, by Jensen's inequality on `K`
(convex, matching endpoints) composed with the SAME rescale, `F(x) <= x` for every `x`
above Target — the guarantee §4.12's own pivot could not make, closed this time by
construction rather than left as a documented gap. `F(x) >= x/D` still holds (the
non-decreasing-secant argument, unchanged). Below Target, `F(x)/x <= S` is inherited
through the rescale unchanged (`L(z) <= S·z` for `z` in `[0,1]` gives
`Target·L(z) <= Target·S·z = S·x`).

**The price: an explicit off switch, not a documented pivot gap.** Rescaling the LIFT half
too is unavoidable once the fixed point must be Target exactly (§4.12's own gap analysis
already said as much: "matching Target exactly would require RESCALING the lift half into
`[0, Target]` too, which would change its values for every `x < Target`"). So the
byte-identical-at-Max-darken-1 guarantee that keeps the shadows untouched when darkening
is off can no longer come from an algebraic telescoping that holds at every `x` regardless
of the dispatch — it comes from an explicit `if (D <= 1) return abv2_curve(...)` branch
instead, gating on **Max darken alone** (a strictly stronger guarantee than the stated
"Max darken 1 AND Darken 0", since `abv2_toe_dark()`'s own `D <= 1` identity guard already
made Darken irrelevant whenever Max darken is at its floor).

**Scene adaptation's aim, re-derived on the rescaled variable.** §4.4's `g_adapt =
ln(Target)/ln(anchor)` (reused for the darken side too, per §4.12) solves "raw anchor to
the `g`-th power lands on Target" — correct reasoning when the curve's own domain IS
`[0, 1]`, wrong once each half's domain is a rescaled sub-interval: fed straight into the
rescaled lift curve, a Target-0.35 / anchor-0.05 scene's old `g = 0.350` gives
`F(anchor) ≈ 0.177`, not the ≈0.35 the old equation was solving for — the QC's "does not
land the anchor near Target", reproduced on this half too, not only the darken side. The
fix solves the SAME kind of equation on each half's OWN rescaled coordinate instead —
`z = anchor/Target` for the lift side, `w = (1-anchor)/(1-Target)` for the darken side —
chosen so the boundary limits are the ones adaptation actually needs (defer to the static
floor exactly at the pivot, saturate at the internal ceiling/floor at the far edge). See
`src/shaders/effects_curve.h`'s own `abv2_g_adapt_lift_z()` / `abv2_g_adapt_dark_z()` for
the closed forms and worked numbers, and `superdoc/features/shader-effects.md`'s
Darkening section for the measured kink/quantisation numbers this redesign carries.

**What did NOT change:** the closed-form `f_dark` family, its `1/D` secant proof, the
static floors (`g_static`/`g_static_dark`), the EMA/scene-cut/attack-direction design
(§4.8, §4.12's own "attack" note), the Knee-mirror decision (still toe-only for darken,
§4.12's own answer stands), and the Detail secant's own clamp expression (though the bound
it ACTUALLY holds to is now tighter — `S · Detail` on both sides of Target, not
`max(S, D) · Detail` — since `F(x) <= x` above Target now holds unconditionally).

---

## 5. Cost and pipeline

### 5.1 Passes and textures

| pass | resolution | reads | writes | notes |
| --- | --- | --- | --- | --- |
| `cs_effects_v2_down` | ¼ × ¼ | layer0 through `grade()` (every source pixel, 4×4 box, as Bloom's down pass does and for the same shimmer reason) | `v2A.r` = Y4 (16-bit packed in two RGBA8 lanes, as the Brightness Map did) | Stage 2 |
| `cs_effects_v2_box1h` / `v` | ¼ | `v2A` | `v2B` = (mean, corr) H, then `v2A` = (a, b) after V | separable box, radius `r`, edge-clamped; the V pass computes `a, b` in its epilogue |
| `cs_effects_v2_box2h` / `v` | ¼ | `v2A` | `v2B` then `v2A` = (ā, b̄) | same kernel |
| `cs_effects_measure` | 1 workgroup | Stage 1: 128×128 taps of layer0 (as now); Stage 2: 128×128 taps of `v2A`'s Y4 | history row 0 (+ `hist_prev` row) | black-floor exclusion, content-median anchor, scene cut, one EMA |
| `cs_effects_layer0` | full | layer0 (+ RCAS taps), `v2A` bilinear ×4, history | `effectsOutput` | curve + detail + colour |

- **Formats.** Keep everything in `ABGR8888`, the one mandatory storage format and the
  one `descriptor_set.h`'s `dst` is declared as; `(ā, b̄)` are two 16-bit UNORM values
  packed into the four 8-bit lanes exactly as `effects_bmap.h` packs its 16-bit map
  (`a, b ∈ [0, 1]` by construction — `b = (1−a)·mean`). `Why not R16G16:` neither
  `R16G16_UNORM` nor `R16G16_SFLOAT` is a mandatory storage-image format, and the
  existing `dst` plumbing is rgba8-only; the 16-in-8 pack is proven in-tree.
- **Buffers.** The Brightness Map's `effectsBmapA/B` pair (allocated at ¼ by
  `update_effects_scratch_pair()`) is exactly the pair v2 needs; rename, do not
  reallocate. History texture: `16×17 → 64×18` (`hist_prev` row of 64 bins).
- **Sampler slots.** `VKR_EFFECTS_BLOOM_SLOT` and the bmap slot exist; v2 takes the
  bmap slot. Stage 3's second pair needs one more (or a 2×-wide `v2A` holding both).

### 5.2 Expected cost (1440p, 2560×1440, mid-range GPU — RX 6600 / RTX 3060 class)

| stage | added work | estimate |
| --- | --- | --- |
| Stage 1 | measure pass: one `if` per tap, a 64-bin L1 walk; apply pass: one `pow` + ~15 ALU per pixel replacing two `pow`s | **≈ 0, likely a small saving** over AB Dynamic |
| Stage 2 | ¼-res passes: 5 dispatches × 230 k texels × ≤ 25 taps ≈ 30 M cache-resident fetches; apply: 4 bilinear fetches of a 640×360 RGBA8 + ~30 ALU per pixel × 3.7 M px | **0.2–0.3 ms** |
| Stage 3 | one more coefficient pair (4 dispatches) + 4 fetches per pixel | **+0.1 ms** |
| removed | Brightness Map's 3 dispatches at up to ¼ res, AG's `pow`s, the dark-floor `smoothstep` | −0.1..−0.2 ms when those were on |

`shader-effects.md` says in three places that there is **no GPU-timestamp
instrumentation** in `vulkan_composite()` and that every "sub-millisecond" claim is
unmeasured. **Stage 0 adds it**: `vkCmdWriteTimestamp` before and after the pre-pass
block into a 2-query pool, read back with the existing `effects_ab_log` staging path,
printed as `effects_gpu_us=` on that log line and shown as a Pipeline facts row. Every
number in this table is then replaced by a measurement in the feature doc.

### 5.3 What is reused and what is replaced

| existing | fate |
| --- | --- |
| `cs_effects_measure.comp` histogram, rank-window means, EMA, `dt`, reset-on-resume, history contract | **kept**; gains the black-floor exclusion, the content anchor, `hist_prev` and the cut test; loses the 16×16 local map and its four blur passes |
| `ab_dyn_*`, `ag_*`, `bmap_*`, `dark_weight()` in `effects_curve.h` | **replaced** by `v2_toe()`, `v2_g()`, `v2_detail()`, `v2_binding()` in the same header, unit-tested the same way |
| `EffectsPushData_t` fields `u_ab*`, `u_ag*`, `u_bmap*`, `u_darkFloor` | replaced by `u_v2Lift, u_v2Target, u_v2MaxLift, u_v2Detail, u_v2Scale (as r texels), u_v2AdaptTau, u_v2Clarity, u_v2Mode` |
| `EFFECT_ADAPTIVE_BRIGHTNESS`, `EFFECT_AB_DYNAMIC`, `EFFECT_ADAPTIVE_GAMMA`, `EFFECT_BRIGHTNESS_MAP` bits | one `EFFECT_ADAPTIVE` bit + `EFFECT_ADAPTIVE_LOCAL` (Stage 2 passes recorded) |
| Bloom | **untouched**; still before the adaptive block, still measured from the raw layer |
| Shadow Control, Saturation, Vibrancy, Pre-Sharpen | untouched; still in `grade()`; v2's Y is the graded luma |
| `update_effects_scratch_pair()`, `effectsBmapA/B` | reused as `effectsV2A/B` |
| `EffectPreview.cpp` | re-pointed at the v2 functions (Stage 1), plus a CPU guided filter (Stage 2) |
| `NeedsStatistics()` | true for v2 in `scene` mode only; `off` mode needs no measure pass at all |

---

## 6. Staged delivery

**Stage 0 — instrumentation and reference (½ day).** GPU timestamps (§5.2). The three
new harness scenes (§7.1) and the four new samplers. `v2_*` functions in
`effects_curve.h` with `tests/test_effects_curve.cpp` cases asserting §4.1's guarantees
1–3 over `g ∈ [0.2, 1] × S ∈ [1, 8] × x` (max slope ≤ S to 1e−4, `f(0) = 0`, `f(1) = 1`,
monotone, `f(x; 1, S) ≡ x`, the secant bound, the shoulder's range and unit slope, the
colour step's range). Nothing user-visible yet; no changelog entry.

**Stage 1 — the curve, the anchor, the cut (1 day). The smallest thing that fixes the
capture.** `cs_effects_layer0.comp`'s adaptive block becomes the toe-gamma on `B = Y`
with the secant/shoulder detail term degenerate (`D = 0`) and the colour step;
`cs_effects_measure.comp` gains the black-floor exclusion, the content anchor and the
scene-cut snap; the panel row becomes v2's (Adaptation, Lift, Target, Max lift, Adapt
speed — five params); AG's row, the Brightness Map's row and the dark-floor row are
removed; config migration per §8. Acceptance: on the capture, replayed through the
harness as a still image scene, **≤ 6 % of pixels ≥ 128** (raw 1.1 %, today 21 %),
**0 % ≥ 250**, the zombie/floor difference **≥ 35 codes** (raw 24.7; prototype 37–43);
every existing `dark-dynamic` / `bright` / `mid` contract re-based to v2 with numbers
recorded; `stability-static` p2p 0; the new `flash` scene settles in ≤ 2 frames.
Changelog: one *Added* line, one *Removed* line.

**Stage 2 — the local base (1–2 days).** The ¼-res passes, the guided coefficients, the
real detail term, Detail and Scale params, the preview strip's CPU filter. **Gated**:
ships only if the harness shows, on `silhouette` and `models`, an object Weber contrast
≥ 10 % higher than Stage 1 at the same `g` *and* the halo criterion holds; otherwise the
passes stay behind a ConVar and the doc says why. Changelog: one *Added* line.

**Stage 3 — the silhouette band (1 day), shadow chroma, and the knee variant if the
user asks for it.** Same gate, measured against Stage 2.

Each stage is one commit with its doc update (`shader-effects.md`'s three sections
collapse into one "Adaptive Brightness (v2)" section that keeps the v1 history as
history), its `CHANGELOG.md` line, and its harness output under
`build-release/verify-shots/adaptive-v2-<date>/`.

---

## 7. Verification

### 7.1 New harness scenes (`tests/effects_scene_client.c`)

| scene | content | what it stands in for |
| --- | --- | --- |
| `silhouette` | a 6-code field with 1-code noise; a 24×48 px figure at 3 codes and a second at 10 codes; one 8 % patch of 40..60-code "lit floor"; the rest literal 0 (70 % of the frame) | the capture: a near-black scene with a darker-than-surround player and a lighter one, on a void |
| `flash` | alternates between the `silhouette` frame and a 230-code frame with 200-code shapes every `--flash N` composites (default 120), with `--motion` still applied | a flashbang / a door into daylight; both directions of the cut |
| `skyfore` | top 55 %: 225-code sky with 235/215 cloud bands; bottom 45 %: 14/20-code ground with a 30-px 8-code figure and a 60-px 26-code one | bright skybox above a dark foreground: the half-split scene with objects in it |
| `capture` | `--image <png>`: the scene client uploads a PNG (the user's own capture) as a still | the real thing, replayable |

`models`, `modelsinv`, `halobox`, `haloinv`, `halfsplit`, `texdark`, `blackout`, `dark`,
`bright`, `mid` are kept and re-based.

### 7.2 Pass criteria (`scripts/effects-regression.sh`, new samplers in `pixel_regression_sample.py`)

| check | scene | criterion (defaults unless stated) | stands in for |
| --- | --- | --- | --- |
| `v2-nobinarise` | `capture`, `silhouette` | pixels ≥ 128 ≤ **6 %** on the capture; on `silhouette` the 3-, 6- and 10-code populations remain three distinct populations (their means ordered, each within 0.5·S of its own lifted value); pixels ≥ 250 ≤ **0.5 %** on every scene at every setting | the binarised capture |
| `v2-silhouette` | `silhouette`, `skyfore`, `models` | each figure's **Weber contrast against its surround ≥ raw − 0.02** (the harness's own tolerance for the shoulder's bounded trade on positive detail, §4.5) and its **absolute difference ≥ 2× raw** at the defaults; at Max lift 1 the frame is byte-identical to raw | "can I see the enemy" |
| `v2-slope` | `dark`, `texdark` | for every adjacent band pair, `(out_b − out_a) / (in_b − in_a) ≤ S · Detail + 0.05` | guarantee 1 |
| `v2-black` | `silhouette`, `blackout` | pixels at code 0..2 in the input are at code 0..2 in the output, exactly | guarantee 2 |
| `v2-mono` | all band scenes | band order preserved, no two bands merge (≥ 2 codes apart) | guarantee 3 |
| `v2-halo` | `halobox`, `haloinv`, `models` | field 4 px from an edge vs far field: **≤ 4 codes** at defaults, **≤ 8** at Max lift 8 / Detail 2 / Clarity 1; monotone profile out from the edge | guarantee 4; the Brightness Map measured +37.5 here |
| `v2-static` | `silhouette` still (`SIGUSR2`) | output pixel p2p **0**, smoothed anchor p2p < 0.1 code over 400 composites | guarantee 5 |
| `v2-pan` | `texdark --periodic --lights 2` | anchor p2p ≤ 3 codes, `g` p2p ≤ 0.02, **no cut fired** over 300 frames | the 2026-09-07 pulse must not return, and a pan must not look like a cut |
| `v2-cut` | `flash` | after each switch the anchor reaches its new value in **≤ 2 composites**; no overshoot; the bright frame has ≤ 0.5 % ≥ 250 on its *first* composite | door / flash |
| `v2-slide` | `texdark` with `--lights` ramped over 3 s | anchor follows with the EMA's own time constant (settle within 5 % of 3·tau); no cut fired | gradual change |
| `v2-sky` | `skyfore` | sky bands keep ≥ 6 codes of separation; sky mean drops by ≤ 12 %; the ground figures pass `v2-silhouette` | the highlight guard |
| `v2-colour` | `colors` | hue angle of every patch within 2° of raw; chroma ratio within 5 % where nothing was compressed | §4.7 |
| `v2-gpu-time` | any | `effects_gpu_us` printed; Stage 2 ≤ 350 µs at 1440p on the desktop GPU (recorded, then asserted at 1.5× the measured value) | §5.2 |
| `v2-preview` | `dark` | the Inspector strip's right half equals the GPU output at the probe within 2 codes | the strip is honest |

### 7.3 Unit tests (`tests/test_effects_curve.cpp`)

The Stage 0 list (§6) plus: `t(g, S)` gives `f′(0) = S` to 1e−4 across the grid; the
secant is ≤ S; the shoulder never exceeds 1 and has unit slope at 0; the colour compress
keeps hue (channel ratios about Y′) and range; `v2_binding()` returns exactly one code
and Target is inert iff it says so (the ab_dyn pattern); the anchor exclusion ignores a
frame that is 99 % black and holds the previous value on one that is 100 % black.

### 7.4 What the user should test on real maps

- **The capture's map, same spot.** Lift 0.5 / Target 0.35 / Max lift 4: does the
  corridor read without turning grey-white? Then Max lift 6 and 8: at what point does
  dither become visible, and is a 3-code shape readable before that?
- **A bright map with dark models** (the original AG complaint): Adaptation Off, Lift
  0.3 / 0.5 / 0.7 — is the model lifted, and does the sky stay a sky?
- **Flashbangs and doors**: is the first frame after a flash white-but-not-worse-than-raw,
  and does a corridor light up immediately on entry rather than over a second?
- **Adaptation Scene vs Off in a full match**: does *anything* pulse; is Off the mode you
  would actually leave on?
- **Stage 2**: Detail 1.0 vs 1.5 on a textured wall: more readable or more noisy?
- **Stage 3**: Clarity 0.5 — do models pop, and is there any rim on hard edges?

---

## 8. Decisions and open questions

### 8.1 Recommended decisions (reversible; say so and the plan changes)

| decision | recommendation | why |
| --- | --- | --- |
| **Adaptive Gamma** | **remove**; v2 *is* gamma-based (toe-gamma) and satisfies both reasons the user asked for AG — cannot clip, changes contrast not exposure — with a bounded slope AG lacks | two rows that aim the same median at the same target from the same statistics was the reason for the mutual exclusion; one operator makes the exclusion, the "which one is on" uniform masking and the twin speed sliders all disappear |
| **Mutual exclusion** | gone with AG | nothing left to exclude |
| **Brightness Map** | remove (the user's request); keep `update_effects_scratch_pair()` and the ¼-res pair for Stage 2; keep `halobox`/`haloinv`/`models` scenes as v2's halo gate | its buffers and its adversarial scenes are the most useful things it leaves behind |
| **Dark floor** (`reshade.dark_floor`, 2026-09-14) | remove in Stage 1; the key is ignored on load, no schema bump | subsumed by the slope bound + the black floor; a floor that switches the effect off on the target scene must not survive |
| **Shadow Control** | keep as-is | it is per-pixel, in `grade()`, feeds Bloom's and v2's own statistics, and a user may want a plain global toe without any adaptivity. v2's `Lift` is a superset; the doc says so |
| **Whole-image mode** | remove | the `.fx` heritage gain; the one mode that clips by design |
| **Config keys** | `reshade.adaptive_brightness.{enabled, mode, lift, target_luminance, max_lift, detail, scale, adapt_speed, clarity}`; migration on load: `max_gain → max_lift` (same number, range widened), `target_luminance` kept, `strength → lift` (0..1 → 0..1), `adapt_up_speed → adapt_speed`, `mode: whole_image|dynamic → scene`; `reshade.adaptive_gamma.*` → if AG was `enabled` and AB was not, `enabled = true, max_lift = max_lift, target = target, lift = 0.5`; `reshade.brightness_map.*` and `dark_floor` ignored. `kCurrentSchemaVersion` 4 → 5 so the migration runs once and an old `mode` string cannot survive | the id `image.shaders.adaptive_brightness` and the struct name stay (palette state, settings-audit rows, the row's position in the Effects band), which is the rule the 2026-09-05 retitle set |
| **Param budget** | seven now, Clarity eighth | `kParamBudget` is 8; the Dynamic-mode row already spends 8 |
| **Row title** | "Adaptive Brightness" unchanged; help text rewritten | the user's own name for the feature |

### 8.2 Questions only the user can answer

1. **Toe (film) or knee (monitor)?** v2's curve compresses the *highlights* by `≈ g` to
   pay for the lift (a bright sky gets ~10–15 % darker at Lift 0.5). The monitor-style
   alternative leaves the top untouched and compresses the mid-tones just above the
   shadows instead (the "flattened grey" at high settings). Which trade do you want by
   default — and is it worth a **Lift shape** choice, or should one be chosen and the
   other dropped?
2. **Adaptation default: Scene or Off?** Off is the zero-motion mode a competitive
   player might prefer; Scene deepens the lift on a genuinely dark map. Which should a
   fresh profile start on?
3. **Max lift ceiling: 8 or 4?** At 8, a 3-code shape becomes 24 codes (readable) and
   1-code dither becomes 8 codes (visible grain on flat walls). Is visible grain an
   acceptable price for the user to choose, or should the slider stop at 4–6?
4. **Colour: preserve chroma (v2) or per-channel (every other effect here)?** Per-channel
   lifting desaturates dark colours; ratio-preserving keeps a dark red red. Do you want
   v2 to preserve colour, and if so should Shadow Control be switched to the same rule
   for consistency?
5. **Should the compositor's HUD/crosshair layer be excluded from the statistics?** The
   game's own HUD cannot be (it is in the frame), but the fork's own HUD is drawn later
   and already is. Do you play with chat/HUD elements that are bright enough to matter,
   and should the measure pass ignore a configurable border (e.g. the bottom 15 % where
   the chat lives)?
6. **Stage 3's Clarity — do you want it at all?** It is the one term that acts on
   silhouettes specifically rather than on exposure, and it is the term competitive
   ReShade users lean on; it is also the only part of v2 with any unsharp-mask heritage.
   Build it, or stop at Stage 2?

---

## 9. Sources

- Durand & Dorsey 2002, *Fast bilateral filtering for the display of HDR images* —
  <https://people.csail.mit.edu/fredo/PUBLI/Siggraph2002/DurandBilateral.pdf>
- He & Sun 2015, *Fast Guided Filter* — <https://arxiv.org/abs/1505.00996>
  (algorithm: <https://ar5iv.labs.arxiv.org/html/1505.00996>); *Guided Image Filtering*,
  TPAMI 2013 — <https://ieeexplore.ieee.org/document/6319316/>
- Eilertsen, Mantiuk, Unger 2015, *Real-time noise-aware tone mapping* —
  <https://www.cl.cam.ac.uk/~rkm38/pdfs/eilertsen2015rt_na_tmo.pdf>,
  <https://computergraphics.on.liu.se/rntm/>
- Wronski 2022, *Exposure Fusion — local tonemapping for real-time rendering* —
  <https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/>
- *Real-time Tone Mapping: A State of the Art Report* — <https://arxiv.org/pdf/2003.03074>
- Boitard et al., *Temporal Coherency for Video Tone Mapping* —
  <https://people.irisa.fr/Ronan.Boitard/articles/2012/TCVTM2012.pdf>; survey —
  <https://people.ece.ubc.ca/rboitard/articles/2013/Boitard%20et%20al.-2013-Temporal%20coherency%20in%20Video%20Tone%20Mapping,%20A%20Survey.pdf>
- CLAHE overview — <https://www.emergentmind.com/topics/contrast-limited-adaptive-histogram-equalization-clahe>;
  MathWorks HDL CLAHE — <https://www.mathworks.com/help/visionhdl/ug/contrast-adaptive-histogram-equalization.html>
- Retinex halo behaviour — <https://www.researchgate.net/publication/255970426_Halo-Free_Design_for_Retinex_based_Real-Time_Video_Enhancement_System>,
  <https://arxiv.org/pdf/2106.06971>
- Black eQualizer / Shadow Boost / Night Vision — <https://prosettings.net/blog/what-is-black-equalizer/>,
  <https://evezone.evetech.co.za/performance-pulse/best-shadow-boost-settings-gaming-tested/>,
  <https://evezone.evetech.co.za/deep-dives/how-to-use-shadow-boost-without-washing-out-dark-areas-in-games>
- NVIDIA App / RTX Dynamic Vibrance — <https://www.nvidia.com/en-us/geforce/news/nvidia-app-beta-download/>,
  <https://www.tweaktown.com/news/96343/rtx-hdr-and-dynamic-vibrance-use-ai-to-dramatically-improve-the-look-of-thousands-games/index.html>;
  Freestyle filters — <https://www.itechguides.com/how-to-customize-gaming-visuals-with-nvidia-freestyle-game-filters/>
- AMD FidelityFX CAS — <https://gpuopen.com/fidelityfx-cas/>
- ReShade Clarity.fx — <https://github.com/BlueSkyDefender/AstrayFX/blob/master/Shaders/Clarity.fx>;
  a VALORANT competitive preset — <https://sfx.thelazy.net/games/preset/10904/>
- CS2 blocks injection — <https://steamcommunity.com/sharedfiles/filedetails/?id=3662582248>;
  overlay vs injection under anti-cheat — <https://backgrind.com/overlay-anticheat-checker/>;
  ReShade and anti-cheat — <https://www.pcgamingwiki.com/wiki/ReShade>
- Riot, *VALORANT Shaders and Gameplay Clarity* — <https://www.riotgames.com/en/news/valorant-shaders-and-gameplay-clarity>
- Contrast metrics — <https://www.sciencedirect.com/science/article/pii/S0042698913001132>;
  spatio-chromatic CSF, mesopic/photopic — <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC7405764/>;
  visual search in low mesopic light — <https://link.springer.com/article/10.3758/s13414-018-1512-0>
- CS2 gamma/brightness guides — <https://bo3.gg/articles/cs2-gamma-command-settings>,
  <https://skin.land/blog/cs2-gamma-settings-guide/>,
  <https://www.xp-feed.com/en/guides/config-grafica-cs2-fps-visibilidad-competitivo>
