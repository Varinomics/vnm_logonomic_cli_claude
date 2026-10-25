# Conversation extraction

The optional `conversation-export@1` capability extracts retained Claude Code
history as UTF-8 Markdown, plain text or the common conversation format. The shared SDK owns the transport
contract and text writer; this package owns Claude's transcript interpretation.

The provider uses the original account profile or frozen `CLAUDE_CONFIG_DIR`,
`HOME` and `USERPROFILE`. It locates the exact session file directly below a
`projects` directory, checks native `sessionId` values, and never substitutes
a nested subagent transcript or another account's history. The host's managed
profile-storage directory is not Claude's native profile.

The latest main conversation leaf selects a `parentUuid` chain. Rewinds and
other leaves remain outside that chain. Forks already contain their copied
history, so the provider does not follow `forkedFrom` into the parent file.
Claude writes separate assistant records for content blocks sharing the same
API `message.id`. Those blocks remain distinct. Parallel tool calls and their
results are reunited when native response/call identities prove they belong
to the selected response; unrelated branches and subagents remain excluded.
Direct-parent results take precedence over off-branch candidates, even when a
stray result was written earlier. Strays fill only calls still unanswered after
the selected chain and all direct results have been considered.

After compaction, the native active chain may begin at a compact summary.
That summary is visibly labelled **Compacted conversation summary**. The
document explains that it contains retained summary content and later messages,
and that original messages before compaction are not reconstructed. The
provider follows `parentUuid`, matching the official SDK, and does not join
`logicalParentUuid` to duplicate pre-compaction history. A missing ordinary
parent or cycle is an explicit failure rather than an apparently complete
export.

Text, nonempty thinking text, tool arguments/results and image/document references
are retained. Native rows that contribute no export blocks are skipped, including
tool results with omitted content or an empty content array.
Tool-discovery references retain their native `tool_reference` JSON,
including `tool_name`, in the containing result's original position. Each encrypted
`redacted_thinking` block remains as `[Encrypted message]` in its original position,
in both full and compact extraction. Native advisor results retain plaintext
guidance and errors in full extraction; encrypted advisor results retain the same
placeholder in both projections. Embedded binary
attachments are identified by media type; their base64 bytes are not inserted
into the conversation text. Unsupported content blocks fail explicitly.

Compact extraction keeps user content and final assistant replies. Native
`stop_reason: end_turn` identifies a final reply; `tool_use` and `pause_turn`
identify continuing responses whose text is omitted in compact mode. Thinking,
tool calls/results, tool-discovery references and compact-summary bodies are
also omitted. The existing notice that pre-compaction originals are not reconstructed
remains visible. Attachments belonging to retained user or assistant content remain.
Missing or other stop reasons stay unclassified and their assistant text is
retained conservatively. No classification depends on message wording. Unchecking
Compact re-extracts the full retained discussion from the unchanged native source.

Extraction freezes the open file's length and reads complete JSONL records
within that snapshot. An unfinished final record is reported in the result.
Malformed complete records, conflicting source files and identity mismatches
leave no completed output. Conversation extraction does not require the
original working directory to remain present.

## Native schema and test oracle

The branch and content-block semantics follow Anthropic's public Agent SDK:

- [Session reader and parallel-tool recovery](https://github.com/anthropics/claude-agent-sdk-python/blob/main/src/claude_agent_sdk/_internal/sessions.py)
- [Session fork transformation](https://github.com/anthropics/claude-agent-sdk-python/blob/main/src/claude_agent_sdk/_internal/session_mutations.py)
- [Native transcript tests](https://github.com/anthropics/claude-agent-sdk-python/blob/main/tests/test_sessions.py)
- [Tool-search reference blocks](https://platform.claude.com/docs/en/agents-and-tools/tool-use/tool-search-tool#custom-tool-search-implementation)
- [Native response stop reasons](https://platform.claude.com/docs/en/build-with-claude/handling-stop-reasons)
- [Encrypted redacted thinking](https://platform.claude.com/docs/en/about-claude/models/extended-thinking-models#redacted-thinking-blocks)
- [Optional tool-result content](https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/types/tool_result_block_param.py)
- [Thinking text and opaque verification signatures](https://github.com/anthropics/anthropic-sdk-python/blob/main/src/anthropic/types/thinking_block.py)
- [Claude Code advisor](https://code.claude.com/docs/en/advisor)
- [Advisor result variants](https://platform.claude.com/docs/en/agents-and-tools/tool-use/advisor-tool#result-variants)

The inspected SDK revision was `163f50ed56fa8c3fd9346148b6e56e7696794b58`.
It records oracle provenance and does not pin supported CLI versions.
`claude_conversation_tests` uses schema-focused fixtures for parallel blocks
sharing `message.id`, tool result ancestry, active branches, forks, compact
summaries, attachments, Unicode, partial tails and source errors. These tests
exercise the provider parser; they do not launch Claude Code.
