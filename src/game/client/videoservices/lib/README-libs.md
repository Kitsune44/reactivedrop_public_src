# lib - video libraries (MSVC + clang-cl)

```
lib\win32\                   shared by MSVC and clang-cl (native, no LTO):
                             dsound.lib, dxguid.lib, ogg.lib, opus.lib, vorbis.lib
lib\win32\msvc\release\      MSVC Release: vpx_mt.lib/.pdb, webm_mt.lib/.pdb
lib\win32\msvc\debug\        MSVC Debug:   vpx_mtd.lib/.pdb, webm_mtd.lib/.pdb
lib\win32\clang_cl\release\  clang-cl Release: vpx_mt.lib, webm_mt.lib   (LLVM bitcode)
lib\win32\clang_cl\debug\    clang-cl Debug:   vpx_mtd.lib, webm_mtd.lib (native COFF + CodeView)
```

`lib\win32` holds only what is shared by both toolsets and all configurations (native
third-party libraries). Anything that depends on the compiler is split into `msvc` and `clang_cl`.

## Contents of clang_cl

| File | Built with | Runtime | Objects |
|---|---|---|---|
| `win32\clang_cl\release\webm_mt.lib` | libwebm, clang-cl 22 + full `-flto` | `/MT` | 7/7 = LLVM bitcode (LTO) |
| `win32\clang_cl\release\vpx_mt.lib` | libvpx, clang-cl 22 + `-flto` | `/MT` | 97/97 = LLVM bitcode (LTO) |
| `win32\clang_cl\debug\webm_mtd.lib` | libwebm, clang-cl 22, no LTO | `/MTd` | native COFF + CodeView |
| `win32\clang_cl\debug\vpx_mtd.lib` | libvpx, clang-cl 22, no LTO | `/MTd` | native COFF + CodeView |

There is no separate library PDB for the Release variants: with LTO the debug info travels
inside the bitcode and materializes in the consumer PDB (e.g. `client.pdb`). That is the
clang-cl equivalent of `webm_mt.pdb`.

Note: MSBuild does not pass `/GL` to clang-cl in these configurations; LTO is requested
explicitly with `-flto=thin` in `clang_cl.props`.

## How the project selects libraries

`swarm_sdk_client.vcxproj` uses toolset-conditional item groups (`.filters` intentionally has
no entries for the clang-cl libraries):

```
<!-- shared (both toolsets): public + third-party video, native, no LTO -->
<!-- MSVC -->
<ItemGroup Condition="'$(Configuration)|$(Platform)'=='Debug|Win32'   And '$(PlatformToolset)'!='ClangCL'"> ... lib\win32\msvc\debug\...   </ItemGroup>
<ItemGroup Condition="'$(Configuration)|$(Platform)'=='Release|Win32' And '$(PlatformToolset)'!='ClangCL'"> ... lib\win32\msvc\release\... </ItemGroup>
<ItemGroup Condition="'$(Configuration)|$(Platform)'=='Profile|Win32' And '$(PlatformToolset)'!='ClangCL'"> ... lib\win32\msvc\release\... </ItemGroup>
<!-- ClangCL -->
<ItemGroup Condition="'$(PlatformToolset)'=='ClangCL' And '$(Configuration)|$(Platform)'=='Debug|Win32'">   ... lib\win32\clang_cl\debug\...   </ItemGroup>
<ItemGroup Condition="'$(PlatformToolset)'=='ClangCL' And '$(Configuration)|$(Platform)'=='Release|Win32'"> ... lib\win32\clang_cl\release\... </ItemGroup>
<ItemGroup Condition="'$(PlatformToolset)'=='ClangCL' And '$(Configuration)|$(Platform)'=='Profile|Win32'"> ... lib\win32\clang_cl\release\... </ItemGroup>
```

The ClangCL groups point at `lib\win32\clang_cl`, which contains bitcode, so the ClangCL build
links with `lld-link` (see `clang_cl.props` in src/, next to the solution). MSVC keeps using `link.exe` and
`lib\win32\msvc`.

## LTO

Thin LTO (`-flto=thin`) is the default for the game translation units, as the counterpart of `/GL` on the MSVC side, because full `-flto` corrupted data
the engine reads from the DLL (startup crash: "Prop DT_ParticleSystem/(null) has an invalid
element count for a non-array").

Disable it for a quick iteration with /p:WholeProgramOptimization=false.

The libraries above stay bitcode regardless, so lld-link still runs LTO inside them.