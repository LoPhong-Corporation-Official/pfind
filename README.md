# pfind

A small, fast, portable C++20 command-line file search tool.

## Features

- Recursive search with `*`, `?`, `[abc]`, `[a-z]`, `[!abc]` wildcards and `\` escapes.
- **Full UTF-8 support**: case-insensitive matching for Vietnamese, Latin, Greek and Cyrillic;
  `?` matches one character (not one byte); Unicode paths work on Windows, Linux and macOS.
- `--ignore-accents`: `tai lieu` finds `Tài liệu`, `da nang` finds `Đà Nẵng` (also works with
  decomposed/NFD names produced by macOS).
- **Content search** (`--grep`): finds text inside files, prints `path:line: text`, skips binary files,
  honours `--ignore-accents` and case options. `-E` makes the text a regular expression,
  `-A/-B/-C` show surrounding lines, and file contents are scanned on several threads
  (`-j`), with results always in the same order as a single-threaded run.
- **Regex file names** (`-r`): `pfind . -r '^test_.*\.(cpp|hpp)$'` (ECMAScript, `std::regex`).
- Multiple patterns, exclusions (`-x node_modules`), depth limit, size and modification-time filters.
- Sorting, `--json` (JSON Lines), `--print0` (for `xargs -0`) and `--count` output modes.
- **`.gitignore` support** (`--gitignore`): reads `.gitignore` files in every directory, the ones in parent
  directories inside a git repository and `.git/info/exclude`; never enters `.git`. Results were checked
  against `git ls-files --exclude-standard` and are identical. `--ignore-file .ignore` adds other ignore
  file names, `--skip-hidden` skips dot-files.
- **Match highlighting** on terminals for `--grep` (also with `--ignore-accents`, `-E` and Unicode text).
- **Search and replace** (`--replace`): previews every change as a diff by default and writes only with
  `--apply` (atomic, keeps permissions, line endings and symlinks; `--backup` keeps `.bak` copies).
  Works with `--ignore-accents`, and with `-E` you can use `$1`, `$2`, `$&` in the replacement.
- **Shell completion** for bash, zsh and PowerShell: `pfind --completion <shell>`.
- Coloured output on terminals (`--color auto|always|never`, `NO_COLOR` respected).
- Results go to stdout, summary and warnings go to stderr, so pipes stay clean.
- Permission errors, unreadable entries and symlink loops are reported as warnings and never stop the search.
- Grep-style exit codes: `0` matches found, `1` no matches, `2` error.
- Unit tests (`ctest`) and a GitHub Actions workflow for Linux, macOS and Windows.

## Install shell completion

```bash
# bash
pfind --completion bash > ~/.local/share/bash-completion/completions/pfind
# zsh (any directory in $fpath, then restart zsh)
pfind --completion zsh > "${fpath[1]}/_pfind"
```

```powershell
# PowerShell (add to $PROFILE to keep it)
pfind --completion powershell | Out-String | Invoke-Expression
```

Release archives already contain these scripts in `completions/`.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows with Visual Studio:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

CMake options: `-DPFIND_BUILD_TESTS=OFF`, `-DPFIND_WARNINGS_AS_ERRORS=ON`.

## Usage

```text
pfind <path> [pattern ...] [options]
```

```text
pfind .
pfind . '*.cpp' '*.hpp'
pfind C:\Projects '*.h' --details --sort size --reverse
pfind ~/Documents 'bao cao*' --ignore-accents --modified-within 7d
pfind . '*.log' --min-size 10M -x node_modules -x .git
pfind . '*.tmp' --print0 | xargs -0 rm
pfind . --type dir --max-depth 2
pfind src '*.cpp' --grep TODO                 # lines containing TODO
pfind . '*.log' -g 'error|fatal' -E -l        # only file names, regex content search
pfind src '*.cpp' -g TODO -C 2 -j 4           # 2 lines of context, 4 scan threads
pfind . --gitignore -g 'fixme' -l              # search only files git would not ignore
pfind src '*.cpp' -g oldName --replace newName          # preview the change
pfind src '*.cpp' -g oldName --replace newName --apply --backup
pfind . -g '(\\w+)@old.com' -E --replace '$1@new.com' --apply
pfind . -r '^test_.*\.(cpp|hpp)$'             # regex on file names
```

Quote patterns so your shell does not expand them first.

## Options

```text
Matching
  -p, --pattern <pattern>       Search pattern (repeatable; extra positional args work too)
  -x, --exclude <pattern>       Skip matching files and directories (repeatable)
  -i, --case-insensitive        Case-insensitive matching (default)
  -s, --case-sensitive          Case-sensitive matching
      --ignore-accents          Ignore diacritics
  -r, --regex                   File name patterns and --exclude are regular expressions
      --full-path               Match the path relative to <path> ('/' separators) instead of the name

Content search
  -g, --grep <text>             Only files containing the text; prints path:line: text
  -E, --grep-regex              Treat the --grep text as a regular expression
  -l, --files-with-matches      With --grep, print only file names
      --max-lines <n>           With --grep, at most n matching lines per file
  -A, --after-context <n>       With --grep, also show n lines after each match
  -B, --before-context <n>      With --grep, also show n lines before each match
  -C, --context <n>             Both of the above
  -j, --jobs <n>                Content-scan threads (default: automatic, at most 8)

Replace (preview unless --apply)
      --replace <text>          Replace every --grep match; with -E, $1 $2 ... and $& are available
      --apply                   Write the changes
      --backup                  Keep <file>.bak copies of changed files

Other
      --completion <shell>      Print a completion script (bash, zsh, powershell)

Filtering
      --type <file|dir|all>     Kind of entry to report (default: file)
      --all                     Same as --type all
  -d, --max-depth <n>           Descend at most n levels (1 = direct children)
      --min-size <size>         Minimum file size (10K, 5M, 2G ...)
      --max-size <size>         Maximum file size
      --modified-within <time>  Changed within 30m, 12h, 7d, 2w ...
      --max-results <n>         Stop after n matches
      --follow-symlinks         Follow directory symlinks (loops are detected)
      --gitignore               Skip what .gitignore files exclude (also parents inside a git repo,
                                .git/info/exclude); never enters .git
      --ignore-file <name>      Also read ignore files with this name in every directory (repeatable)
      --skip-hidden             Skip files and directories whose name starts with '.'

Output
      --details                 Show size and modification time
      --color <auto|always|never>  Colour the output (default auto: only on a terminal)
      --sort <name|size|time>   Sort results
      --reverse                 Reverse the sort order
      --json                    One JSON object per line
      --print0                  NUL-separated paths
  -c, --count                   Print only the number of matches
  -q, --quiet                   No summary or warnings
      --verbose                 Print the effective search settings
  -h, --help / -v, --version
```

## Architecture

```text
main ── CLIParser ──> SearchOptions + OutputOptions
                          │
                          v
                    SearchEngine ──> Matcher (Wildcard) ──> Utf8 (decode / fold / strip accents)
                          │
                          v
                      FileInfo ──> callback ──> console (plain / JSON / NUL)
```

- `Utf8` — UTF-8 decoding, simple case folding, accent removal (`src/AccentTable.inc` is generated by
  `tools/gen_accent_table.py`).
- `Wildcard` — compiled `Matcher`; linear-memory, no exponential backtracking.
- `Completion` — option table and generators for the shell completion scripts.
- `Replace` — safe search-and-replace of one file (preview, atomic write, backup).
- `IgnoreRules` — `.gitignore` parser/matcher (literal and `*.ext` fast paths, memoised glob for the rest).
- `SearchEngine` — explicit directory stack (no recursion limit), per-directory error reporting,
  ancestor-based symlink loop detection.
- `Format` — size/duration parsing, time formatting, JSON escaping.

## Notes

- Regex mode uses `std::regex`: case-insensitivity is ASCII-only there (use wildcards for full Unicode case
  folding), and very long lines are truncated to 64 KB before matching.
- `--gitignore` follows git's syntax (`*`, `?`, `[...]`, `**`, `!`, anchoring, directory-only rules; last
  matching rule wins; a file inside an ignored directory cannot be re-included). Not read: the global
  `core.excludesFile`; matching is always case-sensitive; a symlink to a directory counts as a directory.
- `--replace` rewrites whole files through a temporary file in the same directory, so it needs write
  access to that directory (otherwise the file is reported and left untouched). Binary files, files over
  256 MB and (in regex mode) lines over 64 KB are skipped. Literal replacements insert the text verbatim,
  also for case-insensitive hits. Hard links are not preserved. Without `--apply` nothing is written.
- Highlighting is not available for `--grep-regex` combined with `--ignore-accents` (the line is still shown).
- Context lines never count towards `--max-lines`; with `-l` they are ignored.
- Content search treats a file as binary if it has a NUL byte in its first 8 KB, and gives up on files with
  lines longer than 1 MB.

## Releasing

Update the version in `CMakeLists.txt`, commit, then push a tag `vX.Y.Z` that matches it:

```bash
git tag v1.6.0 && git push origin v1.6.0
```

`.github/workflows/release.yml` builds Linux (x86_64), macOS (universal) and Windows (x86_64), runs the
tests, packages each build with `tools/package.sh` and publishes a GitHub release with the archives and
`SHA256SUMS`. A tag that does not match `CMakeLists.txt` stops the workflow. Running the workflow by hand
(Actions > Release > Run workflow) builds everything without publishing. To add Linux ARM, uncomment the
`linux-arm64` entry in the matrix. Linux binaries need glibc 2.35 or newer.

## Changes in 1.6

- `--replace` / `--apply` / `--backup`, `--completion bash|zsh|powershell`.
- Release workflow, `tools/package.sh`, `tools/check-version.sh`, `tools/check-ps-syntax.ps1`.
- The option table behind the completion scripts is tested against the real parser and `--help`.

## Changes in 1.5

- `--gitignore`, `--ignore-file <name>`, `--skip-hidden` (new `IgnoreRules` module).
- Match highlighting in `--grep` output on terminals.
- The search root is normalised (`a/../b/` becomes `b`), so printed paths no longer contain `..`.
- 232 automated checks, also run under ASan, UBSan and ThreadSanitizer.

## Changes in 1.4

- **Faster content search:** files are read in 256 KB chunks and split into lines without copying;
  plain-text searches use a direct substring search (byte-wise for ASCII text, Unicode-aware otherwise);
  Unicode normalisation reuses its buffers and accent removal is a table lookup. On a 330 MB ASCII data
  set a single-threaded `--grep` went from about 4.4 s to about 0.7 s; accent-insensitive search on
  Vietnamese text improved by roughly a third.
- **Colour output** (`--color`): paths, line numbers and separators on terminals; JSON, `--print0` and
  `--count` never contain escape codes. On Windows the console's ANSI mode is enabled automatically.
- More tests (chunk boundaries, CRLF, empty and over-long files, fast paths versus the general matcher).

## Changes in 1.3

Added `-A/-B/-C` context lines (grep-style output: `path:3: match`, `path-2- context`, `--` between
groups) and multi-threaded content scanning (`-j/--jobs`). Files are scanned in batches of 128 and
reported in traversal order, so output is identical for every `--jobs` value. Directory walking itself
stays single-threaded.

## Changes in 1.2

Added `--regex`, `--grep` / `--grep-regex` / `--files-with-matches` / `--max-lines`.

## Changes from 1.0 to 1.1

- Behaviour changes: the header/summary now go to stderr; exit code is 0/1/2 (previously always 0 or 1/2);
  `--details` also prints the modification time; `Wildcard.hpp` gains `Matcher` (old `wildcardMatch` kept).
- Bug fixes: unreadable directories were silently skipped instead of reported; an inaccessible root
  returned exit code 0; ASCII-only case folding broke non-English file names; `std::string` conversion of
  Unicode paths could throw on Windows.
