# Conversation history inspection

Claude's `SessionStart` hook supplies the native conversation identity and exact
resume record. A `clear` source starts a new conversation; a `resume` source
reopens an existing one. A `compact` source does not create an identity
transition. The host retains departing identities independently of the running
terminal and decides which remembered conversations to display.

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
