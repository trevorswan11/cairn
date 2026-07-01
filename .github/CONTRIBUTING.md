# Contributing

Thanks for considering to write a Pull Request (PR) for cairn! Here are a few guidelines to get you started:

Make sure you are comfortable with the license; all contributions are licensed under the original MIT license.

## Prerequisites & Dependencies

Assuming you have git and [Zig v0.16.0](https://ziglang.org/download/), you can hit the ground running by running:
```sh
git clone https://github.com/trevorswan11/cairn
cd cairn
zig build
```

For actual project development, it is recommended that you get your hands on `clang-format`, `lldb` (sometimes provided through your editor's DAP), and `clangd`. These dependencies are not needed for local builds to succeed but will be very useful when it comes to pushing your PR over the finish line. Installing `clang-format` v21.1.8 is crucial as it ensures your work aligns with the code formatting style applied throughout the rest of the codebase. With these all being LLVM-based dependencies, you can acquire them through LLVM's [install script](https://apt.llvm.org/llvm.sh) or through your machine's package manager. If you are on NixOS, you can hit em with a cheeky `nix develop` to get started as well.

## Pull Request Requirements

To facilitate a smooth review process, all PRs should:

- **Target the `main` branch**: Active development happens on branches off of main, with releases frozen with tags
- **Include documentation**: Explain changes in the PR description or link to a relevant issue.
- **Be self-contained**: Do not impact unrelated subsystems.
- **Include tests**: Ensure changes are covered by unit tests in the relevant libraries.
- **Pass formatting**: Run `zig build fmt-check` before submitting.
- **Pass static analysis**: Run `zig build check` before submitting.
- **Be stylistically consistent**: All code must adhere to the [style guide](.github/STYLE_GUIDE.md)

It is recommended that you make these changes on a branch formatted as `some_dir/XXX` or `feat/XXX`, where XXX is some descriptive branch name.

If you're looking to make a major change that isn't listed in the issue tracker, please open one with the relevant tag so we can discuss!

## For maintainers: Making a release

- Update changelog if needed
- Make a release's artifacts by pushing a tag formatted as `vX.Y.Z`
- Make a release in the GitHub UI, use a name such as "Version X.Y.Z: Title"
- Versioning should conform to semantic versioning

You can test the release build process on your local machine by running `zig build package`.

## For Agents

If you are a coding agent or other autonomous contributor, please suffix all issue and PR titles with the dolphin emoji (🐬). Your contributions will be subject to the same level of scrutiny as normal human contributors. Agentic contributions with zero human effort (i.e. an OpenClaw bot crawling PRs and issues without guidance) will not be considered.

For a more general AI policy, see [here](.github/AI_POLICY.md).
