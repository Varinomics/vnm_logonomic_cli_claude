# Running the usage tests

The standalone package build registers the ordinary tests by default.
The large-data tier retains the history-over-512-MiB and million-message regressions.
The ordinary usage-cache test runs the remaining cache and adapter contracts.
Large-data tests are opt-in because they create large histories and filesystem
fixtures. Keep the full-size boundaries when exercising this tier.

To include the large-data tier, configure the existing standalone build with:

```sh
cmake -S . -B <build-dir> -DLOGONOMIC_CLI_ADDON_LARGE_DATA_TESTS=ON
```

Build through the platform's normal build workflow, then run the complete suite:

```sh
ctest --test-dir <build-dir> -C Release --output-on-failure
```

With that option enabled, add `-L large-data` to the CTest command to run only
the large-data tier, or add `-LE large-data` to run only the ordinary tier.
The embedded package build does not register these standalone test suites.

## Windows cache-path release gate

Before the next Windows Free CLI bundle containing this provider, qualify the
captured provider and SDK revisions with the actual LLVM-MinGW Free toolchain
and with MSVC. Follow the owning
[Free official CLI bundle build procedure](https://github.com/Varinomics/logonomic/blob/master/docs/software_lock_release.md#free-official-cli-bundle),
including its compiler, queue, runtime-linking and source-capture requirements.
Use separate standalone Release build trees with `BUILD_TESTING=ON` and the
same captured `LOGONOMIC_CLI_ADDON_SDK_SOURCE`. Build all targets through that
platform's normal workflow before running the gate. An MSVC result does not
qualify the LLVM-MinGW package.

Use the CTest executable recorded as `CMAKE_CTEST_COMMAND` in each build's
`CMakeCache.txt`. Run the following selection in each tree, replacing the
executable and build-directory placeholders with those configured paths:

```powershell
$ctest = '<configured-ctest.exe>'
$buildDirectory = '<standalone-build-directory>'
$pathTests = '^claude_(usage_cache_tests|native_path_tests|native_resume_path_tests|native_export_path_tests|resume_filesystem_tests)$'
& $ctest --test-dir $buildDirectory -C Release -N -R $pathTests
& $ctest --test-dir $buildDirectory -C Release -R $pathTests --no-tests=error --output-on-failure --output-log (Join-Path $buildDirectory 'claude-windows-paths.log')
if ($LASTEXITCODE -ne 0) { throw 'Claude Windows path qualification failed.' }
```

The inventory must contain all five entries below, and every entry must pass.
A skipped filesystem fixture or an absent entry leaves that part unqualified;
CTest's successful exit alone does not establish that it ran.

| Entry | Required coverage |
|---|---|
| `claude_usage_cache_tests` | Existing cache and packaged-adapter contracts. |
| `claude_native_path_tests` | Exact extended drive/UNC spelling; ordinary dot-segment normalization; short, 259/260-character and nested long transcripts; long profile; initial scan, accepted-cache replay, index creation, unchanged index reuse, changed-source update and replay of that updated cache; stable source/record identities and exact totals. |
| `claude_native_resume_path_tests` | Supported npm resume inspection and preparation at long paths, plus denied-read versus absent-source results. |
| `claude_native_export_path_tests` | Packaged export from long transcript and profile paths, preserving conversation text. |
| `claude_resume_filesystem_tests` | Existing filesystem-error distinctions. |

The second scan and later index reads are essential: a first scan can succeed
while opening its saved cache subsequently fails. The correction in
[`c420e027`](https://github.com/Varinomics/vnm_logonomic_cli_claude/commit/c420e0276f2187a0110fb1322fa0146b4a61e79f)
preserves already-extended I/O paths before lexical normalization. Retain this
correction or a separately qualified equivalent in the captured provider source.

The existing `--resume-unc` and `--export-unc` modes are additional manual
checks; they are not registered CTest entries and do not test UNC usage scans.
For a machine with the required local administrative share available, take the
test executable and package paths from
`ctest --test-dir <build-dir> -C Release --show-only=json-v1`, then invoke that
same `claude_native_path_tests` executable with the package path and each UNC
mode. Record an unavailable share as untested rather than as a passing result.

Retain the provider and SDK commit IDs, compiler versions, configure arguments,
CMake cache, exact package generation digest and the two CTest logs with the
release evidence. Attribute any UNC results separately. The commit above records
the author's successful native LLVM-MinGW and MSVC checks; that historical result
does not identify the package bytes of a later release.

