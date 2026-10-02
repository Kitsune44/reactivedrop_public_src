#!/usr/bin/env bash
#
# Cross-build the ASRD game DLLs on Linux for Windows (Win32, i686-pc-windows-msvc).
#
#     ./crossbuild.sh                 # everything, Release
#     ./crossbuild.sh Debug           # one configuration
#     ./crossbuild.sh Release client  # one project
#     ./crossbuild.sh Debug server ai_behavior_follow.cpp   # one file (smoke test)
#
# Options:
#     -h, --help              this text
#     --dry-run               print the command lines and stop, build nothing
#     --check-conditions      audit which ItemDefinitionGroups are taken from the projects
#
# Nothing has to be configured. The script finds the repository (its own directory), the LLVM
# binaries and the xwin tree by itself, and reads the same .vcxproj files MSBuild reads, so the
# compiler, linker and library command lines are the ones a Windows build uses. The fixes that
# Visual Studio's ClangCL toolset applies to those settings (/Z7 instead of /Zi, no /MP, no
# /RTC1, /DEBUG:FULL) are reproduced - see the note above read_project.
#
# Output goes where a Windows build puts it: the OutDir and IntDir of the project, both listed in
# .gitignore. Nothing outside those directories is written, and nothing in the repository is
# modified.
#
# Case sensitivity. Windows ignores the case of a file name, Linux does not, and this repository
# has about 1600 include directives whose spelling does not match the file on disk (warnings on
# Windows, fatal errors on Linux). Every run scans the sources again - no cached list, so a file
# added later is covered - and builds a shadow include tree under <IntDir>/include-shadow with the
# missing spellings as symlinks. Each include directory inside the repository is replaced on the
# search path by its counterpart in that tree, and the directories holding a corrected spelling are
# appended to it. The tree is rebuilt from scratch on every run, so entries for files that no longer
# exist cannot survive.
#
# Not done on purpose: the PreBuild/PreLink/PostBuild events of the projects (Windows batch scripts
# that copy the DLLs into a game installation). Copy the DLLs yourself if you need them there.
#
# Environment overrides, all optional: LLVM (binaries), XWIN (winsysroot), OUT (scratch directory
# for the generated response files), JOBS (compile parallelism), PYTHON (default python3).

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)     # the src directory, next to the solution
SRC=$(cd "$HERE/.." && pwd)                            # the repository root
PROPS=$HERE/clang_cl.props                             # next to the solution, not in the root
TOOLSET_PROPS=$HERE/asrd_toolset_dirs.props

DRY_RUN=0
CHECK_CONDITIONS=0

usage() {
  awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print } else { exit } }' "${BASH_SOURCE[0]}"
}

die() { echo "crossbuild: $*" >&2; exit 1; }

# ---------------------------------------------------------------- arguments --------------------

positional=()
for arg in "$@"; do
  case "$arg" in
    -h|--help)           usage; exit 0 ;;
    --dry-run)           DRY_RUN=1 ;;
    --check-conditions)  CHECK_CONDITIONS=1 ;;
    -*)                  die "unknown option: $arg (try --help)" ;;
    *)                   positional+=("$arg") ;;
  esac
done

CONFIG=${positional[0]:-Release}
PROJECT=${positional[1]:-all}
ONE=${positional[2]:-}

case "$CONFIG" in
  Debug|Release|Profile) ;;
  *) die "unknown configuration: $CONFIG (Debug, Release or Profile)" ;;
esac

OUT=${OUT:-${TMPDIR:-/tmp}/asrd-crossbuild-$CONFIG}
JOBS=${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}
PYTHON=${PYTHON:-python3}

# The scratch directory is removed recursively per project, so refuse anything that is not a
# clearly specific path.
case "$OUT" in
  ""|/|"$SRC"|"$SRC"/*) die "refusing to use OUT=$OUT" ;;
esac

# ---------------------------------------------------------------- toolchain --------------------

# clang-cl and lld-link have to come from one installation, and that installation has to be able to
# read the bitcode of the vendored video libraries. Where it lives is the package manager business,
# so the candidates are collected from the host - PATH, llvm-config, the prefixes the distributions
# and the ports use - and the version decides, not the directory name. A toolchain of another major
# version is kept as a fallback, because Debug links native COFF and a project without bitcode
# libraries does not care; what settles it either way is the check against the real libraries below.
find_llvm() {
  local dir major fallback=
  if [ -n "${LLVM:-}" ]; then
    if [ -x "${LLVM}/clang-cl" ] && [ -x "${LLVM}/lld-link" ]; then
      echo "$LLVM"; return 0
    fi
  fi
  for dir in \
      "$(dirname "$(command -v clang-cl 2>/dev/null)" 2>/dev/null)" \
      $(llvm-config --bindir 2>/dev/null) \
      $(llvm-config-22 --bindir 2>/dev/null) \
      $(ls -d /usr/lib/llvm*/bin /usr/lib/llvm/*/bin /usr/lib64/llvm*/bin \
               /usr/lib/x86_64-linux-gnu/llvm*/bin /opt/llvm*/bin /opt/homebrew/opt/llvm*/bin \
               /usr/local/opt/llvm*/bin /usr/local/llvm*/bin /mingw64/bin /clang64/bin \
               2>/dev/null) \
      /usr/bin /usr/local/bin; do
    [ -n "$dir" ] || continue
    [ -x "$dir/clang-cl" ] && [ -x "$dir/lld-link" ] || continue
    major=$("$dir/clang-cl" --version 2>/dev/null | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p' | head -1)
    case "$major" in
      22|23) echo "$dir"; return 0 ;;
      *) [ -n "$fallback" ] || fallback=$dir ;;
    esac
  done
  [ -n "$fallback" ] || return 1
  echo "$fallback"
}

# The CRT and the Windows SDK, in the layout the compile and link steps actually need. Any directory
# that has that layout is accepted, whatever it is called and wherever it was put.
find_xwin() {
  local dir
  for dir in ${XWIN:-} "$HOME/.xwin-cache/splat" "$HOME/xwin" /opt/xwin /usr/local/xwin \
             /usr/lib/xwin /usr/share/xwin; do
    [ -n "$dir" ] || continue
    if [ -d "$dir/crt/include" ] && [ -d "$dir/sdk/include/um" ] && [ -d "$dir/sdk/lib/um/x86" ]; then
      echo "$dir"; return 0
    fi
  done
  return 1
}

command -v "$PYTHON" >/dev/null || die "$PYTHON is required (set PYTHON=/path/to/python3)"
[ -f "$PROPS" ] || die "cannot find $PROPS - the src directory of the repository was expected"
[ -f "$TOOLSET_PROPS" ] || die "cannot find $TOOLSET_PROPS"

LLVM=$(find_llvm) || die "clang-cl and lld-link not found; install LLVM 22 (apt.llvm.org) or set LLVM=/path/to/bin"
XWIN=$(find_xwin) || die "no Windows CRT/SDK tree found; create one with xwin (set XWIN=/path/to/splat)"

CC=$LLVM/clang-cl
LINK=$LLVM/lld-link

version=$("$CC" --version | head -1)
major=$(printf '%s' "$version" | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p')
if [ -n "$major" ] && [ "$major" -ge 22 ] && [ "$major" -le 23 ]; then
  :
else
  echo "crossbuild: warning: $version" >&2
  echo "crossbuild: the vendored video libraries are LLVM 22 bitcode; a different major version" >&2
  echo "crossbuild: may fail to read them. Set LLVM=/path/to/llvm-22/bin if both are installed." >&2
fi

echo "repository : $SRC"
echo "toolchain  : $version"
echo "crt/sdk    : $XWIN"
echo "output     : the OutDir/IntDir of each project (ignored by git); scratch: $OUT"
echo

# ------------------------------------------------- keep MSBuild's settings ---------------------
# The settings below are the ones Visual Studio's ClangCL toolset ends up passing. It rewrites the
# project values in Microsoft.Cpp.ClangCl.Common.targets, target ClangClFixupCompileOptions
# ("adjust settings which clang-cl does not support"):
#
#   ProgramDatabase (/Zi) and EditAndContinue (/ZI)  ->  OldStyle (/Z7)
#       clang has no edit-and-continue and would emit no debug information for /ZI
#   MultiProcessorCompilation                        ->  cleared, so /MP is never passed
#   BasicRuntimeChecks                               ->  cleared, so /RTC1 is never passed
#   GenerateDebugInformation                         ->  DebugFull (/DEBUG:FULL)
#
# The Python below reads the project and writes, per project and configuration:
#
#   paths          PROJDIR, OUTDIR, INTDIR (expanded, forward slashes)
#   sources.txt    translation units, relative to the project directory
#   clflags.rsp    compiler switches, one response-file token per line
#   linkflags.rsp  linker switches
#   libs.rsp       library inputs
#
# Response files are used instead of shell variables on purpose: they remove every question of
# quoting and word splitting, and both clang-cl and lld-link accept them.

read_project() {
  local projfile="$1" metadir="$2" repo="$3" mode="${4:-build}"
  "$PYTHON" - "$projfile" "$CONFIG" "$PROPS" "$metadir" "$repo" "$mode" "$XWIN" <<'PY'
import os, re, sys
import xml.etree.ElementTree as ET

projfile, config, propsfile, metadir, repo, mode, xwindir = sys.argv[1:8]
projdir = os.path.dirname(os.path.abspath(projfile))
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
warned = set()

def warn(message):
    """Diverging from MSBuild silently is the one failure this script must not have."""
    if message not in warned:
        warned.add(message)
        print('  warning: ' + message, file=sys.stderr)

# Switches MSBuild derives from a project setting. A value that is not listed here is reported:
# a new value in a project would otherwise change the build without anybody noticing.
SIMPLE = {
    ('ClCompile', 'Optimization'): {'Disabled': '/Od', 'MinSpace': '/O1', 'MaxSpeed': '/O2',
                                    'Full': '/Ox'},
    ('ClCompile', 'RuntimeLibrary'): {'MultiThreaded': '/MT', 'MultiThreadedDebug': '/MTd',
                                      'MultiThreadedDLL': '/MD', 'MultiThreadedDebugDLL': '/MDd'},
    ('ClCompile', 'RuntimeTypeInfo'): {'true': '/GR', 'false': '/GR-'},
    ('ClCompile', 'BufferSecurityCheck'): {'true': '/GS', 'false': '/GS-'},
    ('ClCompile', 'StringPooling'): {'true': '/GF', 'false': '/GF-'},
    ('ClCompile', 'FunctionLevelLinking'): {'true': '/Gy', 'false': '/Gy-'},
    ('ClCompile', 'ExceptionHandling'): {'Sync': '/EHsc', 'SyncCThrow': '/EHs', 'Async': '/EHa',
                                          'false': '/EHs-c-'},
    ('ClCompile', 'WarningLevel'): {'Level1': '/W1', 'Level2': '/W2', 'Level3': '/W3',
                                     'Level4': '/W4', 'EnableAllWarnings': '/Wall',
                                     'TurnOffAllWarnings': '/W0'},
    ('ClCompile', 'TreatWarningAsError'): {'true': '/WX', 'false': '/WX-'},
    ('ClCompile', 'DebugInformationFormat'): {'OldStyle': '/Z7', 'ProgramDatabase': '/Z7',
                                              'EditAndContinue': '/Z7', 'None': ''},
    ('ClCompile', 'FloatingPointModel'): {'Precise': '/fp:precise', 'Strict': '/fp:strict',
                                          'Fast': '/fp:fast'},
    ('ClCompile', 'EnableEnhancedInstructionSet'): {'NoExtensions': '/arch:IA32',
                                                    'StreamingSIMDExtensions': '/arch:SSE',
                                                    'StreamingSIMDExtensions2': '/arch:SSE2',
                                                    'NotSet': '/arch:SSE2'},
    ('ClCompile', 'PrecompiledHeader'): {'NotUsing': '/Y-', 'Use': '', 'Create': ''},
    ('ClCompile', 'CompileAs'): {'CompileAsCpp': '/TP', 'CompileAsC': '/TC', 'Default': ''},
    ('ClCompile', 'LanguageStandard'): {'stdcpplatest': '/std:c++latest',
                                        'stdcpp20': '/std:c++20', 'stdcpp17': '/std:c++17',
                                        'stdcpp14': '/std:c++14', 'Default': ''},
    ('ClCompile', 'InlineFunctionExpansion'): {'Disabled': '/Ob0', 'OnlyExplicitInline': '/Ob1',
                                               'AnySuitable': '/Ob2', 'Default': ''},
    ('ClCompile', 'IntrinsicFunctions'): {'true': '/Oi', 'false': '/Oi-'},
    ('ClCompile', 'FavorSizeOrSpeed'): {'Speed': '/Ot', 'Size': '/Os', 'Neither': ''},
    ('ClCompile', 'OmitFramePointers'): {'true': '/Oy', 'false': '/Oy-'},
    ('ClCompile', 'IntelJCCErratum'): {'true': '/QIntel-jcc-erratum', 'false': ''},
    ('ClCompile', 'SuppressStartupBanner'): {'true': '/nologo', 'false': ''},
    ('ClCompile', 'CallingConvention'): {'Cdecl': '/Gd', 'FastCall': '/Gr', 'StdCall': '/Gz',
                                         'VectorCall': '/Gv'},
    ('Link', 'SubSystem'): {'Windows': '/SUBSYSTEM:WINDOWS', 'Console': '/SUBSYSTEM:CONSOLE'},
    ('Link', 'OptimizeReferences'): {'true': '/OPT:REF', 'false': '/OPT:NOREF'},
    ('Link', 'EnableCOMDATFolding'): {'true': '/OPT:ICF', 'false': '/OPT:NOICF'},
    ('Link', 'GenerateDebugInformation'): {'true': '/DEBUG:FULL', 'false': '/DEBUG:NONE'},
    ('Link', 'RandomizedBaseAddress'): {'true': '/DYNAMICBASE', 'false': '/DYNAMICBASE:NO'},
    ('Link', 'ImageHasSafeExceptionHandlers'): {'true': '/SAFESEH', 'false': '/SAFESEH:NO'},
    ('Link', 'LargeAddressAware'): {'true': '/LARGEADDRESSAWARE', 'false': '/LARGEADDRESSAWARE:NO'},
    ('Link', 'TargetMachine'): {'MachineX86': '/MACHINE:X86', 'MachineX64': '/MACHINE:X64'},
}
# Settings handled somewhere else, or not passed to clang-cl at all. Listing them keeps the
# "not handled" warning meaningful: it fires only for something genuinely new.
KNOWN_ELSEWHERE = {
    # applied explicitly further down
    ('ClCompile', 'AdditionalOptions'), ('ClCompile', 'AdditionalIncludeDirectories'),
    ('ClCompile', 'PreprocessorDefinitions'),
    ('ClCompile', 'ObjectFileName'),                 # /Fo, once per translation unit
    ('ClCompile', 'ProgramDataBaseFileName'),        # /Fd: /Z7 keeps the debug info in the object
    ('ClCompile', 'PrecompiledHeaderFile'), ('ClCompile', 'PrecompiledHeaderOutputFile'),
    ('Link', 'DataExecutionPrevention'),             # -> /NXCOMPAT or /NXCOMPAT:NO, below
    ('Link', 'GenerateManifest'),                    # -> /MANIFEST:NO, below
    ('Link', 'LinkIncremental'),                     # -> /INCREMENTAL[:NO], below
    ('Link', 'AdditionalOptions'), ('Link', 'AdditionalDependencies'),
    ('Link', 'AdditionalLibraryDirectories'), ('Link', 'IgnoreSpecificDefaultLibraries'),
    ('Link', 'OutputFile'), ('Link', 'ImportLibrary'), ('Link', 'ProgramDatabaseFile'),
    ('Link', 'ManifestFile'),
    ('Link', 'LinkTimeCodeGeneration'),              # -flto=thin comes from clang_cl.props
    # rewritten by the ClangCL toolset (target ClangClFixupCompileOptions)
    ('ClCompile', 'WholeProgramOptimization'), ('ClCompile', 'MultiProcessorCompilation'),
    ('ClCompile', 'BasicRuntimeChecks'),
    # not passed by the ClangCL toolset
    ('ClCompile', 'AssemblerListingLocation'), ('ClCompile', 'BrowseInformation'),
    ('ClCompile', 'BrowseInformationFile'), ('ClCompile', 'ErrorReporting'),
    ('ClCompile', 'GenerateXMLDocumentationFiles'),
    ('ClCompile', 'ForceConformanceInForLoopScope'), ('ClCompile', 'SDLCheck'),
    ('ClCompile', 'FloatingPointExceptions'),        # the ClangCL toolset does not pass it either
    ('ClCompile', 'DisableSpecificWarnings'),        # never reaches clang-cl - see the comment
    ('Link', 'ErrorReporting'), ('Link', 'LinkErrorReporting'), ('Link', 'GenerateMapFile'),
    ('Link', 'MapFileName'), ('Link', 'ShowProgress'), ('Link', 'BaseAddress'),
    # SetChecksum (/RELEASE) and SuppressStartupBanner (/NOLOGO) are set in the projects, but the
    # ClangCL link task does not pass them; the same holds for /MAP, /TLBID, /ILK, /LTCG and
    # /LTCGOUT. They are left out to match the ClangCL build.
    ('Link', 'SetChecksum'), ('Link', 'SuppressStartupBanner'),
}

def group_applies(condition):
    """Whether an ItemDefinitionGroup applies to this build.

    This is a rule about the condition text, not a general MSBuild evaluator: these projects only
    use conditions of the form "'$(Configuration)|$(Platform)'=='Debug|Win32'",
    "'$(PlatformToolset)'=='ClangCL'" and combinations of the two. A condition naming the
    PlatformToolset must name ClangCL; one naming a configuration must name ours; a condition that
    is platform-specific must be for Win32. A condition that names none of these applies as well,
    which is what a toolset-only group means. --check-conditions prints every decision.
    """
    if not condition:
        return True
    # '$(PlatformToolset)'!='ClangCL' excludes a group from this build. The condition mentions
    # ClangCL, so the negation is tested before the mention is: otherwise the group counts as
    # applicable and the MSVC variants of the video libraries reach a link that cannot read them.
    if re.search(r"'\$\(\s*PlatformToolset\s*\)\s*'\s*!=\s*'ClangCL'", condition):
        return False
    if 'PlatformToolset' in condition and 'ClangCL' not in condition:
        return False
    if 'Configuration' in condition and config not in condition:
        return False
    if re.search(r'\b(ARM|ARM64|x64)\b', condition) and 'Win32' not in condition:
        return False
    return True

def element_applies(condition):
    """Element-level conditions. Only WholeProgramOptimization is used in these projects: it
    guards the /GL counterpart, so -flto=thin belongs to Release and Profile only."""
    if not condition:
        return True
    if 'WholeProgramOptimization' in condition:
        return config in ('Release', 'Profile')
    warn('unknown element condition "%s" - ignored' % condition)
    return False

def collect(root, tag, label):
    merged = {}
    for g in root.findall('m:ItemDefinitionGroup', NS):
        condition = g.get('Condition', '')
        applies = group_applies(condition)
        if mode == 'conditions':
            print('  %-9s %-52s %s' % (label, (condition or '(no condition)')[:52],
                                       'taken' if applies else 'skipped'))
        if not applies:
            continue
        t = g.find('m:' + tag, NS)
        if t is None:
            continue
        for child in t:
            if not element_applies(child.get('Condition', '')):
                continue
            name = child.tag.split('}')[-1]
            value = (child.text or '').strip()
            # AdditionalOptions appears more than once in one section (clang_cl.props keeps the
            # error limit and the LTO switch apart); every other setting is unique.
            if name == 'AdditionalOptions' and merged.get(name):
                merged[name] += ' ' + value
            else:
                if (tag, name) not in SIMPLE and (tag, name) not in KNOWN_ELSEWHERE and value:
                    warn('setting %s.%s is not handled and will be ignored' % (tag, name))
                merged[name] = value
    return merged

def switches(tag, values):
    out = []
    for name, value in values.items():
        if (tag, name) not in SIMPLE:
            continue
        if value in SIMPLE[(tag, name)]:
            mapped = SIMPLE[(tag, name)][value]
            if mapped:
                out.append(mapped)
        elif value:
            warn('%s.%s = "%s" has no mapping' % (tag, name, value))
    return out

def split_list(value):
    return [p.strip() for p in re.split(r'[;\n]', value or '') if p.strip() and '%(' not in p]

def resolve(p):
    return p if os.path.isabs(p) else os.path.normpath(os.path.join(projdir, p))

def options(value):
    value = re.sub(r'%\([A-Za-z]+\)', '', value or '')
    return [p for p in value.split() if p]

def project_properties(root):
    values = {}
    for g in root.findall('m:PropertyGroup', NS):
        if not group_applies(g.get('Condition', '')):
            continue
        for child in g:
            values[child.tag.split('}')[-1]] = (child.text or '').strip()
    return values

def expand_msbuild(value, intdir_value=''):
    """Expand the properties these paths use, so the layout matches the Windows build."""
    if not value:
        return value
    value = value.replace('$(ProjectDir)', projdir + os.sep)
    value = value.replace('$(Configuration)', config)
    value = value.replace('$(Platform)', 'Win32')
    value = value.replace('$(AsrdToolsetDir)', 'clang_cl')
    if intdir_value:
        value = value.replace('$(IntDir)', intdir_value + os.sep)
    value = os.path.normpath(value)
    return value if os.path.isabs(value) else os.path.normpath(os.path.join(projdir, value))

def slash(p):
    """Forward slashes: clang-cl and lld-link accept them everywhere, and they survive every shell
    and every response-file parser without an escape character in sight."""
    return p.replace('\\', '/') if p else p

proj = ET.parse(projfile).getroot()
props = ET.parse(propsfile).getroot()

if mode == 'conditions':
    print('  %-9s %-52s %s' % ('source', 'condition', 'decision'))
    collect(proj, 'ClCompile', 'project')
    collect(props, 'ClCompile', 'props')
    collect(proj, 'Link', 'project')
    collect(props, 'Link', 'props')
    sys.exit(0)

cl, props_cl = collect(proj, 'ClCompile', 'project'), collect(props, 'ClCompile', 'props')
ln, props_ln = collect(proj, 'Link', 'project'), collect(props, 'Link', 'props')

# Visual Studio's own property sheets declare defaults for settings the projects are silent about,
# because a project only stores what differs from the default. Two of them reach the command line
# (Microsoft.Cl.Common.props, both guarded by 'Condition="%(...)==''"'):
#     SuppressStartupBanner = true    ->  /nologo
#     CallingConvention     = Cdecl   ->  /Gd
# Filling them in here keeps the switch list complete without hard-coding it in the shell part.
for name, default in (('SuppressStartupBanner', 'true'), ('CallingConvention', 'Cdecl'),
                             ('TreatWarningAsError', 'false')):
    cl.setdefault(name, default)

propset = project_properties(props)
propset.update(project_properties(proj))

# Where a setting lives depends on the page it came from in Visual Studio: the same value is item
# metadata inside an ItemDefinitionGroup, or a plain property in a PropertyGroup
# (Link.DataExecutionPrevention is written as metadata, Link.GenerateManifest as a property).
# Reading only one of the two silently misses the other, so both are merged into one view per tool,
# item metadata winning, the way the Visual Studio property sheets feed a property into item
# metadata.
cl_eff = {**propset, **cl}
ln_eff = {**propset, **ln}

# Every name that reaches the merged views and is not covered by one of the tables below is reported,
# so a setting added later - in whichever section - cannot change the build unnoticed.
def known(section, name):
    """Whether a setting is covered: mapped to a switch, or deliberately not passed."""
    return (section, name) in SIMPLE or (section, name) in KNOWN_ELSEWHERE


PROJECT_LEVEL = {'OutDir', 'IntDir', 'TargetName', 'TargetExt', 'TargetDir', 'TargetPath',
                 'ProjectName', 'ProjectDir', 'Configuration', 'Platform', 'PlatformToolset',
                 'ConfigurationType', 'CharacterSet', 'SolutionDir', 'SolutionName',
                 'MSBuildProjectName', 'MSBuildProjectFile', 'MSBuildThisFileDirectory',
                 'VCInstallDir', 'WindowsSdkDir', 'VisualStudioVersion'}
# Names that belong to the build system rather than to the compiler or linker command line.
PLUMBING = {'ProjectGuid', 'VCToolsVersion', 'WindowsTargetPlatformVersion', '_ProjectFileVersion',
            'LinkToolExe', 'LinkStatus', 'CodeAnalysisRuleSet', 'IgnoreImportLibrary',
            'PreBuildEvent', 'PreLinkEvent', 'PostBuildEvent', 'PreBuildEventUseInBuild',
            'PreLinkEventUseInBuild', 'PostBuildEventUseInBuild', 'LinkTimeCodeGenerationUseInBuild'}
UNKNOWN = set()
for section, values in (('ClCompile', cl), ('Link', ln)):
    for name in sorted(values):
        if not known(section, name):
            UNKNOWN.add('%s.%s' % (section, name))
for name in sorted(propset):                       # properties Visual wrote instead of metadata
    if not known('ClCompile', name) and not known('Link', name):
        UNKNOWN.add(name)
for name in sorted(n for n in UNKNOWN if n.split('.')[-1] not in PROJECT_LEVEL
                   and n.split('.')[-1] not in PLUMBING):
    warn('%s is not handled and may change the build' % name)

proj_outdir = slash(expand_msbuild(propset.get('OutDir', '')) or os.path.join(projdir, config))
proj_intdir = slash(expand_msbuild(propset.get('IntDir', ''))
                    or os.path.join(proj_outdir, 'intermediate'))

# A source can be listed by more than one ItemGroup: the projects repeat the same list per
# configuration, and only the groups whose condition holds apply. Each unit is compiled once, from
# those groups.
sources = []
applicable = set()
for _group in proj.findall('.//m:ItemGroup', NS):
    if not group_applies(_group.get('Condition', '')):
        continue
    for _item in _group.findall('m:ClCompile', NS):
        applicable.add(id(_item))
        _inc = _item.get('Include')
        if not _inc:
            continue
        _rel = _inc.replace('\\', '/')
        if _rel not in sources:
            sources.append(_rel)

# A translation unit can carry settings of its own, which override the shared ones for that file
# alone. The precompiled header is the one that actually differs per file in these projects, so it
# is collected here and turned into flags per unit further down. Nothing about it is decided by
# this script: a project that starts using a precompiled header, or one file that stops using it,
# is enough for the flags to change.
per_file = {}
for item in proj.findall('.//m:ClCompile', NS):
    include = item.get('Include')
    if not include or id(item) not in applicable:
        continue
    rel = include.replace('\\', '/')
    values = {}
    for child in item:
        if not group_applies(child.get('Condition', '')):
            continue
        name = child.tag.split('}')[-1]
        value = (child.text or '').strip()
        if name in ('PrecompiledHeader', 'PrecompiledHeaderFile', 'PrecompiledHeaderOutputFile'):
            values[name] = value
            continue
        shared = cl.get(name, '')
        if value and value != shared:
            warn('%s sets %s to "%s" while the project says "%s"' % (rel, name, value, shared))
    if values:
        per_file[rel] = values

# Effective precompiled header flags for one translation unit: the file wins over the project, and
# the compiler default (/Y-) applies when neither names one. The output path is expanded exactly
# like the include directories are, so it lands in the intermediate directory of this build.
pch_project = cl_eff.get('PrecompiledHeader', '')
pch_header_project = cl_eff.get('PrecompiledHeaderFile', '')
pch_output_project = cl_eff.get('PrecompiledHeaderOutputFile', '')

def effective_pch(rel):
    """(flags, creates) for one unit."""
    own = per_file.get(rel, {})
    mode = own.get('PrecompiledHeader', pch_project) or 'NotUsing'
    if mode in ('', 'NotUsing'):
        return ['/Y-'], False
    header = own.get('PrecompiledHeaderFile', pch_header_project)
    output = own.get('PrecompiledHeaderOutputFile', pch_output_project)
    if not output:
        # MSBuild falls back to $(IntDir)$(TargetName).pch when the project names no output file.
        # /Yc writes that file and /Yu reads it, so both sides have to arrive at the same path.
        _target = propset.get('TargetName', '') or os.path.splitext(os.path.basename(projfile))[0]
        output = os.path.join(proj_intdir, _target + '.pch')
    flags = []
    if header:
        flags.append(('/Yc' if mode == 'Create' else '/Yu') + header)
    else:
        warn('%s uses a precompiled header but names no header file' % rel)
    if output:
        flags.append('/Fp' + slash(expand_msbuild(output.replace('$(TargetName)', propset.get('TargetName', '')), proj_intdir)))
    return flags, mode == 'Create'

per_file_flags = {}
pch_create = []
for rel in sources:
    flags, creates = effective_pch(rel)
    per_file_flags[rel] = flags
    if creates:
        pch_create.append(rel)
pch_used = sum(1 for flags in per_file_flags.values() if flags and flags[0] != '/Y-')
print('  precompiled header: %d units use it, %d of them create it, %d compile with /Y-'
      % (pch_used, len(pch_create), len(sources) - pch_used))

# /std: is not a base switch: it comes from LanguageStandard through the table below, so a project
# that changes its standard produces exactly one such switch. A base /std: would add a second,
# contradictory one whose effect depends on which the compiler reads last.
# The base switches are the only ones no project setting owns: /c (compile only) and --target. Every
# setting a project can own comes from the table, which carries the effective default where the
# project is silent (see the setdefault calls above).
# --target: on a 64-bit host clang-cl defaults to x86_64, and the projects are Win32 only. Without
# this every /arch: switch is rejected as invalid for 64-bit and the CRT headers are looked up for
# the wrong architecture.
clflags = ['/c', '--target=i686-pc-windows-msvc']
clflags += switches('ClCompile', cl_eff)
clflags += ['/D' + d for d in split_list(cl_eff.get('PreprocessorDefinitions', ''))]
# DisableSpecificWarnings is not mapped: the ClangCL toolset does not pass it, and the ids are MSVC
# warning numbers that clang does not know.
clflags += ['/I' + slash(d) for d in (resolve(expand_msbuild(x, proj_intdir))
                                      for x in split_list(cl_eff.get('AdditionalIncludeDirectories', '')))]
clflags += options(cl_eff.get('AdditionalOptions', ''))
clflags += options(props_cl.get('AdditionalOptions', ''))
if propset.get('CharacterSet') == 'MultiByte':
    clflags.append('/D_MBCS')
elif propset.get('CharacterSet') == 'Unicode':
    clflags += ['/D_UNICODE', '/DUNICODE']
if propset.get('ConfigurationType', '').startswith('DynamicLibrary'):
    clflags.append('/D_WINDLL')
# The precompiled header comes from the project and is turned into per-unit flags: the units that
# use it get /Yu, the one that builds it gets /Yc, and the rest get /Y-. Nothing is forced here.
# /diagnostics:column is the one switch the compile task adds on its own: it is neither a
# project property nor declared in Microsoft.Cl.Common.props, so there is nowhere to read it from.
clflags.append('/diagnostics:column')
# /MP is deliberately absent: the ClangCL toolset clears MultiProcessorCompilation so that clang-cl
# is never handed /MP, which it would answer with "argument unused during compilation".

libs = []
for g in proj.findall('.//m:ItemGroup', NS):
    condition = g.get('Condition', '')
    if condition and not group_applies(condition):
        continue
    for lib in g.findall('m:Library', NS):
        inc = lib.get('Include')
        if inc:
            p = inc.replace('\\', '/')
            libs.append(p if ('/' not in p and not os.path.isabs(p)) else slash(resolve(p)))
libs = list(dict.fromkeys(libs))

# The classic Win32 import libraries that every Visual C++ project gets, whether it names them or
# not. They are resolved through the CRT/SDK library paths.
CHECKED_LIBS = ['kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib',
                'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib',
                'odbc32.lib', 'odbccp32.lib']
have = {os.path.basename(x).lower() for x in libs}
libs += [l for l in CHECKED_LIBS if l.lower() not in have]

# Libraries named as a plain string (winmm.lib, ws2_32.lib, discord-rpc.lib,
# legacy_stdio_definitions.lib, ...), in the project and in the props.
for extra in (ln_eff.get('AdditionalDependencies', ''), props_ln.get('AdditionalDependencies', '')):
    for name in split_list(extra):
        p = name.replace('\\', '/')
        entry = p if ('/' not in p and not os.path.isabs(p)) else slash(resolve(p))
        if entry not in libs:
            libs.append(entry)

linkflags = switches('Link', ln_eff)
linkflags += options(ln_eff.get('AdditionalOptions', ''))
linkflags += options(props_ln.get('AdditionalOptions', ''))
linkflags += ['/NODEFAULTLIB:' + x for x in split_list(ln_eff.get('IgnoreSpecificDefaultLibraries', ''))]
linkflags += ['/LIBPATH:' + slash(d) for d in (resolve(expand_msbuild(x, proj_intdir))
                                               for x in split_list(ln_eff.get('AdditionalLibraryDirectories', '')))]
if ln_eff.get('GenerateManifest', 'true') == 'false':
    linkflags.append('/MANIFEST:NO')
if ln_eff.get('RandomizedBaseAddress', 'true') != 'false':
    linkflags.append('/DYNAMICBASE')
default_incremental = 'true' if config == 'Debug' else 'false'
linkflags.append('/INCREMENTAL' if ln_eff.get('LinkIncremental', default_incremental) == 'true'
                 else '/INCREMENTAL:NO')
# Every linker setting is read from the project's ItemDefinitionGroup for Link. A setting the
# project leaves out means the tool's default: the generator that wrote these projects omits
# anything equal to it, so an absent DataExecutionPrevention is DEP on, not an unknown.
linkflags.append('/NXCOMPAT:NO' if ln_eff.get('DataExecutionPrevention', 'true') == 'false'
                 else '/NXCOMPAT')
linkflags = [f for f in linkflags if not f.startswith('/SOURCELINK')]   # lld-link has no such option
linkflags = list(dict.fromkeys(linkflags))

# Case sensitivity: the shadow tree built by build_case_shadow goes in front of every real include
# directory inside the repository, so the spellings the sources use are found. Done here because
# these paths are already normalised for this platform.
shadow = os.path.join(proj_intdir.rstrip('/'), 'include-shadow')
if repo:
    repo_norm = os.path.normpath(repo)
    rebuilt = []
    for f in clflags:
        if f.startswith('/I') and os.path.isabs(f[2:]):
            real = os.path.normpath(f[2:])
            if real.lower().startswith(repo_norm.lower() + os.sep) and os.path.isdir(real):
                rebuilt.append('/I' + slash(os.path.join(shadow, os.path.relpath(real, repo_norm))))
        rebuilt.append(f)
    clflags = rebuilt

def write_response(path, tokens):
    """One token per line. MSBuild-style response files split on whitespace, so a token that
    contains whitespace has to be quoted; a quotation mark inside a token cannot be expressed and
    is refused instead of being written wrong."""
    with open(path, 'w', newline='\n') as handle:
        for token in tokens:
            if '"' in token:
                print('response file: cannot express a quote in %r' % token, file=sys.stderr)
                sys.exit(1)
            handle.write('"%s"\n' % token if re.search(r'\s', token) else token + '\n')

os.makedirs(metadir, exist_ok=True)
with open(os.path.join(metadir, 'paths'), 'w', newline='\n') as handle:
    # rstrip('/'): $(OutDir) and $(IntDir) in the projects end with a separator, and the shell adds
    # its own when it builds /OUT:$outdir/$name.dll - without this the paths come out as clang_cl//.
    handle.write('PROJDIR=' + slash(projdir).rstrip('/') + '\n')
    handle.write('OUTDIR=' + proj_outdir.rstrip('/') + '\n')
    handle.write('INTDIR=' + proj_intdir.rstrip('/') + '\n')
with open(os.path.join(metadir, 'sources.txt'), 'w', newline='\n') as handle:
    handle.write('\n'.join(sources) + '\n')
clflags = list(dict.fromkeys(clflags))

# The CRT and the Windows SDK headers. On Windows these arrive through the INCLUDE environment
# variable that vcvars sets, which is why MSBuild never puts them on the command line and why the
# .tlog files do not show them either. On Linux nothing sets INCLUDE, so they are taken from the
# xwin tree. -imsvc rather than /I: the SDK headers are not ours and -Wnonportable-include-path fires
# on nearly every one of them otherwise. The path is joined to the switch (-imsvc<dir>), which is the
# spelling the option requires.
for _sub in ('crt/include', 'sdk/include/ucrt', 'sdk/include/um', 'sdk/include/shared'):
    _dir = os.path.join(xwindir, _sub)
    if os.path.isdir(_dir):
        clflags += ['-imsvc' + slash(_dir)]
    else:
        print('  warning: %s is missing in %s' % (_sub, xwindir))

# The same for the link step: the CRT and SDK import libraries, which a Windows build finds through
# the LIB environment variable that vcvars sets.
for _sub in ('crt/lib/x86', 'sdk/lib/um/x86', 'sdk/lib/ucrt/x86'):
    _dir = os.path.join(xwindir, _sub)
    if os.path.isdir(_dir):
        linkflags += ['/LIBPATH:' + slash(_dir)]
    else:
        print('  warning: %s is missing in %s' % (_sub, xwindir))
write_response(os.path.join(metadir, 'clflags.rsp'), clflags)

# One response file per translation unit, holding the flags that unit alone gets. The shared file
# carries no precompiled header switch, so a unit that opts out gets /Y- without affecting others.
per_file_dir = os.path.join(metadir, 'perfile')
os.makedirs(per_file_dir, exist_ok=True)
for rel in sources:
    obj_name = re.sub(r'[/\\:]', '_', rel)
    write_response(os.path.join(per_file_dir, obj_name + '.rsp'), per_file_flags[rel])
with open(os.path.join(metadir, 'pch-create.txt'), 'w', newline='\n') as handle:
    for rel in pch_create:
        handle.write(rel + '\n')
write_response(os.path.join(metadir, 'linkflags.rsp'), linkflags)
write_response(os.path.join(metadir, 'libs.rsp'), libs)

clflags = list(dict.fromkeys(clflags))
real_dirs = [x for x in clflags if x.startswith('/I') and 'include-shadow' not in x]
mirrors = [x for x in clflags if x.startswith('/I') and 'include-shadow' in x]
print('  %d sources, %d include dirs (+%d shadow mirrors), %d libraries' %
      (len(sources), len(real_dirs), len(mirrors), len(libs)))
PY
}

# ------------------------------------------------- case-sensitive file names --------------------
# Scans the sources for #include directives whose spelling does not match the file on disk and
# writes the missing spellings as symlinks into a shadow tree. No list is cached and the tree is
# rebuilt from scratch on every run: a stale entry would point at a header that has been renamed
# or removed, which is exactly the kind of silent failure this is meant to prevent. Nothing is
# written into the repository.
build_case_shadow() {
  local shadow="$1" rsp="$2"
  "$PYTHON" - "$SRC" "$shadow" "$rsp" <<'PYCASE'
import os, re, subprocess, sys

root, shadow, rsp = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
# Both forms are matched. A quoted include is looked up next to the including file first and then
# along the include directories; an angled include along the include directories only. That difference
# decides where an entry has to go, so neither form may be skipped.
INCLUDE = re.compile(r'^\s*#\s*include\s+([<"])([^>"]+)[>"]')
SUFFIXES = ('.h', '.hpp', '.hxx', '.c', '.cpp', '.cxx', '.inc', '.inl', '.ipp')

try:
    listing = subprocess.run(['git', '-C', root, 'ls-files'], capture_output=True, text=True,
                             check=True).stdout
    files = [f for f in listing.split('\n') if f]
except Exception as error:
    print('  warning: git ls-files failed (%s); walking the tree instead, which also picks up' % error,
          file=sys.stderr)
    print('  warning: generated files. Install git for an exact list.', file=sys.stderr)
    files = []
    for base, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d != '.git']
        for name in names:
            files.append(os.path.relpath(os.path.join(base, name), root).replace('\\', '/'))

exact_files = set(files)
exact_dirs = set()
for rel in files:
    directory = os.path.dirname(rel)
    while directory:
        exact_dirs.add(directory)
        directory = os.path.dirname(directory)

# The include directories of the project, read from the response file that was built out of the
# project settings: every /I entry that is not one of the shadow mirrors. The builder needs them to
# tell an include the compiler finds anyway from one it does not.
include_dirs = []
if os.path.exists(rsp):
    with open(rsp, encoding='utf-8', errors='ignore') as handle:
        for line in handle:
            line = line.strip()
            if line.startswith('/I') and 'include-shadow' not in line:
                candidate = line[2:].replace('\\', '/')
                if not os.path.isabs(candidate):
                    candidate = os.path.join(root, candidate)
                include_dirs.append(os.path.normpath(candidate))


def reachable(includer, wanted, angled):
    """True when the compiler finds the include exactly as written, case included."""
    if not angled and os.path.normpath(os.path.join(os.path.dirname(includer), wanted)) in exact_files:
        return True
    for directory in include_dirs:
        if os.path.normpath(os.path.join(directory, wanted)) in exact_files:
            return True
    return False


by_name = {}
for rel in files:
    by_name.setdefault(os.path.basename(rel).lower(), []).append(rel)

bad = []
for rel in files:
    if not rel.lower().endswith(SUFFIXES):
        continue
    try:
        with open(os.path.join(root, rel), encoding='utf-8', errors='ignore') as handle:
            lines = handle.read().splitlines()
    except OSError:
        continue
    for number, line in enumerate(lines, 1):
        match = INCLUDE.match(line)
        if not match:
            continue
        angled = match.group(1) == '<'
        wanted = match.group(2).replace('\\', '/')
        if os.path.isabs(wanted):
            continue
        if wanted.startswith('.'):
            # Relative to the including file: resolve it exactly rather than guess.
            resolved = os.path.normpath(os.path.join(os.path.dirname(rel), wanted))
            if resolved in exact_files or resolved in exact_dirs:
                continue
            bad.append((rel, wanted, resolved))
            continue
        if reachable(rel, wanted, angled):
            continue
        candidates = by_name.get(os.path.basename(wanted).lower(), [])
        if not candidates:
            continue
        # A path-shaped include like fgdlib/EntityDefs.h is matched against its directory as well: the
        # repository holds both src/public/entitydefs.h and src/public/fgdlib/entitydefs.h, and the
        # first file of that name is not the one the path asks for.
        wanted_dir = os.path.dirname(wanted).lower()
        if wanted_dir:
            narrowed = [c for c in candidates if os.path.dirname(c).lower().endswith(wanted_dir)]
            if narrowed:
                candidates = narrowed
        bad.append((rel, wanted, candidates[0]))

def entry_target(real, wanted):
    """Where the entry has to go so that <include dir>/<wanted> resolves: under the include
    directory that holds the real file, spelled the way the include asks for. When the directory
    name itself differs in case - src/public/SoundEmitterSystem holds the file while the include
    says soundemittersystem/ - an entry next to the real file would sit in a directory the
    compiler never asks about. Falls back to the real file own directory."""
    # Absolute on both sides: real comes from git ls-files and is relative to the repository,
    # candidate is built from an include directory and is absolute.
    real_dir = os.path.normpath(os.path.join(root, os.path.dirname(real))).lower()
    wanted_dir = os.path.dirname(wanted)
    for directory in include_dirs:
        candidate = os.path.join(directory, wanted_dir) if wanted_dir else directory
        if os.path.normpath(candidate).lower() == real_dir:
            rel = os.path.relpath(candidate, root)
            return os.path.join(shadow, rel, os.path.basename(wanted))
    return os.path.join(shadow, os.path.dirname(real), os.path.basename(wanted))


made = 0
failed = 0
shadow_dirs = set()
for includer, wanted, real in bad:
    link = os.path.join(root, real)
    targets = [entry_target(real, wanted)]
    for target in dict.fromkeys(targets):
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shadow_dirs.add(os.path.dirname(target))
        if os.path.lexists(target):
            continue
        try:
            os.symlink(link, target)
            made += 1
        except OSError:
            # Symlinks are not available everywhere (Windows without developer mode, some network
            # file systems). Report it rather than dying halfway through the tree.
            failed += 1

# Every directory of the shadow tree replaces the real one on the include path, so it has to hold
# every file of that directory, not only the corrected spellings. The compiler resolves the siblings
# of a header next to the header itself: if it entered through the shadow, a correctly spelled
# sibling with no entry there - IVguiMatInfo.h next to public/vgui/ISurface.h - is not found at all.
for directory in sorted(shadow_dirs):
    real_dir = os.path.relpath(directory, shadow)
    source_dir = os.path.join(root, real_dir)
    if not os.path.isdir(source_dir):
        continue
    for name in sorted(os.listdir(source_dir)):
        source = os.path.join(source_dir, name)
        if not os.path.isfile(source) or os.path.islink(source):
            continue
        target = os.path.join(directory, name)
        if os.path.lexists(target):
            continue
        try:
            os.symlink(source, target)
            made += 1
        except OSError:
            failed += 1

print('  include spelling: %d mismatches, %d symlinks created%s' %
      (len(bad), made, '' if not failed else ' (%d could not be created)' % failed))
PYCASE
}

# ------------------------------------------------- build ---------------------------------------

read_paths() {
  local file="$1"
  PROJDIR=''; OUTDIR=''; INTDIR=''
  local key value
  while IFS='=' read -r key value; do
    case "$key" in
      PROJDIR) PROJDIR=$value ;;
      OUTDIR)  OUTDIR=$value ;;
      INTDIR)  INTDIR=$value ;;
    esac
  done < "$file"
  [ -n "$PROJDIR" ] && [ -n "$OUTDIR" ] && [ -n "$INTDIR" ] ||
    die "$file does not define PROJDIR, OUTDIR and INTDIR"
}

build_one() {
  local name="$1" relproj="$2"
  local projfile=$SRC/$relproj
  local meta=$OUT/$CONFIG/$name
  rm -rf -- "$meta"
  mkdir -p -- "$meta"
  
  echo
  echo "=================================="
  echo " $name ($CONFIG)"
  echo "=================================="

  read_project "$projfile" "$meta" "$SRC" build
  read_paths "$meta/paths"

  local outdir=$OUTDIR intdir=$INTDIR
  local helper=$meta/compile-one.sh
  local shadow=$intdir/include-shadow

  # The flags already point at $shadow; fill it in, from scratch, before compiling.
  rm -rf -- "$shadow"
  build_case_shadow "$shadow" "$meta/clflags.rsp"

    # Which include directory has no shadow mirror. The mirror is what carries the corrected
    # spellings, so a directory without one cannot be reached with a name that differs in case.
    while IFS= read -r _flag; do
        case "$_flag" in /I*include-shadow*) continue ;; esac
        case "$_flag" in /I*) ;; *) continue ;; esac
        _dir=${_flag#/I}
        _rel=${_dir#"$SRC"/}
        [ -d "$intdir/include-shadow/$_rel" ] || echo "  missing shadow for the include directory: $_dir"
    done < "$meta/clflags.rsp"

# A quoted include is looked up next to the including file before the include directories are
# searched, and that file is the real one, not the shadow copy. A sibling whose spelling differs -
# #include "HelperInfo.h" for a file called helperinfo.h - is therefore found only if the directory
# holding the corrected spelling is on the search path itself: those are the shadow directories that
# carry at least one symlink. This has to happen before the source list is piped into xargs: anything
# placed inside that pipeline swallows the list and no compiler runs at all.
"$PYTHON" - "$intdir/include-shadow" "$meta/clflags.rsp" <<'PYCASEDIRS'
import os
import sys

shadow, rsp = sys.argv[1], sys.argv[2]
dirs = []
for root, _dirs, files in os.walk(shadow):
    if any(os.path.islink(os.path.join(root, name)) for name in files):
        dirs.append(root.replace('\\\\', '/'))
if dirs:
    with open(rsp, 'a', newline='\n') as handle:
        for entry in sorted(set(dirs)):
            handle.write('/I' + entry + '\n')
PYCASEDIRS

  if [ "$DRY_RUN" = 1 ]; then
    echo "  dry run - commands that would be used (the helper adds @perfile/<unit>.rsp to the"
    echo "  compile command, which holds the precompiled header flags of that unit):"
    echo "    $helper $CC $meta/clflags.rsp $intdir $PROJDIR <source>"
    echo "    $LINK @$meta/linkflags.rsp /OUT:$outdir/$name.dll /IMPLIB:$outdir/$name.lib \\"
    echo "          /PDB:$outdir/$name.pdb $intdir/*.obj @$meta/libs.rsp"
    return 0
  fi

  # The helper is generated once per project and takes its parameters as arguments.
  cat > "$helper" <<'EOS'
#!/usr/bin/env bash
# Generated by crossbuild.sh. Arguments: clang-cl, response file, object directory, project
# directory, source file. Prints the object path on success, so the caller can count what was built
# without guessing from the file system. On failure it prints one marked block holding only the
# errors, because a project emits tens of thousands of warning lines and the errors would drown.
set -euo pipefail
cc="$1" rsp="$2" objdir="$3" projdir="$4" src="$5"
rel=${src#"$projdir"/}
obj="$objdir/$(printf '%s' "$rel" | tr '/\:' '___').obj"
# The flags that belong to this unit alone, written by the project reader: the precompiled header
# settings differ per file, and a unit that opts out needs /Y- even when the project uses one.
own="$(dirname "$rsp")/perfile/$(printf '%s' "$rel" | tr '/\:' '___').rsp"
log="$obj.log"
args=("$cc" "@$rsp")
if [ -f "$own" ]; then
    args+=("@$own")
fi
if "${args[@]}" /Fo"$obj" "$src" > "$log" 2>&1; then
    rm -f -- "$log"
    printf '%s\n' "$obj"
    exit 0
fi
printf '\n===== FAILED %s =====\n' "$rel" >&2
grep -E 'error:|fatal error:|note:' "$log" | head -40 >&2 || true
printf '===== end %s =====\n' "$rel" >&2
exit 1
EOS
  chmod +x "$helper"

  mkdir -p -- "$outdir" "$intdir"

  if [ -n "$ONE" ]; then
    local src=$ONE
    case "$src" in /*) ;; *) src=$PROJDIR/$src ;; esac
    [ -f "$src" ] || die "no such file: $src"
    echo "  single file: $src"
    "$BASH" "$helper" "$CC" "$meta/clflags.rsp" "$intdir" "$PROJDIR" "$src" >/dev/null
    echo "  ok - the toolchain is wired up correctly"
    return 0
  fi

  local expected built
  expected=$(wc -l < "$meta/sources.txt")
  : > "$meta/built.txt"
  # A unit that creates the precompiled header has to run before the units that read it, and the
  # units that read it must not run while it is being written. The list comes from the project: a
  # project that names no such unit gets an empty file here and this step does nothing.
  if [ -s "$meta/pch-create.txt" ]; then
      while IFS= read -r s; do
          s=${s%$'\r'}
          "$BASH" "$helper" "$CC" "$meta/clflags.rsp" "$intdir" "$PROJDIR" "$PROJDIR/$s" \
              >> "$meta/built.txt" || true
      done < "$meta/pch-create.txt"
  fi
  # NUL separated: paths with spaces survive, and xargs does not treat a backslash as an escape.
  # A failing compilation must not abort the run: the errors are printed, and the count at the end
  # says how many units did not make it.
  if ! while IFS= read -r s; do
         s=${s%$'\r'}
         if [ -s "$meta/pch-create.txt" ] && grep -qxF "$s" "$meta/pch-create.txt"; then
             continue
         fi
         printf '%s\0' "$PROJDIR/$s"
       done < "$meta/sources.txt" |
xargs -0 -r -P "$JOBS" -n 1 "$BASH" "$helper" "$CC" "$meta/clflags.rsp" "$intdir" "$PROJDIR" \
         >> "$meta/built.txt"; then
    :
  fi
  built=$(sort -u "$meta/built.txt" | wc -l)
  if [ "$built" -lt "$expected" ]; then
    die "$name: compiled $built of $expected translation units - see the errors above"
  fi
  echo "  compiled $built of $expected translation units"

  # Which sources produced no object, and how many objects are handed to the linker. Without this
  # a linker error about symbols that should exist says nothing about where they went.
  _missing=0
  while IFS= read -r _s; do
      _s=${_s%$'\r'}
      [ -f "$intdir/$(printf '%s' "$_s" | tr '/\:' '___').obj" ] || {
          echo "  no object for: $_s"
          _missing=$((_missing + 1))
      }
  done < "$meta/sources.txt"
  [ "$_missing" -eq 0 ] && echo "  objects created for all sources ($(wc -l < "$meta/sources.txt"))"
  echo "  linking..."
  # dotglob: an object for a source outside the project directory is named after the relative
  # path, so "../../fgdlib/gamedata.cpp" becomes ".._.._fgdlib_gamedata.cpp.obj" and starts with
  # a dot, which a plain *.obj glob skips.
  shopt -s nullglob dotglob
  local objs=("$intdir"/*.obj)
  echo "  ${#objs[@]} objects will be passed to the linker"
  shopt -u nullglob dotglob
  [ "${#objs[@]}" -gt 0 ] || die "$name: no object files in $intdir, nothing to link"

  # The vendored video libraries exist in two variants - LLVM bitcode from the clang_cl build and
  # MSVC LTCG bitcode from the msvc one - and only the first can be linked here. Rather than assume
  # which one arrived, every library that is about to be linked is offered to llvm-nm, which reads
  # both COFF and bitcode: one it cannot read is one this link could not have used either.
  if [ -x "$LLVM/llvm-nm" ]; then
      local _lib
      while IFS= read -r _lib; do
          case "$_lib" in
              /*.lib|/*.LIB) ;;
              *) continue ;;
          esac
          "$LLVM/llvm-nm" --defined-only "$_lib" > /dev/null 2>&1 && continue
          die "$name: cannot read $_lib - the linker would not have been able to use it."
      done < "$meta/libs.rsp"
  fi
  "$LINK" @$meta/linkflags.rsp \
          /DLL /OUT:"$outdir/$name.dll" /IMPLIB:"$outdir/$name.lib" /PDB:"$outdir/$name.pdb" \
          "${objs[@]}" @$meta/libs.rsp
  [ -f "$outdir/$name.dll" ] || die "$name: the link reported success but $outdir/$name.dll is missing"
  # The DLL is the result of the whole run, so it gets its own block instead of sharing a line with
  # the counters above it.
  echo
  echo "  generated $name.dll"
  echo "  path: $(printf '%s' "$outdir/$name.dll") ($(wc -c < "$outdir/$name.dll") bytes)"
}

# ------------------------------------------------- run -----------------------------------------

if [ "$CHECK_CONDITIONS" = 1 ]; then
  echo "Condition decisions for configuration $CONFIG (Win32, toolset ClangCL):"
  for relproj in src/game/client/swarm_sdk_client.vcxproj \
                 src/game/server/swarm_sdk_server.vcxproj \
                 src/game/missionchooser/swarm_sdk_missionchooser.vcxproj; do
    echo
    echo "=================================="
    echo " $(basename "$relproj")"
    echo "=================================="
    read_project "$SRC/$relproj" "$OUT/$CONFIG/check" "$SRC" conditions
  done
  exit 0
fi

case "$PROJECT" in
  client)         build_one client         src/game/client/swarm_sdk_client.vcxproj ;;
  server)         build_one server         src/game/server/swarm_sdk_server.vcxproj ;;
  missionchooser) build_one missionchooser src/game/missionchooser/swarm_sdk_missionchooser.vcxproj ;;
  all)
    build_one missionchooser src/game/missionchooser/swarm_sdk_missionchooser.vcxproj
    build_one server         src/game/server/swarm_sdk_server.vcxproj
    build_one client         src/game/client/swarm_sdk_client.vcxproj
    ;;
  *) die "unknown project: $PROJECT (client, server, missionchooser or all)" ;;
esac
