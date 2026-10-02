# Building the game DLLs on Linux (cross-build for Windows)

`crossbuild.sh`, in this directory next to the solution, produces `client.dll`, `server.dll` and
`missionchooser.dll` on a Linux machine. The result is a 32-bit Windows binary that the Windows
engine loads like a build made with Visual Studio.

It is a cross-build: the build runs on Linux, the target is `i686-pc-windows-msvc`. The target
triple matters, because the DLLs are loaded by an engine built with the MSVC toolchain and must
therefore use the MSVC ABI and the MSVC C runtime - not MinGW/GNU. That is also why the projects use
clang-cl and lld-link: clang can target the MSVC ABI, GCC cannot.

The switches the script passes to clang-cl and lld-link match the ones MSBuild records for the
ClangCL configuration in its `.tlog` files, for all six project and configuration combinations,
precompiled header included. The DLLs built this way run in the game.

## What you need

**LLVM 22 or 23**, with `clang-cl` and `lld-link`. On Debian and Ubuntu:

```bash
wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && sudo ./llvm.sh 22 && sudo apt install lld-22
```

Where the installation lives is the package manager's business, so the script asks the host instead
of assuming a path: it looks for `clang-cl` on `PATH`, asks `llvm-config` for its bindir and scans
the prefixes the distributions and the ports use, and picks the candidate by the version it reports.
`LLVM=` overrides all of that and is used as given.

The major version matters because of the files being linked: the Release variants under
`src/game/client/videoservices/lib/win32/clang_cl/release/` are LLVM bitcode. The script verifies
before linking, with `llvm-nm`, that every library the project asks for can be read, so a library it
cannot read is reported as such instead of surfacing inside the linker. The Debug variants are native
COFF and do not depend on the LLVM version.

**The MSVC headers, CRT and Windows SDK**, either with `xwin` (it downloads them from Microsoft and
lays them out for cross-compilation, including the symlinks that fix the casing of the SDK headers):

```bash
cargo install xwin --locked
xwin --accept-license --arch x86 --http-retry 5 splat --output ~/xwin \
     --include-debug-libs --preserve-ms-arch-notation
```

or by copying the toolchain and the Windows SDK from a Visual Studio installation you are licensed
for. The layout the script needs is `crt/include`, `sdk/include/um` and `sdk/lib/um/x86` under one
directory; `XWIN=` points it at that directory.

**python3**, used to read the project files.

Nothing else is needed, and nothing has to be configured: the script finds the repository (its own
directory), the LLVM binaries and the SDK tree by itself.

## Running it

```bash
./crossbuild.sh                                    # everything, Release
./crossbuild.sh Debug                              # one configuration (Debug, Release, Profile)
./crossbuild.sh Release client                     # one project
./crossbuild.sh Debug server ai_behavior_follow.cpp # one file (smoke test)
./crossbuild.sh --dry-run                          # print the command lines, build nothing
./crossbuild.sh --check-conditions                 # audit which project settings are taken
./crossbuild.sh --help                             # the text at the top of the script
```

Useful environment overrides: `LLVM`, `XWIN`, `OUT` (scratch directory for the generated response
files), `JOBS` (compile parallelism), `PYTHON`.

## What it reads, and what it reproduces

The script reads the three `.vcxproj` files and the two property sheets next to them
(`asrd_toolset_dirs.props`, `clang_cl.props`). It does not read the solution.

It reproduces the command lines of the **ClangCL** configuration, including the fixes Visual
Studio's ClangCL toolset applies to the project settings (target `ClangClFixupCompileOptions` in
`Microsoft.Cpp.ClangCl.Common.targets`): `ProgramDatabase`/`EditAndContinue` become `/Z7`, `/MP` and
`/RTC1` are never passed, and `GenerateDebugInformation` becomes `/DEBUG:FULL`.

A setting can live in a `PropertyGroup` or as item metadata in an `ItemDefinitionGroup`, and Visual
Studio uses both forms depending on which page it came from; the script merges the two, with item
metadata winning, and reads every setting from that merged view. Values Visual Studio supplies from
its own property sheets, because the projects only store what differs from the default, are embedded
in the script with the file they come from named in a comment. Every switch that reaches the
compiler is either mapped from the project files or declared there; anything new is reported instead
of being ignored.

Items are collected only from the `ItemGroup`s whose condition holds for the configuration being
built, which is how the projects select between the two variants of the video libraries and how a
source that several groups repeat is compiled once.

Each translation unit can also carry settings of its own, which win over the project's. The
precompiled header is read that way: a unit marked `Create` gets `/Yc`, one marked `Use` gets `/Yu`,
and one marked `NotUsing`, or carrying nothing, gets `/Y-`, with the output path the project names or
the one MSBuild derives. The unit that creates the header is compiled before the units that read it.

## Where the output goes

Where a Windows build puts it - the `OutDir` and `IntDir` the projects define, which are
`<Configuration>_swarm\<toolset>\` and `<Configuration>_swarm\<toolset>\intermediate\` (the
missionchooser project uses `<Configuration>\<toolset>\`). Both are listed in `.gitignore`, so a
cross-build never dirties the repository. The generated response files and the helper script go to a
scratch directory outside the repository (`/tmp/asrd-crossbuild-<config>` by default).

The `PreBuild`, `PreLink` and `PostBuild` events of the projects are **not** run: they are Windows
batch scripts that copy the DLLs into a game installation. Copy the DLLs yourself if you need them
there.

## Case sensitivity of file names

Windows ignores the case of a file name, Linux does not, and this repository contains about 1600
`#include` directives whose spelling does not match the file on disk - for example
`src/public/tier0/logging.h` includes `"color.h"` while the file is `Color.h`. On Windows that is a
warning (`-Wnonportable-include-path`); on Linux it is a fatal error.

Every run scans the sources again - no list is cached, so a file added later is covered - and builds
a shadow include tree under `<IntDir>/include-shadow` that carries the missing spellings as symlinks.
Each include directory inside the repository is replaced on the search path by its counterpart in
that tree, and the directories holding a corrected spelling are appended to it. The tree is rebuilt
from scratch each time, so an entry for a header that has been renamed or removed cannot survive. A directory of the shadow tree
mirrors the whole directory it stands for, because the compiler looks up the siblings of a header
next to the header itself and would otherwise lose the ones whose spelling is already correct.

Nothing tracked by git is modified, and no `#include` directive is rewritten. The shadow tree is not
a report: it is what the preprocessor is given to search, which is why it has to exist as files, and
it lives in the project's intermediate directory, which git ignores. Every entry in it is a symlink
to the real header, so no header content is copied.

## If something goes wrong

- **The CRT or SDK headers are not found.** The script found no usable tree. Create one with `xwin`
  as above, or point `XWIN=` at an existing one; the include directories and library paths are then
  composed from it. `--dry-run` prints the response files it wrote, which show what was passed.
- **A symbol defined by the engine is unresolved.** The engine libraries live in `src/lib/common`
  and `src/lib/public`; the projects list them and the script passes both directories as
  `/LIBPATH:`. Compare with the Windows build if a library is missing.
- **A library is reported as unreadable.** It is not in a format the chosen linker can consume -
  most often the `msvc` variant of the video libraries instead of the `clang_cl` one, or LLVM
  bitcode from a different major version. Check the condition on the `ItemGroup` that names it.
- **A project setting is reported as not handled.** That is the script saying it does not know what
  to do with it, rather than quietly building something different. `--check-conditions` shows which
  groups were taken from which configuration.
