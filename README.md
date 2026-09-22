# Logonomic Claude CLI add-on

Standalone C++20 Claude Code provider for Logonomic on Windows and Linux. The
package links only the public Logonomic CLI add-on SDK. It owns launcher
recognition, account profiles, launch preparation, Claude hooks, interactive
resume, structured review and implementation sessions, authentication, quota,
and transcript usage parsing.

The SDK dependency tracks the owned Logonomic `master` branch. Set
`LOGONOMIC_CLI_ADDON_SDK_SOURCE` to a local `sdk/logonomic_cli_addon_sdk` directory
to qualify unpublished coordinated SDK changes. Keep build directories outside
this repository. Windows compilation follows the workspace queued-build and
Visual Studio 2026 policy.

The package target produces an indexed package containing the adapter, attached
process bridge, authenticated hook helper, capability declarations, and immutable
Claude hook plugin. Its output is `<build>/package/logonomic.cli.claude` with a
single-configuration generator, or
`<build>/package/<config>/logonomic.cli.claude` with a multi-configuration generator.
Node must be available in the frozen launcher environment
for hooks and resume; ordinary launch preparation reports its actual reduced
lifecycle disposition when Node is unavailable. Hook responses contain only
instructions returned by the authenticated host helper boundary.

The package recipe proves the Windows/Linux x86-64 compiler target during
configuration. `cmake --install` exports a fresh artifact beneath its install-time
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
to repositories from recorded working directories. No monetary cost is inferred.
The first scan consumes a host-bound native file-cache seed when supplied and
commits a package-owned per-file cache before returning results. The cache
records each transcript's size and modification time; an incremental scan
parses only transcripts whose values changed, while repair and reset parse all
of them. A transcript that changes while it is read keeps its previous evidence
until a later scan reads it at rest. Only a single record is bounded in size,
not a transcript or the history. Deleted files remain represented by their
retained per-message evidence. Historical coverage
remains explicitly incomplete because transcripts do not prove a complete
lifetime history.

Delegated review and implementation use session contract version 3 and keep
native stdin open through initialization, an awaited reconnect/discovery for
each required host stdio server, and a live status/catalog check. The task is
sent only when every requested server is connected and every required raw
action is present, followed by stdin closure. Unsupported or uncorrelated
control replies, missing catalogs/actions and premature model output fail before
task release. This is protocol feature admission, not an upstream version pin.
Native permission decisions, review initialization validation, account binding
and root-result provenance still apply. Reconnected task-server children remain
within the host-owned process lifetime; unrelated servers are not reconnected.
Non-delegated sessions retain their one-shot input behavior.

Contract tests cover launcher and resume restrictions, root/child structured
stream handling, delegated control correlation and catalog admission, ordered
task/EOF delivery, lifecycle correlation, auth and quota interpretation, transcript
deduplication, partial coverage, and hook response admission. Qualification of
the coordinated host file-cache handoff and package persistence is required
before cutover.

The Claude icon derives from Lobe Icons. Its unchanged MIT notice is in
`package/notices/lobe_icons_mit.txt`; `package/provenance/lobe_icons.json`
records the upstream source and SVG viewport normalization.
