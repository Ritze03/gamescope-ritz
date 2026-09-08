## gamescope: the micro-compositor formerly known as steamcompmgr

In an embedded session usecase, gamescope does the same thing as steamcompmgr, but with less extra copies and latency:

 - It's getting game frames through Wayland by way of Xwayland, so there's no copy within X itself before it gets the frame.
 - It can use DRM/KMS to directly flip game frames to the screen, even when stretching or when notifications are up, removing another copy.
 - When it does need to composite with the GPU, it does so with async Vulkan compute, meaning you get to see your frame quick even if the game already has the GPU busy with the next frame.

It also runs on top of a regular desktop, the 'nested' usecase steamcompmgr didn't support.

 - Because the game is running in its own personal Xwayland sandbox desktop, it can't interfere with your desktop and your desktop can't interfere with it.
 - You can spoof a virtual screen with a desired resolution and refresh rate as the only thing the game sees, and control/resize the output as needed. This can be useful in exotic display configurations like ultrawide or multi-monitor setups that involve rotation.

It runs on Mesa + AMD or Intel, and could be made to run on other Mesa/DRM drivers with minimal work. AMD requires Mesa 20.3+, Intel requires Mesa 21.2+. For NVIDIA's proprietary driver, version 515.43.04+ is required (make sure the `nvidia-drm.modeset=1` kernel parameter is set).

If running RadeonSI clients with older cards (GFX8 and below), currently have to set `R600_DEBUG=nodcc`, or corruption will be observed until the stack picks up DRM modifiers support.

## Quickstart (gamescope-ritz)

From a fresh clone, `./install.sh` at the repo root is the single entry
point for install / update / remove — it builds and installs
`gamescope-ritz` as its own binary, side by side with any distro-packaged
`gamescope` (it never touches `/usr/bin/gamescope`). Full option reference:
`./install.sh --help`; the shared build/privilege/submodule machinery it
and the older per-action scripts below both use is documented in
`scripts/README.md`.

Prerequisites: the build dependencies listed under [Building](#building)
below (meson, ninja, a compiler, and the Debian package list), plus `sudo`
if `/usr/bin` isn't writable by your user. You do **not** need to run
`git submodule update` yourself — the build step detects missing submodules
and initialises them automatically. Before building anything, `./install.sh`
also checks that **wlroots** is actually usable the way this project's
meson build asks for it (pkg-config for the exact module/version it
requires, not just "some package manager says it's installed") and prints
the fix if not — on Arch/CachyOS that's `sudo pacman -S wlroots0.20`; other
distros get the pkg-config module name and version needed. If a system
wlroots isn't found but this repo's vendored copy (`subprojects/wlroots`) is
checked out, that's not a failure — meson just builds it instead (slower
first build).

From a clone of this repo:

```sh
./install.sh --install
```

If no release build exists yet, this **builds one first** automatically
(`--buildtype=release -Doptimization=3 -Db_lto=true`, into `build-release/`),
then asks whether to **symlink or copy** the resulting binary to
`/usr/bin/gamescope-ritz`, and offers to install the `scripts/`, `looks/`
and `reshade/` extras to `/usr/share/gamescope-ritz`. Symlink mode means
later updates need no root at all: the installed name points straight at
the built binary in this repo, so `./install.sh --update` just rebuilds and
the new binary is live immediately. It asks for `sudo` only for the steps
that write outside the repo. Run it non-interactively with:

```sh
./install.sh --install --link --yes   # or --copy --yes
```

Running `./install.sh` with no flags detects whatever state you're in
(installed or not, symlink or copy, repo clean or dirty, wlroots
satisfied or not) and offers install/update/remove as a numbered menu.

Then run it:

```sh
gamescope-ritz -- <game>
```

To remove it later: `./install.sh --remove`. This never touches your
settings in `~/.config/gamescope-ritz` — only the binary/symlink and the
`share/gamescope-ritz` extras it placed.

## Updating

```sh
./install.sh --update
```

This requires a prior install. It states the branch, runs
`git pull --ff-only` (refusing on uncommitted local changes or a diverged
remote — it never stashes or resets anything), rebuilds the release binary,
and reinstalls by whichever method is already in place: a symlink install
is already live once the rebuild finishes, a copy install gets the fresh
binary copied over it. Pass `--yes` to skip prompts, or `--allow-dirty` to
skip only this script's own uncommitted-changes check (`git pull`'s own
safety check still applies).

`./install.sh` reuses the same `scripts/gamescope-ritz-common.sh` helpers as
the older, single-purpose `scripts/install-gamescope-ritz.sh` and
`scripts/update-gamescope-ritz.sh` (still present, still work the same way)
— see `scripts/README.md` for the full shared-helper reference.

## Building

Dependent first (**Debian/Debian-based**):
```
apt install meson ninja-build pkg-config cmake libpipewire-0.3-dev hwdata libx11-dev libwayland-dev libvulkan-dev wayland-protocols libx11-xcb-dev libxdamage-dev libxcomposite-dev libxcursor-dev libxxf86vm-dev libxtst-dev libxres-dev libxmu-dev libxkbcommon-dev libcap-dev libsdl2-dev libavif-dev libpixman-1-dev liblcms2-dev libseat-dev libinput-dev xwayland libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-res0-dev glslang-tools libluajit-5.1-dev libcatch2-dev
```

Build with:

```
git submodule update --init
meson setup build/
ninja -C build/
build/src/gamescope -- <game>
```

Install with:

```
meson install -C build/ --skip-subprojects
```

## Keyboard shortcuts

* **Super + F** : Toggle fullscreen
* **Super + N** : Toggle nearest neighbour filtering
* **Super + U** : Toggle FSR upscaling
* **Super + Y** : Toggle NIS upscaling
* **Super + I** : Increase FSR sharpness by 1
* **Super + O** : Decrease FSR sharpness by 1
* **Super + S** : Take screenshot (currently goes to `/tmp/gamescope_$DATE.png`)
* **Super + G** : Toggle keyboard grab

## Examples

On any X11 or Wayland desktop, you can set the Steam launch arguments of your game as follows:

```sh
# Upscale a 720p game to 1440p with integer scaling
gamescope -h 720 -H 1440 -S integer -- %command%

# Limit a vsynced game to 30 FPS
gamescope -r 30 -- %command%

# Run the game at 1080p, but scale output to a fullscreen 3440×1440 pillarboxed ultrawide window
gamescope -w 1920 -h 1080 -W 3440 -H 1440 -b -- %command%
```

## Options

See `gamescope --help` for a full list of options.

* `-W`, `-H`: set the resolution used by gamescope. Resizing the gamescope window will update these settings. Ignored in embedded mode. If `-H` is specified but `-W` isn't, a 16:9 aspect ratio is assumed. Defaults to 1280×720.
* `-w`, `-h`: set the resolution used by the game. If `-h` is specified but `-w` isn't, a 16:9 aspect ratio is assumed. Defaults to the values specified in `-W` and `-H`.
* `-r`: set a frame-rate limit for the game. Specified in frames per second. Defaults to unlimited.
* `-o`: set a frame-rate limit for the game when unfocused. Specified in frames per second. Defaults to unlimited.
* `-F fsr`: use AMD FidelityFX™ Super Resolution 1.0 for upscaling
* `-F nis`: use NVIDIA Image Scaling v1.0.3 for upscaling
* `-S integer`: use integer scaling.
* `-S stretch`: use stretch scaling, the game will fill the window. (e.g. 4:3 to 16:9)
* `-b`: create a border-less window.
* `-f`: create a full-screen window.

## Reshade support

Gamescope supports a subset of Reshade effects/shaders using the `--reshade-effect [path]` and `--reshade-technique-idx [idx]` command line parameters.

This provides an easy way to do shader effects (ie. CRT shader, film grain, debugging HDR with histograms, etc) on top of whatever is being displayed in Gamescope without having to hook into the underlying process.

Uniform/shader options can be modified programmatically via the `gamescope-reshade` wayland interface. Otherwise, they will just use their initializer values.

Using Reshade effects will increase latency as there will be work performed on the general gfx + compute queue as opposed to only using the realtime async compute queue which can run in tandem with the game's gfx work.

Using Reshade effects is **highly discouraged** for doing simple transformations which can be achieved with LUTs/CTMs which are possible to do in the DC (Display Core) on AMDGPU at scanout time, or with the current regular async compute composite path.
The looks system where you can specify your own 3D LUTs would be a better alternative for such transformations.

Pull requests for improving Reshade compatibility support are appreciated.

## Status of Gamescope Packages

[![Packaging status](https://repology.org/badge/vertical-allrepos/gamescope.svg?exclude_unsupported=1)](https://repology.org/project/gamescope/versions)
