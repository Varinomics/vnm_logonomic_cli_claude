# Conversation history inspection

Claude's `SessionStart` hook supplies the native conversation identity and exact
resume record. A `clear` source starts a new conversation; a `resume` source
reopens an existing one. A `compact` source does not create an identity
transition. The host retains departing identities independently of the running
terminal and decides which remembered conversations to display.

## Native execution context snapshots

The optional `execution-context@1` companion publishes the main-session
[`SessionStart`](https://code.claude.com/docs/en/hooks#sessionstart) hook's exact
`cwd` beside its identity transition. A host requests the companion explicitly;
classification responses without that request omit it. The snapshot uses the
same transition nonce as the helper delivery and sequence `0`, with source
`claude.SessionStart`. It describes an absolute local path within the shared
contract's bounds, preserving subdirectories, whitespace and symlink spellings.
It does not infer a containing Git worktree or rewrite immutable provenance.

Missing, malformed or unsupported cwd produces an attributed `unknown` snapshot
with empty directory and environment fields. The native identity and exact
resume record still transition. A transcript path, launch directory, Git root,
`CLAUDE_PROJECT_DIR`, typed command or tool-input directory cannot fill that gap.
Child-session events cannot replace the main identity or cwd. Compaction keeps
the existing activation. Invalid companion negotiation fails only that optional
capability, preserving valid lifecycle evidence.

This capability covers the native session boundary. It does not establish
continuous cwd observation, native event ordering or an alternate-directory
resume recipe. The helper mints a nonce for each delivery; it cannot infer the
activation of a separately delayed `CwdChanged` event from the current host
ledger, particularly after A -> B -> A. Such an event requires source-captured
activation correlation before asynchronous delivery. Uncorrelated standalone
cwd events are therefore ignored. Host/provider negotiation preserves this
boundary independently of the content-inspection contract below.

The exact resume recipe is `--resume <native UUID>` with the existing launcher,
account, profile and native-store inspection. A chosen process directory alone
is not confirmation that Claude resumed there. The accepted native identity and
actual execution directory remain separate observations; a UUID alone does not
establish equivalent account, profile or native-store scope.

Claude's [hook contract](https://code.claude.com/docs/en/hooks) gives
`SessionStart` and `CwdChanged` no native selection veto. Its
[session contract](https://code.claude.com/docs/en/sessions#branch-a-session)
permits concurrent writers to the same conversation. A rejected Logonomic
notification cannot enforce single native ownership, undo a selection or stop
another runtime. Native ownership control and live interaction acceptance are
tracked in [Claude #12](https://github.com/Varinomics/vnm_logonomic_cli_claude/issues/12).

## Native Mod activation lifetime

Claude's [Mods lifecycle](https://code.claude.com/docs/en/plugins/mods/reference)
runs `session.start` when a module loads; native clear and resume operations
instead end the current session and emit another `classic.SessionStart`.
Both wake modules therefore restart their listeners from each accepted main
session start and retire the departing activation synchronously. An activation
has its own listener, readiness and context-measurement state. Returning to the
same native ID after another conversation does not reactivate the old object.
Delayed callbacks cannot disable the successor or publish the first A's usage
as the returning A's measurement. An abandoned start does not publish telemetry
or start a listener. Authenticated classic hooks retain ownership of wake
registration cleanup.

Background transport consumers also bind once to the helper's accepted native
ID and transition nonce. Every listen/receive scan checks that binding before
claiming work; dispatch readiness carries the same nonce. A waiting consumer
from an earlier activation therefore cannot consume the returning A's request
or advertise its predecessor's readiness. A consumer leaves another
activation's live request untouched for its owner or lifecycle cleanup.

Module bootstrap cannot start a consumer while that activation's native
`SessionStart` is pending; the completed native event owns that readiness
transition. Binding also waits for publication of `registration.json`, within
the existing transport deadline. Creating its directory alone does not prove
that the accepted registration has been published.

The maintained fixtures cover this native event sequence and delayed callback
ordering. They do not establish live acceptance for a particular installed
Claude version or enforce exclusive native ownership.

## Resume contracts

The package supports three independent resume contracts:

- `interactive-resume@2` prepares an exact native resume and validates native
  availability again before allocating hooks or wake state.
- `interactive-resume@3` inspects native availability without preparing a
  process or modifying the profile.
- `interactive-resume@4` also reports whether the exact saved conversation has
  retained content. It is negotiated separately; version 3 responses omit the
  additional member so installed hosts retain their existing behavior.

Availability and content share the same resolved launcher, account, profile,
environment, working directory and exact transcript lookup. A natively
resumable transcript can still have no conversation content. Content never
changes the version 2 availability predicate or the stored resume record.

Resume preparation reports a distinct reason and a complete user-facing sentence
for an invalid saved reference, an unsupported launcher command or Claude option,
missing Node.js, and an absent saved conversation. The option sentence names the
option without including its value. Missing launch environment or an invalid
request directory remains a developer failure without a user-facing description.

## Evidence for empty and nonempty content

Content inspection follows the Agent SDK's main user/assistant conversation
semantics. It certifies nonempty content only from a complete, understood
transcript containing a main conversation message. A title or token count is
not evidence of conversation content, and the last assistant record's content
length does not determine whether earlier work exists.

A complete transcript containing only recognized `custom-title`, `tag` or
`permission-mode` metadata, or a complete chain of `system` and `progress`
records, can be empty even when native availability remains unknown. The
inspection checks the requested identity and requires each chain parent to
have appeared earlier in the transcript. Repeated identities, missing or
reordered parents, sidechains, team messages, compaction, explicit rewind
metadata, summary-only logs and unrecognized record kinds remain unknown.

Read errors, limits, unsupported native configuration, corrupt records,
unterminated final records and changes to the file during inspection cannot
certify empty content. An unknown content result does not hide retained work.
Inspection never deletes native files or resume records; a subsequent read can
observe a late completed write and change empty or unknown to nonempty.

The native schema oracle is Anthropic's public Agent SDK:

- [Transcript record types, chain reconstruction and visible messages](https://github.com/anthropics/claude-agent-sdk-python/blob/2209f2c134fee914c0f75733b631a5fc238ca846/src/claude_agent_sdk/_internal/sessions.py)
- [Typed title and tag metadata](https://github.com/anthropics/claude-agent-sdk-python/blob/2209f2c134fee914c0f75733b631a5fc238ca846/src/claude_agent_sdk/_internal/session_mutations.py)
- [Progress-only chain and permission-mode fixtures](https://github.com/anthropics/claude-agent-sdk-python/blob/2209f2c134fee914c0f75733b631a5fc238ca846/tests/test_sessions.py)

These references record schema provenance; they do not restrict supported
Claude versions. Unrecognized native data remains unknown rather than becoming
an empty conversation.
