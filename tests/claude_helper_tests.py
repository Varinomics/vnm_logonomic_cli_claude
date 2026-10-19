"""Exercise the actual helper and SDK wrapper transport, without real sessions."""

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def launch_options():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def relay_through_launcher(relay, payload, environment=None):
    process = subprocess.Popen([sys.executable, __file__, "--launcher", str(relay)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
        **launch_options())
    stdout, stderr = process.communicate(json.dumps(payload).encode("utf-8"), timeout=25)
    assert process.returncode == 0, stderr.decode("utf-8", errors="replace")
    return process.pid, stdout


def native_owner_fixture(helper, relay):
    for line in sys.stdin:
        event = json.loads(line)
        if event["hook_event_name"] == "SessionStart":
            result = subprocess.run([helper, "wake-owner"], capture_output=True, timeout=5, **launch_options())
            assert result.returncode == 0, result.stderr
        launcher_pid, _ = relay_through_launcher(relay, event)
        print(json.dumps({"owner_pid": os.getpid(), "launcher_pid": launcher_pid}), flush=True)


class Adapter:
    def __init__(self, executable):
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **launch_options())
        self.serial = 0
        self.send({"type": "hello", "core_protocol_generations": [1], "package_id": "logonomic.cli.claude",
            "generation_digest": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "generation_nonce": uuid.uuid4().hex,
            "limits": {"max_frame_bytes": 4194304, "max_json_depth": 32,
                "max_in_flight_requests": 64, "max_progress_frames_per_request": 100000},
            "host": {"product": "logonomic", "version": "1.0.0"}})
        assert self.read()["type"] == "hello"
        assert self.read()["type"] == "capabilities"

    def send(self, frame):
        self.process.stdin.write((json.dumps(frame) + "\n").encode("utf-8"))
        self.process.stdin.flush()

    def read(self):
        frame = self.process.stdout.readline()
        assert frame, "adapter closed before its response"
        return json.loads(frame)

    def prepare(self, profile):
        self.serial += 1
        request_id = "r" + str(self.serial)
        self.send({"type": "invoke", "request_id": request_id, "capability_id": "launch-preparation",
            "version": 3, "operation": "prepare", "deadline_ms": 5000, "payload": {
                "launcher": {"script_body": "claude", "runner_key": "direct", "environment_text": ""},
                "working_directory": str(profile.parent), "title_instruction": "Set the session title.",
                "declared_assets": [], "profile_storage": {"version": 1, "root": str(profile)},
                "launcher_argv": ["claude"], "session_wake": {"version": 1},
                "launch_environment": {"platform": "windows-x86_64" if os.name == "nt" else ("macos-arm64" if sys.platform == "darwin" else "linux-x86_64"),
                    "effective_environment": {"PATH": os.environ["PATH"]}}}})
        while True:
            response = self.read()
            if response["type"] == "result":
                assert response["request_id"] == request_id and response["status"] == "success", response
                assert response["payload"]["session_wake"]["available"], response
                return response["payload"]["environment"]["set"]

    def close(self):
        self.send({"type": "shutdown", "grace_ms": 5000})
        self.process.stdin.close()
        assert self.process.wait(timeout=10) == 0


def sweep_policy(adapter, profile, parent, pid, birth):
    def marker(directory, native_live, registrar_live):
        file = parent / (directory.name + f".{pid}.{birth}" + ("" if native_live else "0") +
            f".{pid}.{birth}" + ("" if registrar_live else "0") + ".owner")
        file.touch()
        return file

    live, publishing, dead, unowned, malformed, never_created = [parent / uuid.uuid4().hex for _ in range(6)]
    for directory in (live, publishing, dead, unowned, malformed):
        directory.mkdir()
    live_marker = marker(live, True, False)
    publishing_marker = marker(publishing, False, True)
    dead_marker = marker(dead, False, False)
    pending_marker = marker(never_created, False, False)
    malformed_marker = parent / (malformed.name + ".invalid.owner")
    malformed_marker.touch()
    for name in ("registration.json", "active.json." + uuid.uuid4().hex,
                 uuid.uuid4().hex + ".request", "a" * 64 + ".receipt." + uuid.uuid4().hex):
        (dead / name).write_text("{}", encoding="utf-8")
    adapter.prepare(profile)
    assert live.is_dir() and live_marker.is_file(), "alive native ownership was swept"
    assert publishing.is_dir() and publishing_marker.is_file(), "alive registrar no longer protects publication"
    assert unowned.is_dir() and malformed.is_dir() and malformed_marker.is_file(), "unknown ownership was swept"
    assert not dead.exists() and not dead_marker.exists() and not pending_marker.exists(), "dead ownership leaked"


def wake_owner_contract(helper, executable, relay, environment, root):
    adapter = Adapter(executable)
    environment = {**environment, "LOGONOMIC_PROVIDER_HELPERS_DIR": str(executable.parent.parent / "helpers")}
    profile = root / "profile"
    start = {"hook_event_name": "SessionStart", "session_id": "outer-session", "source": "startup"}
    try:
        unanchored = adapter.prepare(profile)
        wake = Path(unanchored["VNM_CLAUDE_WAKE_DIRECTORY"])
        relay_through_launcher(relay, {**start, "native_owner_pid": os.getpid()}, {**environment, **unanchored})
        assert not wake.exists() and not list(wake.parent.iterdir()), "disabled Mods allocated wake storage"

        for crash in (False, True):
            prepared = adapter.prepare(profile)
            wake = Path(prepared["VNM_CLAUDE_WAKE_DIRECTORY"])
            native_helper = Path(prepared["VNM_CLAUDE_WAKE_OWNER_HELPER"])
            assert native_helper.is_absolute() and native_helper.samefile(executable.parent.parent / "helpers" / helper.name)
            owner = subprocess.Popen([sys.executable, __file__, "--native-owner", str(native_helper), str(relay)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env={**environment, **prepared}, text=True, **launch_options())

            def event(payload):
                owner.stdin.write(json.dumps(payload) + "\n")
                owner.stdin.flush()
                return json.loads(owner.stdout.readline())

            try:
                launched = event(start)
                registration = json.loads((wake / "registration.json").read_text(encoding="utf-8"))
                assert registration["owner_pid"] == owner.pid != launched["launcher_pid"], registration
                assert owner.poll() is None
                adapter.prepare(profile)
                assert wake.is_dir(), "an exited intermediate launcher caused live-session cleanup"
                if crash:
                    owner.kill()
                    owner.communicate(timeout=10)
                    adapter.prepare(profile)
                    assert not wake.exists() and not list(wake.parent.glob(wake.name + ".*.owner")), \
                        "a dead native owner left its directory or owner markers"
                else:
                    sweep_policy(adapter, profile, wake.parent, owner.pid, registration["owner_birth"])
                    event({**start, "session_id": "cleared-session", "source": "clear"})
                    registration = json.loads((wake / "registration.json").read_text(encoding="utf-8"))
                    assert registration["provider_session_id"] == "cleared-session" and registration["owner_pid"] == owner.pid
                    end = {"hook_event_name": "SessionEnd", "session_id": "outer-session", "reason": "exit"}
                    event(end)
                    assert wake.is_dir(), "an old session end removed the replacement session"
                    event({**end, "session_id": "cleared-session"})
                    assert not wake.exists() and not list(wake.parent.glob(wake.name + ".*.owner")), \
                        "SessionEnd through a new intermediate launcher failed to clean stable native ownership"
                    owner.stdin.close()
                    owner.stdin = None
                    _, stderr = owner.communicate(timeout=10)
                    assert owner.returncode == 0, stderr
            finally:
                if owner.poll() is None:
                    owner.kill()
                    owner.communicate(timeout=10)
    finally:
        adapter.close()


def main():
    helper, wrapper, adapter, relay = (Path(value).resolve() for value in sys.argv[1:5])
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

        completion = {"hook_event_name": "ModTurnComplete", "session_id": "new-session", "reason": "answer"}
        result, records, _ = run(completion)
        assert result.returncode == 0 and result.stdout == ""
        assert [item["operation"] for item in records] == ["provider-event", "acknowledge"]
        assert records[0]["request"]["event"]["body"]["reason"] == "answer"
        assert "prompt_id" not in records[0]["request"]["event"]["body"]

        wake_parent = Path(temporary).resolve() / "wake"
        wake_parent.mkdir()
        wake_directory = wake_parent / ("d" * 32)
        anchor = subprocess.run([str(helper), "wake-owner"], capture_output=True, timeout=5,
            env={**environment, "VNM_CLAUDE_WAKE_DIRECTORY": str(wake_directory)}, **launch_options())
        assert anchor.returncode == 0 and not wake_directory.exists(), "native anchor allocated before host acceptance"
        result, _, _ = run(start, VNM_CLAUDE_WAKE_DIRECTORY=str(wake_directory),
                           CLAUDE_TEST_LIFECYCLE="refused")
        assert not wake_directory.exists(), "refused native start allocated wake storage"
        result, _, _ = run(start, VNM_CLAUDE_WAKE_DIRECTORY=str(wake_directory))
        assert result.returncode == 0 and (wake_directory / "registration.json").is_file()
        registration = json.loads((wake_directory / "registration.json").read_text(encoding="utf-8"))
        for _ in range(2):
            result, records, _ = run({**start, "source": "compact"},
                VNM_CLAUDE_WAKE_DIRECTORY=str(wake_directory))
            assert result.returncode == 0, result.stderr
            assert [item["operation"] for item in records] == ["provider-event", "request-title-instruction", "acknowledge"]
            assert json.loads(result.stdout)["hookSpecificOutput"]["hookEventName"] == "SessionStart"
            current = json.loads((wake_directory / "registration.json").read_text(encoding="utf-8"))
            assert current == registration, "compaction changed the accepted startup wake registration"
            assert records[0]["request"]["transition_nonce"] != registration["transition_nonce"], \
                "fixture must distinguish the compaction delivery nonce from the session transition"
        end = {"hook_event_name": "SessionEnd", "session_id": "other-session", "reason": "exit"}
        run(end, VNM_CLAUDE_WAKE_DIRECTORY=str(wake_directory))
        assert wake_directory.is_dir(), "unrelated session end removed a live binding"
        run({**end, "session_id": "new-session"}, VNM_CLAUDE_WAKE_DIRECTORY=str(wake_directory))
        assert not wake_directory.exists() and not list(wake_parent.iterdir())
        wake_owner_contract(helper, adapter, relay, environment, Path(temporary).resolve())

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
    if sys.argv[1] == "--launcher":
        result = subprocess.run([shutil.which("node"), sys.argv[2]], input=sys.stdin.buffer.read(),
            capture_output=True, timeout=25, **launch_options())
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    if sys.argv[1] == "--native-owner":
        native_owner_fixture(sys.argv[2], sys.argv[3])
    else:
        main()
