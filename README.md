# Logonomic Claude CLI add-on

[![CI](https://github.com/Varinomics/vnm_logonomic_cli_claude/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/Varinomics/vnm_logonomic_cli_claude/actions/workflows/ci.yml)

Standalone C++20 Claude Code provider for Logonomic on Windows, Linux and macOS.
The package depends on the public Logonomic CLI add-on SDK and, outside
Windows, libcurl for its quota helper. It owns launcher
recognition, account profiles, launch preparation, Claude hooks, interactive
resume, structured review and implementation sessions, authentication, quota,
and transcript usage parsing.

The SDK dependency tracks the owned Logonomic `master` branch. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to a local `sdk/logonomic_cli_addon_sdk` directory
to qualify unpublished coordinated SDK changes. Keep build directories outside
this repository. Windows compilation follows the workspace queued-build and
Visual Studio 2026 policy.

Outside Windows, configuration requires the libcurl development files. The Linux
and macOS packages carry libcurl and the rest of their non-system runtime closure
inside the package, and each carried library needs a notice: Linux falls back
to the distribution's `/usr/share/doc/<package>/copyright` for a dpkg-owned
library, otherwise (and always on macOS) set `CLAUDE_RUNTIME_NOTICES_DIRECTORY`
to a directory holding `<library filename>.txt` for each. Linux also needs
`objdump`, and macOS assembly needs Python >=3.9.

The package target produces an indexed package containing the adapter, attached
process bridge, authenticated hook helper, capability declarations, and immutable
Claude hook plugin. Its output is `<build>/package/logonomic.cli.claude` with a
single-configuration generator, or
`<build>/package/<config>/logonomic.cli.claude` with a multi-configuration generator.
Node must be available in the frozen launcher environment
for hooks and resume; ordinary launch preparation reports its actual reduced
lifecycle disposition when Node is unavailable. Hook responses contain only
instructions returned by the authenticated host helper boundary.

The optional `session-wake@1` capability can notify the same live interactive
Claude session through the public [Mods API](https://code.claude.com/docs/en/plugins/mods/api).
It requires Node, writable private profile storage, and a Claude installation
that actually loads both packaged Mods. Installing the package or recognizing a
Claude version does not establish readiness. Older installations, disabled Mods,
missing session registration and unsupported launch forms leave wake unavailable.
Ordinary mailbox delivery remains available independently.

The admission Mod is part of `logonomic-claude-hooks`; the separate
`logonomic-wake-dispatch` plugin declares it as a native dependency. The
dispatcher submits only requests produced by the admission Mod. This separation
lets Claude run the admission hook when the dispatcher calls `$.prompt.submit`;
Claude skips a submitting plugin's own hooks. Disabling the dependency prevents
the dispatcher from loading. Both use a private, per-launch file bridge bound to
the authenticated native session ID and transition nonce. No terminal input,
native inbox wire, resumed second process, or user-attributed prompt is used.

Wake requires explicit live `crossSessionInbound: "accept"` in the user's,
launcher's or managed policy settings. Stricter project/local `hold` or `refuse`
still applies; missing or unrecognized policy leaves wake unavailable. The
package never changes these settings. A busy turn, prompt draft, interrupted
turn, or unknown pause state defers the request. The admission hook checks these
conditions again at native submission. Reloading Mods requires a subsequent
ordinary prompt or completed answer before wake becomes eligible again.
Acceptance means Claude admitted a plugin-attributed notification; it does not
mean the agent read or acknowledged the mailbox. A lost native result is reported
as uncertain and retained against the notification ID to prevent automatic
duplicate submission.

Wake storage is allocated only after an authenticated native session start.
The Mod anchors native process birth identity through a direct packaged helper
before the classic SessionStart relay runs; Node launchers cannot become the
owner. Without that native anchor, no wake directory is allocated. The ordinary
SessionEnd hook removes its binding even when Mods are subsequently disabled;
later preparation reclaims bindings whose recorded owners have exited.
Unverifiable ownership is retained rather than expiring live sessions by age.
After a native root answer completes, the Mod publishes an authenticated
completion event so pending mail is rechecked after its busy state clears;
the earlier classic Stop hook alone cannot establish that readiness.

The package recipe proves the Windows/Linux x86-64 or macOS ARM64 compiler target
during configuration. `cmake --install` exports a fresh artifact beneath its install-time
prefix and preserves executable permissions. An existing per-package destination,
including a dangling symlink, is refused. POSIX `DESTDIR` staging is supported;
nonempty `DESTDIR` with a drive-letter prefix is refused. Export is not atomic:
a failure may leave a partial artifact directory. Choose a fresh destination or
manually remove only that identified disposable export before retrying; never
remove an installed generation this way. The application's package ingestion
owns atomic installation and updates.

Account selection uses the host-admitted canonical profile path. Scans read
finalized assistant messages under that profile's `projects` directory, deduplicate
exact message IDs, retain partial token-category coverage, and attribute tokens
to repositories from recorded working directories.
Each scan commits a package-owned per-file cache before returning results. The cache
records each transcript's size and modification time; an incremental scan
parses only transcripts whose values changed, while repair and reset parse all
of them. Equal size and modification time are taken to mean unchanged content,
the check the pre-cutover scanner used: transcripts are append-only, and repair
rereads every transcript. A transcript's cached messages are written across
lines of about 1 MiB each, so no cache line nears the record bound. A transcript that changes while it is read keeps its previous evidence
until a later scan reads it at rest. Only a single record is bounded in size;
the size and number of transcripts and messages are not. Deleted files remain
represented by their retained per-message evidence. Historical coverage
remains explicitly incomplete because transcripts do not prove a complete
lifetime history.

Hosts may use `usage-incremental@1` with an accepted cache generation to receive
only changed complete session observations. Unchanged scans inspect the saved
source index and filesystem metadata without loading or exporting message
accounting. The first conversion from a monolithic cache publishes its saved
session observations once because that format did not retain resolved repository
allocations; it does not reread unchanged transcripts. `usage-scan@3` continues
to return complete snapshots, including from converted caches. Accepted message
IDs keep their original source and session owner when another transcript copies
them; all copies still contribute conflict evidence.

Usage failures report the operation that failed and a safe explanation. Native
profile paths, transcript contents and credentials are excluded from these
diagnostics. Failed scans do not replace the host's accepted usage cache.

The optional `usage-temporal@1` capability retains strict RFC 3339 instants
from admitted finalized messages before conversation reduction. It allocates
only the existing total-token amount; raw offsets and fractional precision
remain intact for the host's selected display timezone. Missing or conflicting
instants remain an unavailable remainder. The independently versioned
per-message `usage-temporal` cache carrier binds its instant to the exact
canonical message accounting digest, within the selected cache's sealed
account and snapshot. Date-only retained history remains usable without
invented instants, and unknown carriers remain opaque in cache replay.
Temporal capacity withdraws optional evidence while preserving accounting
history and its existing transport limits. Saved daily metrics remain separate.

Saved lifetime and daily totals come from Claude Code's own `stats-cache.json`
in the selected profile, read through `usage-lifetime-summary@1` and
`usage-saved-daily-tokens@1`: the per-model token classes and the dated
per-model totals Claude Code saved, with the digest of the exact file and the
version and date it states. A profile without the file, or a file without a
dated series, reports that capability as unavailable; a malformed part fails
only the capabilities that read it. These totals are reported apart from the
transcript scan and are never added to it.

Delegated review and implementation use session contract version 3 and keep
native stdin open through initialization and live status/catalog checks of
each required host stdio server's initial connection. Pending discovery is
checked at 100 ms intervals for up to 30 seconds after initialization is
acknowledged, subject to the host's process deadline. The task is
sent only when every requested server is connected and every required raw
action is present, followed by stdin closure. Unsupported or uncorrelated
control replies, missing catalogs/actions and premature model output fail before
task release. This is protocol feature admission, not an upstream version pin.
Native permission decisions, review initialization validation, account binding
and root-result provenance still apply. Task-server children remain
within the host-owned process lifetime.
Non-delegated sessions retain their one-shot input behavior.

A structured or delegated implementation keeps the launcher's permission
posture: the launcher's own options, such as `--permission-mode` or
`--allowedTools`, and the user's Claude settings. The package adds no
permission mode and pre-approves only the task's own Task tools
(`--allowedTools mcp__logonomic-task-<task>-<server>__<action>`), so the
headless run denies whatever else the launcher and settings do not grant. An
implementation that edits files needs a launcher that grants edits, for example
with `--permission-mode acceptEdits`. A run whose settings or options remove or
refuse one of its Task tools, for example with a `permissions.deny` rule
matching `mcp__logonomic-task-...`, fails with `task_tools_not_permitted`.

Read-only review likewise keeps the launcher's options, model, settings and
hooks. The package limits the native tools to `Read`, `Glob`, `Grep` and its
own Task tools, and loads only the host's Task MCP configuration. It adds only
the exact Task-tool pre-approvals. It does not select plan mode: current Claude
Code refuses Task calls in that mode even when individually pre-approved.
User deny rules and permission hooks still apply. Review checks the actual
native tool/server surface, including when plugins are configured, and waits
for a successful process exit before returning its authenticated root result.

Both session purposes require a direct launcher whose normalized arguments
match the host's invocation. Review additionally refuses `--tools`,
`--mcp-config` and `--`, including quoted spellings, because they conflict with
its fixed tool ceiling or appended arguments. Use a separate direct launcher
without those options for review; retain permission restrictions with the
launcher's normal settings and `--disallowedTools`.

Interactive launch preparation also accepts the host's
`interactive-initial-prompt@1` member. The prepared invocation starts a new
interactive session whose first user message is the host's text, passed as
Claude's positional prompt; it lets Claude read the directory of the host's
referenced file through `--add-dir` and nothing wider, and the result echoes
the text's SHA-256. Only Claude itself with known interactive options is
prepared this way, never a package runner. Preparation fails `unavailable`
with `initial_prompt_unavailable`, instead of launching without the prompt,
wherever a layer between the host and Claude could change the text: a POSIX
shell on Windows; cmd.exe variable expansion, quotes, operators outside quotes,
a quoted program or its line limit; Windows PowerShell's native argument
passing; or an invocation over the launch-preparation@3 argument bounds. It
also fails where Claude itself would not submit the text as it is: Claude
submits its positional prompt as typed input, so a text that is one word,
starts with `-` or `/`, has whitespace or a non-ASCII character at either end,
names `ultraplan` or holds a paste reference such as `[Pasted text #1]` is
refused, and so is a launcher that selects a main-thread agent with `--agent`,
whose own initial prompt Claude puts first, or passes `--settings` inline JSON,
which can do the same or add a hook. The user's own Claude configuration (a
default agent, hooks, managed settings, plugins, or a settings file a launcher
names) is outside the acknowledgment and acts on the first message as on any
typed one.

Contract tests cover launcher and resume restrictions, root/child structured
stream handling, delegated control correlation and catalog admission, ordered
task/EOF delivery, lifecycle correlation, auth and quota interpretation, transcript
deduplication, partial coverage, hook response admission, and initial-prompt
preparation and refusal. A delivery test runs every prepared initial-prompt
launcher shape against a stand-in CLI that records the arguments it receives.

Quota uses `quota@3` to report every observed limit in provider order. Structured
Claude usage reports take precedence over older named fields and text output,
including an empty collection when the server reports no limits. Limit scopes,
headline selection, percentages and optional resets are preserved; missing
durations remain unknown. Hosts supporting only `quota@2` cannot consume these
observations, while the package's other capabilities remain independent.

The Claude icon derives from Lobe Icons. Its unchanged MIT notice is in
`package/notices/lobe_icons_mit.txt`; `package/provenance/lobe_icons.json`
records the upstream source and SVG viewport normalization.
