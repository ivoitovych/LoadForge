# Packaging and release

How LoadForge is built for release, what comes out, and how a release is made. The scripts
here are the whole of the release build: the `release` workflow
(`.github/workflows/release.yml`) calls them, and you can call them by hand and get the
same files.

| File | What it does |
|---|---|
| `build-release.sh` | Builds the static and the dynamic artifact for this machine, reads each back with the project's gates, makes the tarballs and the Debian package, runs each unpacked copy, writes the checksums |
| `build-deb.sh` | Makes the Debian package from a dynamically linked binary (called by the above) |
| `test-deb.sh` | Installs a package, runs the installed copy, removes it, checks nothing is left (needs root) |
| `validate-machine.sh` | The scripted half of the hardware validation: runs a binary through the checklist's commands and writes the results file (`docs/hardware-validation.md`) |

## What a release is

Two artifacts, for each of x86-64 and ARM64:

| Artifact | For | Form |
|---|---|---|
| **static** | live and rescue media; any Linux older than Ubuntu 24.04; any machine where you install nothing | `loadforge-VERSION-linux-ARCH-static.tar.gz`, holding one statically linked `loadforge` |
| **dynamic** | Ubuntu 24.04 and later | `loadforge-VERSION-linux-ARCH-dynamic.tar.gz` (the same binary, for a machine without `dpkg`) and `loadforge_VERSION_ARCH.deb` |

and one `loadforge-VERSION-linux-ARCH.SHA256SUMS` per architecture (a release merges them
into `SHA256SUMS`). Every tarball holds `loadforge`, `LICENSE`, `NOTICE`, `README.md`,
`validate-machine.sh` and `docs/` (architecture, safety, telemetry sources, the hardware
checklist). The package installs `/usr/bin/loadforge` and the same documents under
`/usr/share/doc/loadforge/`.

Ubuntu 24.04 is the declared minimum for the dynamic build (`docs/IMPLEMENTATION.md`, Q11):
it is built and tested there, with GCC 13, and needs the libstdc++ 13 that 24.04 ships. The
package's `Depends` line is computed from the binary by `dpkg-shlibdeps`, not written by
hand. On the first build it read:

```
Depends: libc6 (>= 2.34), libgcc-s1 (>= 4.3), libstdc++6 (>= 13.1)
```

The static build has no such dependency; that is what it is for (F19).

The package is named `loadforge`, installs one executable and no service, no
configuration, nothing outside `/usr/bin` and `/usr/share/doc/loadforge`, and has no
maintainer scripts. Removing it removes everything it installed.

## Build the artifacts by hand

On a machine of the architecture you want, with `cmake` 3.28 or later, `ninja-build`,
`g++-13`, `dpkg-dev`, `binutils` and `file`:

```
packaging/build-release.sh --out ~/artifacts
```

It takes about four minutes on four cores. `--only static` or `--only dynamic` builds one.
For each flavour it:

1. builds (static: `cmake --preset static`; dynamic: Release flags without the static
   link and without the tests);
2. reads the binary back: `tools/check-no-test-hooks.sh` on both,
   `tools/check-runtime-deps.sh` on the dynamic one (only libc, libm, libstdc++ and
   libgcc_s), and for the static one that it has no program interpreter and no dynamic
   section;
3. strips a copy and makes the tarball with the commit's timestamp, so the same binary
   always gives the same tarball bytes;
4. unpacks the tarball elsewhere and **runs the unpacked copy**: `--version` must match,
   and `stress --test sort --duration 2s` must end `verdict: clean` with exit 0;
5. for the dynamic flavour, builds the package.

It ends by writing the checksum file. Any failing step stops it with that step's exit
code. What a run printed on the machine this was written on (x86-64):

```
test-hook check: .../build/release-dynamic/loadforge: 3303 symbol(s) read, 0 test hook(s), ok
  ok        libc.so.6
  ok        libgcc_s.so.1
  ok        libm.so.6
  ok        libstdc++.so.6
runtime dependencies (Advanced Micro Devices X86-64): 4 linked, 0 not permitted
   loadforge-0.0.0-m2-linux-x86_64-dynamic: --version '0.0.0-m2', two-second stress run clean
== done: 0.0.0-m2 on x86_64
7526816497f2...  loadforge-0.0.0-m2-linux-x86_64-static.tar.gz
59c36b5eb013...  loadforge-0.0.0-m2-linux-x86_64-dynamic.tar.gz
b1783e52f82a...  loadforge_0.0.0~m2_amd64.deb
```

(The checksums depend on the build; yours differ.) The two binaries print the same
`--selftest-digest` (`e0ff849ff34906de`) as a debug build of the same commit.

## Install and remove the package

```
sudo apt install ./loadforge_VERSION_ARCH.deb      # or: sudo dpkg -i loadforge_VERSION_ARCH.deb
loadforge --version
sudo apt remove loadforge                          # or purge; there is no configuration
```

`sudo packaging/test-deb.sh loadforge_VERSION_ARCH.deb` does that and checks it: the
installed binary is `/usr/bin/loadforge`, reports the package's version, has no unresolved
library, ends a two-second stress run `verdict: clean`, and after `dpkg --purge` no file
of the package is left and `loadforge` is gone from the `PATH`. It refuses to run if
`loadforge` is already installed, and purges the package on the way out even if a check
failed.

The Debian version is the binary's version with the first `-` turned into `~`
(`1.0.0-rc1` becomes `1.0.0~rc1`), so that a pre-release sorts before the release.

## Make a release

1. On the release commit set `project(LoadForge VERSION X.Y.Z)` and
   `LOADFORGE_MILESTONE` in `CMakeLists.txt` so that `loadforge --version` prints exactly
   the version you mean (`1.0.0`, or `1.0.0-rc1`). The build does not choose a version;
   the tree at this commit still says `0.0.0-m2`.
2. Hardware validation (`docs/hardware-validation.md`) is done on artifacts built from that
   commit, either by `build-release.sh` on each machine or from a rehearsal (below).
3. Push an unsigned tag: `git tag vX.Y.Z && git push origin vX.Y.Z`. The
   `release` workflow then, on x86-64 and on ARM64: builds both artifacts, installs and
   removes the package, verifies the checksums, and checks that the tag is the version the
   binary reports (a mismatch fails the run). Then one job merges the checksums and
   creates a **draft** release with every file attached.
4. Publish the draft by hand, after the hardware results are in `tests/hardware/`.

**Rehearsal.** Run the `release` workflow by hand (Actions, release, Run workflow, or
`workflow_dispatch`) on any branch: it builds and checks everything and publishes
nothing. A pull request that touches `packaging/` or the workflow does the same.

All actions in the workflow are pinned to commit SHAs, as `licensing.yml` requires.

## Why this shape

The choices, with the alternatives and the reasons, are recorded in
`docs/journal/34-release-1-0.md`: a package built from the release binary rather than
through CPack and an `install()` rule; one script shared by the workflow and the owner;
a draft release; tarballs for both flavours as well as a package.
