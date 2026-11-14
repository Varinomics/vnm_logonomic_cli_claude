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

`claude_resume_preparation_tests` exercises the actual resume producer, including
the distinct failure codes and complete descriptions. The filesystem resume
entry also checks the missing-conversation description.

On Windows, the standalone build also registers `claude_native_path_tests`,
`claude_native_resume_path_tests` and `claude_native_export_path_tests`, which
run the packaged adapter against long and extended-length Windows paths.
`claude_resume_filesystem_tests` returns 77 and prints the reason when its
filesystem fixtures are unavailable on the host; CTest reports that entry as
skipped, and a skip is not a pass. The `claude_native_path_tests` executable
also has manual `--resume-unc` and `--export-unc` modes, which are not
registered with CTest: pass the mode after the package directory that the
registered entries use. These modes need the `\\localhost\<drive>$`
administrative share of the drive that holds the temporary directory.
