# Native model observations

The optional `model-state@1` capability reports literal native model evidence.
The existing lifecycle and structured-session carriers remain usable without
opting in, and malformed optional model data fails only that capability.
A model does not identify its serving backend: provider remains absent unless
a native source identifies it independently. Claude Code may use custom
endpoints, Bedrock, or Vertex.

## Source and actor attribution

| Native source | Actor | Meaning |
| --- | --- | --- |
| Root `SessionStart.model` | Root session | Live model on the same accepted transition, sequence zero |
| `classic.PostModelSwitch.to_model` | Root session | Resolved live model after a native session switch |
| `turn.step` result `usage.model` | Root or named child | Model reported for that one native response |
| Structured `system/init.model` | Own structured root | Native live model after initialization and provenance checks |
| Structured root `assistant.message.model` | Own structured root | Literal served model, replacing earlier live evidence |

The native [Mods declarations](https://github.com/anthropics/claude-code/blob/main/mods/types/claude-code.d.ts)
distinguish `TurnStepInput.model` (the request's selection) from
`TurnStepResult.usage.model` (the model that answered that one request).
The implementation uses the latter. It does not inspect `turn.complete`
aggregates, `result.modelUsage`, session usage totals, agent types, or launcher
settings to choose a badge. Native
[PostModelSwitch](https://code.claude.com/docs/en/hooks#postmodelswitch)
identifies the resolved `to_model`; `requested_model` is not used.

A child step is eligible only when its own native `agentId` is present in
`$.agent.list()`. Native internal compaction/memory forks have IDs absent from
that roster. A root model is never copied into a child. Compaction stop results,
synthetic models, and aborted steps are excluded. Structured inspection excludes
foreign session IDs, parent tool responses, synthetic assistant records, and
compact summaries.

## Activation and ordering

The existing in-process plugin holds the binding published after the host
accepts the native start. A model hook captures that binding and increments its
source sequence synchronously before awaiting the native request or switch.
The helper transports that captured nonce and sequence without replacing either
with its own delivery nonce or the host's current ledger state.

A Mods reload evaluates a new copy of the module while the session keeps its
accepted registration and transition nonce, so the sequence cannot restart with
the module. As its `session.start` begins, each copy writes a load marker in
native session state (`$.state`); the version of that write orders the copies.
Claude Code keeps state versions increasing when `/clear`, `/resume` and
`/branch` reset that state together with the activation.

When a copy obtains the binding, it takes over sequencing with a
compare-and-set write, unless a later copy already owns it, and counts on from
the highest sequence published. Native events reach only the newest copy, but a
replaced copy's listener can still finish its binding afterwards; its earlier
load cannot take ownership back. An observation is published only after another
compare-and-set confirms that its copy still owns sequencing and records its
sequence. A request captured by the copy that a reload replaced is therefore
omitted rather than published at or above its successor's sequences.

The source checks the same activation again after asynchronous work. Retired
activations, including an earlier A after A → B → A, cannot publish into the
returning session. An out-of-order completion retains its earlier sequence so
the host can reject stale evidence. A child roster read is also fenced against
an activation change.

A step that begins before the binding and its ownership are available is
omitted; a later binding cannot retroactively authenticate it. Without native
session state, the activation reports no model observations. The native
SessionStart snapshot can still supply the root model. A child with no later
eligible response remains unconfirmed. Absence of a native model is not replaced
with a parent or launcher value.

`ModModel` uses the existing provider-event transport but is classified as
ignored for lifecycle purposes. It adds no lifecycle observation, attention
state, independent log row, or UI surface.

Structured observations stay within the same inspected exchange and are retained
across JSONL chunk boundaries. A later process failure does not erase already
observed native evidence. Native startup administration, including the
`system/ui_invalidate` event with `event=ui.render` observed in 2.1.325, must bind
the same session before initialization; it never supplies model evidence.

## Qualification scopes

`claude_model_tests` checks the classifier, structured parser, and actual SDK
carriers. `claude_model_hook_tests.mjs` checks the in-process source against
controlled activation, module reload and completion races. The existing
continuation, context, and activation tests cover neighboring behavior.

`claude_model_mod.test.ts` runs in Claude's actual plugin engine with scripted
native response, roster, and subprocess boundaries. Copy it beside the candidate
plugin and run `claude plugin test <candidate-plugin-directory>`. This qualifies
native hook dispatch and production module behavior, not a real model service,
a complete interactive child run, or the Logonomic UI.

`claude_native_model_tests.py --claude <native-cli> --adapter <built-adapter>`
uses a disposable profile, `--bare`, a loopback Messages endpoint, a fixture API
key, and disabled nonessential traffic. It obtains actual native headless output
whose init and response models differ, then sends that unmodified stream through
the compiled production adapter's structured inspector. No live account or paid
model is used. Its scope is native structured model evidence, independently of
package staging and full host integration.
