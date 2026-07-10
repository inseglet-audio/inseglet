# Contributing to Inseglet

Thanks for your interest in Inseglet — a native REAPER MCP extension for immersive/spatial audio and
native ADM (Dolby Atmos) authoring. Contributions of all sizes are welcome: bug reports, docs, tests,
new tools, and DSP.

By participating you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

## Ways to contribute

- **Report a bug** or **request a feature** via [Issues](../../issues) (templates provided).
- **Ask a question** or float an idea in [Discussions](../../discussions).
- **Open a pull request** — for anything larger than a small fix, please open an issue or discussion
  first so we can agree on the approach before you invest the time.

## Getting oriented

Start with:

- [`docs/REFERENCE.md`](docs/REFERENCE.md) — the generated tool/resource/prompt surface.
- [`docs/CONVENTIONS.md`](docs/CONVENTIONS.md) — channel order, coordinate math, bed layouts, ambisonic
  wiring, loudness/deliverable specs. **Read the relevant section before touching spatial or metering code.**

## Build & test

Prereqs: CMake ≥ 3.20 and a C++17 compiler (MSVC on Windows, AppleClang on macOS, GCC/Clang on Linux).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release -R unit    # host-core unit + protocol suite — no REAPER required
cmake --install build                        # deploys the extension + bundled JSFX
```

The unit + protocol suite runs entirely off‑REAPER (SDK‑free host core), so you can develop and test
most changes without a REAPER install. Native tool bodies compile under `REAPER_MCP_HAVE_SDK`.

### Two gotchas that will save you an hour

- **`cmake --install` after every rebuild.** Building alone leaves the previously installed binary in
  REAPER's `UserPlugins/`; the install step is what REAPER actually loads.
- **Keep REAPER in the foreground when live‑testing.** macOS App Nap suspends the UI timer and stalls
  the main‑thread pump.

## Pull request checklist

- [ ] Tests pass: `ctest --test-dir build -C Release -R unit`.
- [ ] New/changed behavior is covered by a unit or protocol test.
- [ ] If you changed the tool/resource/prompt surface, **regenerate the reference**
      (`cmake --build build --target reference-doc`) so `docs/REFERENCE.md` stays in sync.
- [ ] Every state mutation is wrapped in a single undo block.
- [ ] First‑party source files carry an `SPDX-License-Identifier: MIT` header.
- [ ] No secrets, tokens, or absolute local paths committed.
- [ ] Builds clean with no new first‑party warnings.

## Commit & PR style

- Keep commits focused; write imperative subject lines (e.g. `spatial: add convert_order`).
- [Conventional Commits](https://www.conventionalcommits.org/) prefixes are appreciated but not required.
- Reference the issue you're closing (`Closes #123`).
- Sign off your commits with the [Developer Certificate of Origin](https://developercertificate.org/):
  `git commit -s` adds a `Signed-off-by` line certifying you have the right to submit the work.

## Licensing

Inseglet is MIT‑licensed. By contributing, you agree that your contributions are licensed under the same
[MIT License](LICENSE).

## Questions

Open a [Discussion](../../discussions) or reach the maintainer listed in [`MAINTAINERS.md`](MAINTAINERS.md).
