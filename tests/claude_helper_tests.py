"""Exercise the actual helper and SDK wrapper transport, without real sessions."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    helper, wrapper = (Path(value).resolve() for value in sys.argv[1:3])
    with tempfile.TemporaryDirectory(prefix="claude-helper-") as temporary:
        capture = Path(temporary) / "requests.jsonl"
        environment = {
            **os.environ,
            "LOGONOMIC_VNMCTL_PATH": str(wrapper),
            "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.claude",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(helper.parent),
            "CLAUDE_TEST_CAPTURE": str(capture),
        }
        prompt = {"hook_event_name": "UserPromptSubmit", "session_id": "native-session",
                  "prompt": "Show my changes"}
        expected = {"hookSpecificOutput": {"hookEventName": "UserPromptSubmit",
                    "additionalContext": "Host changed-files and delegation.\nExact notice text."}}

        def run(payload, **overrides):
            capture.write_text("", encoding="utf-8")
            started = time.monotonic()
            result = subprocess.run([str(helper), "event"], input=json.dumps(payload),
                encoding="utf-8", capture_output=True, timeout=25,
                env={**environment, **overrides},
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            elapsed = time.monotonic() - started
            records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
            return result, records, elapsed

        for mode in ("accepted", "refused", "transport", "stall"):
            result, records, elapsed = run(prompt, CLAUDE_TEST_LIFECYCLE=mode)
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout) == expected
            assert records[0]["operation"] == "request-title-instruction"
            assert records[0]["request"]["changed_files_requested"] is True
            deliveries = [item for item in records if item["operation"] == "provider-event"]
            assert deliveries and all(item == deliveries[0] for item in deliveries)
            acknowledgments = [item for item in records if item["operation"] == "acknowledge"]
            assert bool(acknowledgments) == (mode in ("accepted", "refused"))
            if mode == "stall":
                assert elapsed < 17, "cooperatively terminated fixture exhausted the native relay deadline"

        for mode in ("empty", "refused"):
            result, records, _ = run(prompt, CLAUDE_TEST_CONTEXT=mode)
            assert result.returncode == 0 and result.stdout == ""
        stop = {"hook_event_name": "Stop", "session_id": "native-session",
                "last_assistant_message": "Which directory?"}
        for payload, retained in ((stop, True), ({**stop, "agent_id": "child"}, False),
                                  ({**stop, "last_assistant_message": "x" * 2049}, False)):
            result, records, _ = run(payload)
            assert result.returncode == 0, result.stderr
            delivered = next(item for item in records if item["operation"] == "provider-event")
            event = delivered["request"]["event"]
            assert event["name"] == "Stop"
            assert ("last_assistant_message" in event["body"]) == retained
            if retained:
                assert event["body"]["last_assistant_message"] == stop["last_assistant_message"]
        result, records, _ = run({**prompt, "agent_id": "child-agent"})
        assert result.stdout == "" and all(item["operation"] != "request-title-instruction" for item in records)
        start = {"hook_event_name": "SessionStart", "session_id": "new-session", "source": "startup"}
        result, records, _ = run(start)
        assert result.returncode == 0
        assert [item["operation"] for item in records] == ["provider-event", "request-title-instruction", "acknowledge"]
        result, records, _ = run(start, CLAUDE_TEST_LIFECYCLE="refused")
        assert result.stdout == "" and all(item["operation"] != "request-title-instruction" for item in records)

        capture.write_text("", encoding="utf-8")
        process = subprocess.Popen([str(helper), "event", "--watch-parent"], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env={**environment, "CLAUDE_TEST_CONTEXT": "stall"},
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            process.stdin.write(json.dumps(prompt) + "\n")
            process.stdin.flush()
            until = time.monotonic() + 5
            while not capture.read_text(encoding="utf-8") and time.monotonic() < until:
                time.sleep(0.01)
            assert capture.read_text(encoding="utf-8"), "context request never started"
            process.stdin.close()
            process.stdin = None
            output, _ = process.communicate(timeout=10)
            assert output == "", "parent cancellation emitted unaccepted context"
            assert "acknowledge" not in capture.read_text(encoding="utf-8")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    main()
