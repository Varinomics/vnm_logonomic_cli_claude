# P17 — Claude hooks and fixture portability

Date: 2026-11-02  
Owner: `Varinomics/vnm_logonomic_cli_claude`  
Draft PR: https://github.com/Varinomics/vnm_logonomic_cli_claude/pull/7  
Branch: `work/linux-review-20261102-p17-provider-claude`  
Validated code commit: `8a2028036b6aa7811179d87c74fb7ab530cbc9f8`

## Outcome and continuation

Two narrow changes are implemented and pushed:

1. Select the Unicode CRT startup for the standalone MinGW `claude_fake_cli` fixture.
2. Return from the relay before serializing or launching the helper for ordinary child `PreToolUse` checkpoints that the helper already treats as a no-op.

The changes add four CMake lines, five relay lines and 34 focused regression-test lines. Root context delivery and the attention-tool lifecycle routes remain intact. All five selected Linux CTests have passing evidence; the affected relay/helper tests also passed after restaging the final package. There were no skipped tests.

The PR remains draft because the Windows LLVM-MinGW standalone link/run gate has not executed. That is the next concrete native action. The broader historical claim about shipping Windows hook slowdown remains unproved; this report establishes a bounded Linux synthetic-path measurement and the removal of redundant child work.

| Finding | Current evidence | Disposition | Remaining uncertainty |
|---|---|---|---|
| 42 | Real relay/helper/wrapper/core and authenticated framework transport exercised against a disposable synthetic host; root freshness and child exclusion checked; executable starts traced | Removed one unnecessary child helper launch and its payload serialization/parsing. Retained every root checkpoint and both attention-tool lifecycle routes. | Production host ledger/title/delegation/message-store cost, vendor scheduling and Windows latency were not measured. No uniform tail-latency claim. |
| 133 | Current source still defined only `wmain` for the Windows fixture without MinGW Unicode startup selection; Linux fixture built and all six Linux delivery cases passed | Added target-specific `-municode` under `if(MINGW)`, following the existing bridge/SDK convention | Supported Windows LLVM-MinGW configure/link/run remains pending. No native Windows failure or success is claimed from this Linux work. |

## Finding 133: fixture startup

The baseline `tests/claude_fake_cli.cpp` uses `wmain` under Windows and ordinary `main` only on other platforms. Its CMake target linked `shell32` but did not select the wide MinGW startup. The same provider already applies `-municode` to `claude_launch_bridge`; current SDK command-line tools use that convention too.

The fix applies the option to `claude_fake_cli` only. Keeping `wmain` preserves the fixture's independent comparison of CRT wide arguments and `CommandLineToArgvW`. Embedded package builds return before this standalone test target is registered. The existing MSVC entry-point branch is source-preserved; this task does not claim a new MSVC execution result.

The compiler contract is documented in [GCC's MinGW options](https://gcc.gnu.org/onlinedocs/gcc/Cygwin-and-MinGW-Options.html): `-municode` selects Unicode-capable runtime startup. [CMake's MINGW variable](https://cmake.org/cmake/help/latest/variable/MINGW.html) selects compilers targeting the GNU ABI on Windows.

## Finding 42: causal scope and preserved delivery

The current helper distinguishes root context by **absence of the `agent_id` member**, not by whether its value is nonempty. An ordinary child `PreToolUse` reaches the helper, then returns without context or a lifecycle request. The relay can make that same decision before copying/stringifying the payload and spawning the helper.

The new guard matches that member-presence boundary for JSON input, including an empty string, `null`, `false`, zero, an array and an object. It explicitly excludes `AskUserQuestion` and `ExitPlanMode`, whose child events still require lifecycle handling. Other event kinds continue through the existing relay.

Every ordinary root `PreToolUse` still requests context from the host. There is no throttle, timestamp cache, narrower hook matcher or reused context. The [adopted model-context contract](https://github.com/Varinomics/logonomic/blob/b9e44f6fe131624d48b0338c23295dae67d526bd/sdk/logonomic_cli_addon_sdk/docs/model_context_delivery_1.md) requires root delivery at eligible checkpoints and excludes delivery belonging to the root from subagent hooks. The synthetic measurements do not justify changing that cadence or widening the shared SDK transport.

The 34 added relay-test lines establish:

- Repeated identical root input invokes the helper twice and returns two distinct fresh contexts.
- All seven tested JSON `agent_id` values start no helper for ordinary tool checkpoints. An explicit counter outside the relay prevents its caught spawn exception from creating a false-positive test.
- Child `AskUserQuestion` and `ExitPlanMode` still reach the helper with their payload unchanged.
- Child `UserPromptSubmit` and `PostToolUse` still forward normally.

Node still starts and parses the input JSON for child hooks. The change removes the subsequent copy/stringify, helper startup and helper parsing; it does not claim to eliminate all hook overhead.

## Authenticated experiment

The complete ordinary root path used the unchanged production helper, Logonomic wrapper and core, and framework local-control transport:

`Node relay → claude_helper → vnmctl → logonomic_vnmctl_core → authenticated disposable host`.

The host used `VNM_local_control_session` with a fresh private endpoint/token and a fixture callback registered as `logonomic.app / logonomic.agent_event / request_title_instruction`. The callback checked the complete synthetic session/package/generation binding and returned a fresh marker, or an empty instruction in the quiet condition.

This is real transport authentication with a **synthetic host callback**. It does not execute the production session ledger, one-shot title/delegation state, coordination/message-store work or Claude's own scheduler. No live vendor session or desktop UI was operated.

### Method

- Ubuntu 24.04.3, glibc 2.39, x86-64; GCC 13.3 Release builds; Qt 6.12; Python 3.12.3; native Node 22.23.4.
- Thirty samples and three warmups per condition, with round-robin conditions reversed on alternating rounds.
- Two exact relays across five conditions: root/child with small input, root/child with a 1 MiB command, and quiet root with small input.
- 300 timed hooks plus 30 warmup hooks. p95 uses nearest rank.
- Small JSON inputs were 173 bytes for root and 196 for child. Large inputs were 1,048,717 and 1,048,740 bytes respectively; the command text itself was exactly 1 MiB.
- `hook_ms` ends after the complete response is captured and process exit is confirmed.
- `hook_to_tool_started_ms` continues until a disposable Python tool begins, after the harness consumes the response. This is a synthetic tool-release measurement, not Claude scheduling.
- Process traces ran separately and contributed no tracing overhead to timed samples.

### Complete hook-to-tool results

All values are milliseconds, using the explicit native Node executable.

| Condition | Baseline p50 | Corrected p50 | Baseline p95 | Corrected p95 |
|---|---:|---:|---:|---:|
| Root, fresh context, small | 57.59 | 54.15 | 75.40 | 63.68 |
| Root, fresh context, 1 MiB command | 71.13 | 71.68 | 85.62 | 83.42 |
| Child, small | 45.57 | 41.36 | 53.89 | 47.37 |
| Child, 1 MiB command | 63.35 | 45.63 | 73.02 | 55.15 |
| Root, empty instruction, small | 53.92 | 53.91 | 68.34 | 72.03 |

The differences between child medians are approximately 4.2 ms and 17.7 ms. Matched iteration pairs favored the corrected relay in 24 of 30 small-child cases and all 30 large-child cases.

Root work is unchanged. Its fresh-context differences must not be attributed to a root optimization. The quiet-root median is essentially identical, while its observed p95 increased from 68.34 to 72.03 ms. This small, shared-environment experiment does not establish a universal tail improvement.

### Hook completion alone

These values exclude the subsequent dummy-tool startup.

| Condition | Baseline p50 | Corrected p50 | Baseline p95 | Corrected p95 |
|---|---:|---:|---:|---:|
| Root, fresh context, small | 41.93 | 40.11 | 58.11 | 48.17 |
| Root, fresh context, 1 MiB command | 56.40 | 56.18 | 69.19 | 67.01 |
| Child, small | 30.56 | 26.36 | 37.18 | 33.08 |
| Child, 1 MiB command | 49.03 | 30.63 | 57.20 | 37.25 |
| Root, empty instruction, small | 40.15 | 38.94 | 47.62 | 50.47 |

### Correctness, authentication and starts

Both exact relays passed negative authority probes. A wrong framework token was rejected by the real transport before the callback. Wrong session capabilities and generation nonces were rejected by the fixture's exact binding checks; those probes do not certify the production host's ledger/admission implementation.

The primary run recorded 198 accepted root callbacks, four rejected synthetic-binding callbacks and 202 total callback requests. Wrong-token probes reached no callback. Every fresh-context root instruction carried its expected changing marker; quiet-root calls returned no instruction as requested. Ordinary child calls returned no context and reached no callback.

Separate native process traces confirmed:

| Ordinary checkpoint | Executable starts before | Executable starts after | Host callbacks before / after |
|---|---:|---:|---:|
| Root | 4: Node, helper, wrapper, core | 4: same chain | 1 / 1 |
| Child | 2: Node and helper | 1: Node | 0 / 0 |

The stage probes below each had 30 samples and three warmups. They are independent nested-path measurements, not an additive breakdown; subtracting their percentiles would not produce reliable exclusive stage costs.

| Path | p50 | p95 |
|---|---:|---:|
| Helper with parent-pipe watch → wrapper → core → host | 8.14 ms | 9.33 ms |
| Wrapper → core → host | 6.64 ms | 8.42 ms |
| Core → host | 3.80 ms | 5.63 ms |

### Measurement correction retained in the evidence

The first bounded run used the external SDK's `bin/node` launcher. Process tracing showed that it starts shell/readlink/dirname helpers before reaching the native executable. Both relays used the same launcher, but those extra starts confounded the intended absolute process path.

That complete dataset remains in `hook-benchmark-sdk-launcher.json`, including its worse small-child p95. One corrected run used the explicit native Node ELF, whose path and SHA-256 are recorded in the primary `hook-benchmark.json`. The primary tables above use only that corrected run. The correction addressed observed extra processes; it was not a rerun selected to improve a percentile.

The independent helper/wrapper/core stage samples do not include Node startup and were retained. Their initial combined stage/trace invocation saved the successful stage samples before the launcher-count assertion failed. A separate native-Node trace run completed successfully and is recorded in `hook-traces.json`. Raw traces were discarded after extracting executable counts.

## Validation actually performed

| Check | Actual result and scope |
|---|---|
| Archive/source integrity | Package manifest verified; provider's 106 tracked file blobs matched the selected baseline. Selected current SDK files, wrapper closure and dependency blobs were verified against Git metadata. |
| SDK preflight | Passed with the checksum-verified public SDK in a task-writable extraction. |
| Standalone Release configure | Passed with current explicit Logonomic SDK source, GCC 13.3 and system libcurl 8.5.0. |
| Focused build | `claude_package`, `claude_fake_vnmctl`, `claude_fake_cli`, `claude_initial_prompt_delivery_tests` passed. |
| `claude_initial_prompt_delivery_tests` | Passed all six Linux direct/bash/sh cases, each with and without wake. Stand-in CLI only. |
| `claude_context_helper_tests` | Passed. Component helper-to-adapter context checks. |
| `claude_context_hook_tests` | Passed. Script context-hook contracts. |
| `claude_hook_contract_tests` | Passed initially and after the 34 added regressions. Final run: 0.0334 s. |
| `claude_helper_tests` | Passed in a consistent native process namespace; passed again after final package restaging, 14.3653 s. Existing fake-wrapper component fixture. |
| Final package restage | Passed; 79 indexed files; no C++ compilation was needed for the relay asset change. |
| Final relay identity | Source, staged file, package index and measured candidate all matched SHA-256 `49543fb28d4f5be8208af7e89efa38495e05b1e0e4106705a23f3e62eb9e08cb`, 4,432 bytes. |
| Authenticated experiment | Completed the primary paired run, both-relay negative probes, independent stages, JSON-member-presence checks and separate process traces. Scope and limits above. |
| Diff/review | `git diff --check` passed; an independent read-only review found no blockers in the final code/test diff or corrected measurement interpretation. |

Compiler-invoking configure steps used one queued slot. Focused builds used at most four slots with matching `--parallel 4`; final package staging used one slot/parallel one. Tests ran serially. Effective persistent ccache was verified in Ninja commands and statistics: the initial focused build recorded 83 cacheable cold-cache calls, and the host fixture reused the cache. Owned source stayed outside the external SDK.

Resolved setup failures are retained rather than relabeled as initial passes:

- The first SDK preflight refused a read-only extraction. Copying the already verified kit to task-writable storage resolved that condition.
- The first helper-suite run could not establish its native birth identity because sandbox PIDs and mounted `/proc` disagreed. An isolated diagnostic showed that the helper's own PID had no corresponding stat record. The unchanged test/build passed in the native execution environment with matching PID and `/proc/self`; no ownership check was weakened.
- The initial scratch host-build recipe imposed extra warning-as-error flags on unchanged Logonomic wrapper code. The recipe was corrected to match the owning wrapper target's actual policy, and the focused fixture build passed. No production source was changed for that setup issue.

No private hosted CI, live-user UI, vendor invocation or Windows execution was performed. Passing helper fixtures do not independently prove production host policy or shipping performance.

## Reproducible inputs

Source inputs were resolved once for this task; no moving-master refresh was used to relabel earlier evidence.

| Input | Revision |
|---|---|
| Provider baseline | `821ed4c69530c0327ef5b411d7f8ac436d9bc9ba` |
| First pushed fixture checkpoint | `55ba70069100f79fc7cae8e1604cdcd2bd823a71` |
| Final code | `8a2028036b6aa7811179d87c74fb7ab530cbc9f8` |
| Final code tree | `6f0c149252077062f50be4ed63c822954f06ca56` |
| Logonomic wrapper and CLI SDK | `b9e44f6fe131624d48b0338c23295dae67d526bd` |
| Framework | `d9334145f489a5650fb79a5a7086d8b6618f0c6f` |
| Logger | `9b136ddff29b9b86bfdb8b991f4aec2647be5974` |
| Qt dispatch | `666f6d054b4cf79b39a240370eeb7053d2a07ee3` |
| Fonts | `3a7e37bab54a705ddc08e3de5cc3e4f655256396` |
| CMake support | `66d5ee14f4f69c952daa58a95a4759ad77a636f4` |

External kit: `logonomic-linux-development-sdk-2026.10.23.1`, SHA-256 `56100ede7b00bd7faf06a7b64c57b94106f63e3ba1b77c0bbdf5d61d7ca220c2`. It supplied external binaries/tools; current owned components were compiled from the recorded sources. The standalone provider selected its SDK explicitly, and the fixture selected the current framework control targets and wrapper sources explicitly; no full application build is claimed.

The separate [P17_provider_claude_evidence_2026-11-02.zip](https://chatgpt.com/library?file_id=libfile_899b3d191148819185a70868cc312bb2&account_id=personal) contains the disposable benchmark source, exact reproduction commands, input identities, sanitized raw samples, retained setup failures and final validation logs. Its SHA-256 is `3f31124125f7e0c08f8a5dda3805f34a8f0ad0bc44c9db0dafadd2c6125bbbfa`. The bundle excludes owned repository sources, binaries, the SDK, raw credential-bearing traces and generated attachment secrets. Obtain the two relay revisions from Git as described in its README.

The experimental cross-repository build recipe is retained with that evidence, not added to the provider's maintained build/test surface.

## Native follow-up and next action

| Gate | What remains | Required setup |
|---|---|---|
| Finding 133 | Standalone configure and link of both `claude_fake_cli` and `claude_initial_prompt_delivery_tests`, then run the latter CTest. Record any optional-shell skip individually. | Supported Windows LLVM-MinGW toolchain, current explicit CLI SDK source, queued compiler width matching the build. |
| Windows hook latency | Measure the shipping complete vendor-hook-to-tool boundary if making a Windows responsiveness or performance claim. Preserve root freshness and actual host policy work. | Representative Windows installation and explicitly authorized native fixture/session setup. |
| Production root checkpoint cost | The four-process authenticated path remains. A further change would need evidence from the actual host/vendor boundary and a preserved context-delivery contract. | Separate measured investigation if further optimization is requested. |

Continue from the open task branch/PR, preserving these results. Build both Windows fixture targets explicitly: the delivery-test executable does not itself make the fake CLI a target dependency. Run `ctest --output-on-failure --no-tests=error -R '^claude_initial_prompt_delivery_tests$'` in that native standalone build. Add the actual toolchain and outcomes to the PR; do not promote Linux/source evidence into a Windows pass.

The canonical checkout remains on `master`. The task branch is retained for the named native follow-up; no default-branch merge or force-push was performed.
