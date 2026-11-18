"""Exercise the actual helper and SDK wrapper transport, without real sessions."""

import json
import hashlib
from concurrent.futures import ThreadPoolExecutor
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
        self.capabilities = self.read()
        assert self.capabilities["type"] == "capabilities"

    def send(self, frame):
        self.process.stdin.write((json.dumps(frame) + "\n").encode("utf-8"))
        self.process.stdin.flush()

    def read(self):
        frame = self.process.stdout.readline()
        assert frame, "adapter closed before its response"
        return json.loads(frame)

    def invoke(self, capability, version, operation, payload):
        self.serial += 1
        request_id = f"r{self.serial}"
        self.send({"type": "invoke", "request_id": request_id, "capability_id": capability,
                   "version": version, "operation": operation, "deadline_ms": 5000, "payload": payload})
        while True:
            result = self.read()
            if result["type"] == "result":
                assert result["request_id"] == request_id
                return result

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
                prepared = response["payload"]["environment"]["set"]
                assert Path(prepared["VNM_CLAUDE_WAKE_DIRECTORY"]).parent == (profile / "wake").resolve()
                assert response["payload"]["session_wake"]["binding"]["directory"] == prepared["VNM_CLAUDE_WAKE_DIRECTORY"]
                return prepared

    def close(self):
        self.send({"type": "shutdown", "grace_ms": 5000})
        self.process.stdin.close()
        assert self.process.wait(timeout=10) == 0

    def classify(self, delivery, version, current_session=None, *, attention=False, pending=None, execution=None):
        self.serial += 1
        request_id = "r" + str(self.serial)
        payload = {"event": delivery["event"], "transition_nonce": delivery["transition_nonce"],
            "ledger": {"current_epoch": 0, "current_transition": None}}
        if current_session is not None:
            payload["ledger"] = {"current_epoch": 2, "current_transition": {
                "kind": "start", "provider_session_id": current_session, "nonce": "7" * 32}}
        if version is not None:
            payload["context_state_version"] = version
        if pending is not None:
            payload["pending_conversational_request_id"] = pending
        if execution is not None:
            payload["execution_context"] = execution
        self.send({"type": "invoke", "request_id": request_id,
            "capability_id": "attention-kind" if attention else "helper-ingress",
            "version": 1 if attention else 2, "operation": "classify_event", "deadline_ms": 5000, "payload": payload})
        while True:
            response = self.read()
            if response["type"] == "result":
                assert response["request_id"] == request_id and response["status"] == "success", response
                return response["payload"]


def reply_evidence_contract(helper, executable, relay, environment, capture):
    adapter = Adapter(executable)
    stop = {"hook_event_name": "Stop", "session_id": "native-session", "prompt_id": "native-turn"}
    question = "Which directory should I use?"
    private = "Earlier-detail-canary " * 180
    long_paragraph = "Please provide " + "the required configuration detail " * 120 + "for this task."
    utf8_paragraph = "Which " + "résumé🙂 " * 400 + "should I use？"

    def deliver(payload):
        capture.write_text("", encoding="utf-8")
        _, stdout = relay_through_launcher(relay, payload, environment)
        if payload["hook_event_name"] == "Stop":
            assert stdout == b"", "reply evidence must not answer the native question"
        records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
        return next(record["request"] for record in records if record["operation"] == "provider-event")

    cases = (
        ("long explanation", private + "\n\n" + question, True, True),
        ("long paragraph", long_paragraph, True, True),
        ("UTF-8 paragraph", utf8_paragraph, True, True),
        ("ordinary conclusion", private + "\n\nThe change is complete.", True, False),
        ("earlier question", question + "\n\nThe change is complete.", True, False),
        ("short question", question, True, True),
        ("open fence", "\x60\x60\x60text\n" + private + "\n\n" + question, False, False),
        ("closed fence", "~~~~text\n" + private + "\n\n" + question + "\n~~~~", False, False),
        ("prose after fence", "\x60\x60\x60\n" + private + "\n\x60\x60\x60\n\n" + question, True, True),
        ("lazy blockquote", "> Quoted text\n" + private + question, False, False),
        ("indented code", "    " + question, False, False),
        ("invalid native type", {"text": question}, False, False),
        ("embedded NUL", "Which \0directory?", False, False),
    )
    try:
        assert {"capability_id": "attention-kind", "version": 1} in adapter.capabilities["capabilities"]
        for invalid in (b'{"hook_event_name":"Stop","last_assistant_message":"\xff?"}',
                        b'{"hook_event_name":"Stop","last_assistant_message":"Which directory?'):
            capture.write_text("", encoding="utf-8")
            rejected = subprocess.run([str(helper), "event"], input=invalid,
                capture_output=True, timeout=10, env=environment, **launch_options())
            assert rejected.returncode == 2 and rejected.stdout == b""
            assert capture.read_text(encoding="utf-8") == "", "invalid native input reached the host"
        positive = None
        for label, reply, retained, hint in cases:
            delivery = deliver({**stop, "last_assistant_message": reply})
            body = delivery["event"]["body"]
            assert "last_assistant_message" not in body, label
            assert "Earlier-detail-canary" not in json.dumps(body), label
            assert ("assistant_reply_evidence" in body) == retained, label
            if retained:
                evidence = body["assistant_reply_evidence"]
                assert set(evidence) == {"scope", "context", "prefix", "suffix"}, label
                assert evidence["scope"] == "final_paragraph_edges" and evidence["context"] == "prose", label
                assert all(0 < len(evidence[edge].encode("utf-8")) <= 128 for edge in ("prefix", "suffix")), label
                if reply == long_paragraph:
                    assert evidence["prefix"].startswith("Please provide ") and evidence["suffix"].endswith("for this task.")
                if reply == utf8_paragraph:
                    assert evidence["suffix"].endswith("should I use？")
            classified = adapter.classify(delivery, None, "native-session", attention=True)
            assert ("attention_kind" in classified) == hint, label
            observation = classified["observations"][0]
            assert observation["provider_event_name"] == "Stop" and observation["provider_turn_id"] == "native-turn", label
            assert observation["phase_transition"] == "awaiting_input" and observation["phase_evidence"] == "inferred", label
            assert observation["outcome_transition"] == "none", label
            assert adapter.classify(delivery, None, "native-session", attention=True) == classified, label
            plain = adapter.classify(delivery, None, "native-session")
            assert plain["observations"][0]["attention_transition"] == "none", label
            if hint:
                assert classified["attention_kind"] == {"version": 1, "observation_index": 0, "kind": "conversational_hint"}
                assert observation["attention_transition"] == "requested" and observation["attention_evidence"] == "inferred"
                assert observation["attention_request_id"]
                positive = delivery
            else:
                assert observation["attention_transition"] == "none" and observation["attention_request_id"] == "", label

        assert positive is not None
        for extra in ({"agent_id": "child"}, {"agent_id": ""}, {"hook_event_name": "StopFailure"}):
            delivery = deliver({**stop, "last_assistant_message": question, "error": "native error", **extra})
            assert "assistant_reply_evidence" not in delivery["event"]["body"]
            assert "last_assistant_message" not in delivery["event"]["body"]
            assert "attention_kind" not in adapter.classify(delivery, None, "native-session", attention=True)

        # Native input cannot smuggle preconstructed metadata past the helper.
        supplied = positive["event"]["body"]["assistant_reply_evidence"]
        delivery = deliver({**stop, "assistant_reply_evidence": supplied})
        assert "assistant_reply_evidence" not in delivery["event"]["body"]

        # The classifier also validates the evidence contract at its own boundary.
        for malformed in (None, question, {**supplied, "scope": "complete_message"},
                          {**supplied, "context": "code"}, {**supplied, "prefix": "x" * 129},
                          {**supplied, "suffix": ""}, {**supplied, "extra": True}):
            delivery = json.loads(json.dumps(positive))
            delivery["event"]["body"]["assistant_reply_evidence"] = malformed
            assert "attention_kind" not in adapter.classify(delivery, None, "native-session", attention=True)
        delivery["event"]["body"].pop("assistant_reply_evidence")
        delivery["event"]["body"]["last_assistant_message"] = question
        assert "attention_kind" not in adapter.classify(delivery, None, "native-session", attention=True)

        hint = adapter.classify(positive, None, "native-session", attention=True)
        request_id = hint["observations"][0]["attention_request_id"]
        answer = deliver({"hook_event_name": "UserPromptSubmit", "session_id": "native-session",
            "prompt_id": "next-turn", "prompt": "Use the chosen directory."})
        resolved = adapter.classify(answer, None, "native-session", attention=True, pending=request_id)
        assert resolved["observations"][0]["attention_transition"] == "resolved"
        assert resolved["observations"][0]["attention_request_id"] == request_id
        assert "attention_kind" not in adapter.classify(answer, None, "native-session", attention=True)
    finally:
        adapter.close()


def execution_context_contract(executable, run, root):
    adapter = Adapter(executable)
    session_id = "a375d05e-f6cf-4812-80f5-91b33d24c053"
    start = {"hook_event_name": "SessionStart", "session_id": session_id, "source": "resume"}
    cwd = str(root / "uncreated directory" / "nested ")

    def deliver(payload):
        result, records, _ = run(payload, CLAUDE_TEST_CONTEXT="empty")
        assert result.returncode == 0 and result.stdout == "", result.stderr
        return next(record["request"] for record in records if record["operation"] == "provider-event")

    try:
        assert {"capability_id": "execution-context", "version": 1} in adapter.capabilities["capabilities"]
        delivery = deliver({**start, "cwd": cwd, "transcript_path": "/private/transcript",
            "project_dir": "/launch/project", "tool_input": {"command": "cd /not-authoritative"}})
        body = delivery["event"]["body"]
        assert body["cwd"] == cwd
        assert not any(key in body for key in ("transcript_path", "project_dir", "tool_input"))
        result = adapter.classify(delivery, None, execution={"version": 1})
        expected = {"version": 1, "updates": [{"provider_session_id": session_id,
            "transition_nonce": delivery["transition_nonce"], "sequence": "0", "state": "confirmed",
            "environment": "local", "cwd": cwd, "source": "claude.SessionStart"}]}
        assert result["execution_context"] == expected
        assert result["classification"] == "transition" and result["transition"] == {"kind": "resume"}
        assert result["observations"][0]["resume_record"] == {"provider_session_id": session_id}
        assert adapter.classify(delivery, None, execution={"version": 1}) == result
        assert "execution_context" not in adapter.classify(delivery, None)
        invalid = adapter.classify(delivery, None, execution={"version": "1"})
        assert invalid["classification"] == "transition" and "failure" in invalid["execution_context"]
        for malformed in (None, 12, "relative/path", "ssh://remote/work", "/work\nother", "/work\0other", "/" * 4097):
            delivery = deliver({**start, "cwd": malformed})
            assert delivery["event"]["body"]["cwd"] is None
            result = adapter.classify(delivery, None, "different-session", execution={"version": 1})
            update = result["execution_context"]["updates"][0]
            assert result["classification"] == "transition" and update["state"] == "unknown"
            assert update["provider_session_id"] == session_id and update["transition_nonce"] == delivery["transition_nonce"]
            assert update["cwd"] == "" and update["environment"] == ""
        delivery = deliver(start)
        assert "cwd" not in delivery["event"]["body"]
        assert adapter.classify(delivery, None, execution={"version": 1})["execution_context"]["updates"][0]["state"] == "unknown"
        delivery = deliver({**start, "cwd": cwd, "agent_id": "child"})
        assert "cwd" not in delivery["event"]["body"]
        result = adapter.classify(delivery, None, execution={"version": 1})
        assert result["classification"] == "ignore" and "execution_context" not in result
    finally:
        adapter.close()


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


def wake_exchange_contract(adapter, wake, registration):
    def receive():
        until = time.monotonic() + 7
        while time.monotonic() < until:
            requests = list(wake.glob("*.request"))
            if requests:
                request_path = requests[0]
                request = json.loads(request_path.read_text(encoding="utf-8"))
                assert request["operation"] == "inspect"
                assert request["payload"]["session"]["provider_session_id"] == registration["provider_session_id"]
                assert request["payload"]["correlation"]["transition_nonce"] == registration["transition_nonce"]
                request_path.rename(request_path.with_suffix(".claimed"))
                request_path.with_suffix(".response").write_text(json.dumps({"available": True}), encoding="utf-8")
                return
            time.sleep(0.01)
        raise AssertionError("adapter did not publish its wake request")

    adapter.serial += 1
    request_id = "r" + str(adapter.serial)
    with ThreadPoolExecutor(max_workers=1) as executor:
        received = executor.submit(receive)
        adapter.send({"type": "invoke", "request_id": request_id, "capability_id": "session-wake",
            "version": 1, "operation": "inspect", "deadline_ms": 10000, "payload": {
                "session": {"provider_session_id": registration["provider_session_id"],
                    "working_directory": str(wake.parent), "launcher_argv": ["claude"],
                    "launch_environment": {"platform": "windows-x86_64" if os.name == "nt" else
                        ("macos-arm64" if sys.platform == "darwin" else "linux-x86_64"),
                        "effective_environment": {"PATH": os.environ["PATH"]}}},
                "correlation": {"version": 1, "transition_nonce": registration["transition_nonce"]},
                "binding": {"directory": str(wake)}}})
        while True:
            response = adapter.read()
            if response["type"] == "result":
                assert response["request_id"] == request_id and response["status"] == "success", response
                assert response["payload"] == {"available": True}, response
                break
        received.result(timeout=8)
    assert not any(wake.glob("*.request")) and not any(wake.glob("*.claimed"))
    assert not any(wake.glob("*.writing")) and not any(wake.glob("*.response"))


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
                line = owner.stdout.readline()
                assert line, owner.stderr.read()
                return json.loads(line)

            try:
                launched = event(start)
                registration = json.loads((wake / "registration.json").read_text(encoding="utf-8"))
                assert registration["owner_pid"] == owner.pid != launched["launcher_pid"], registration
                assert owner.poll() is None
                wake_exchange_contract(adapter, wake, registration)
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

        if "--reply-only" in sys.argv[5:]:
            reply_evidence_contract(helper, adapter, relay, environment, capture)
            print("Claude helper-to-adapter reply evidence checks passed.")
            return

        if "--execution-only" in sys.argv[5:]:
            execution_context_contract(adapter, run, Path(temporary))
            print("Claude helper-to-adapter execution context checks passed.")
            return

        context_adapter = Adapter(adapter)
        try:
            assert {"capability_id": "context-state", "version": 2} in context_adapter.capabilities["capabilities"]
            context = {"used_tokens": 64000, "context_window": 200000}
            result, records, _ = run({"hook_event_name": "ModContext", "session_id": "native-session",
                "context_state": {**context, "unrelated_text": "must remain local"}})
            assert result.returncode == 0 and result.stdout == "", result.stderr
            assert [item["operation"] for item in records] == ["provider-event", "acknowledge"]
            delivery = records[0]["request"]
            assert delivery["event"]["body"]["context_state"] == context
            classified = context_adapter.classify(delivery, 2)
            assert classified["classification"] == "ignore" and classified["observations"] == []
            expected_update = {**context, "provider_session_id": "native-session",
                "event_id": delivery["transition_nonce"]}
            assert classified["context_updates"] == [expected_update]
            assert context_adapter.classify(delivery, 2) == classified, "redelivery changed the native observation"
            for version in (None, 1, "2"):
                unrequested = context_adapter.classify(delivery, version)
                assert "context_updates" not in unrequested and unrequested["classification"] == "ignore"
                assert ("context_failure" in unrequested) == (version is not None)
                if version is not None:
                    assert "provider_session_id" not in unrequested["context_failure"]
            for malformed in ({"used_tokens": None, "context_window": 200000},
                              {**context, "usage_unavailable": "unexpected native content"}):
                result, records, _ = run({"hook_event_name": "ModTurnComplete", "session_id": "native-session",
                    "reason": "answer", "context_state": malformed})
                assert result.returncode == 0, result.stderr
                invalid = context_adapter.classify(records[0]["request"], 2, "replacement-session")
                assert invalid["classification"] == "event" and len(invalid["observations"]) == 1
                assert "context_updates" not in invalid and "context_failure" in invalid
                assert invalid["context_failure"]["provider_session_id"] == "native-session"
            result, records, _ = run({"hook_event_name": "ModContext", "session_id": "native-session",
                "context_state": None})
            assert result.returncode == 0, result.stderr
            invalid = context_adapter.classify(records[0]["request"], 2, "replacement-session")
            assert invalid["classification"] == "ignore" and invalid["observations"] == []
            assert invalid["context_failure"]["provider_session_id"] == "native-session"
            result, records, _ = run({"hook_event_name": "ModContext", "session_id": "native-session",
                "context_state": {"usage_unavailable": True}})
            assert result.returncode == 0, result.stderr
            unavailable = context_adapter.classify(records[0]["request"], 2)
            assert unavailable["context_updates"] == [{"provider_session_id": "native-session",
                "event_id": records[0]["request"]["transition_nonce"], "usage_unavailable": True}]
        finally:
            context_adapter.close()
        if "--context-only" in sys.argv[5:]:
            print("Claude helper-to-adapter context checks passed.")
            return

        for mode in ("accepted", "refused", "transport", "stall"):
            result, records, elapsed = run(prompt, CLAUDE_TEST_LIFECYCLE=mode)
            assert result.returncode == 0, result.stderr
            assert json.loads(result.stdout) == expected
            assert records[0]["operation"] == "request-title-instruction"
            assert records[0]["request"]["changed_files_requested"] is True
            assert records[0]["request"]["delivery"] == {"version": 1, "kind": "model-context"}
            deliveries = [item for item in records if item["operation"] == "provider-event"]
            assert deliveries and all(item == deliveries[0] for item in deliveries)
            acknowledgments = [item for item in records if item["operation"] == "acknowledge"]
            assert bool(acknowledgments) == (mode in ("accepted", "refused"))
            if mode == "stall":
                assert elapsed < 17, "cooperatively terminated fixture exhausted the native relay deadline"

        for mode in ("empty", "refused"):
            result, records, _ = run(prompt, CLAUDE_TEST_CONTEXT=mode)
            assert result.returncode == 0 and result.stdout == ""
        checkpoint = {"hook_event_name": "PreToolUse", "session_id": "native-session", "tool_name": "Bash",
                      "tool_input": {"command": "private tool input"}}
        result, records, _ = run(checkpoint, CLAUDE_TEST_CONTEXT="peer")
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout) == {"hookSpecificOutput": {"hookEventName": "PreToolUse",
            "additionalContext": "Peer session parser-review: the requested parser result is ready."}}
        assert len(records) == 1 and records[0]["operation"] == "request-title-instruction"
        request = records[0]["request"]
        assert request["hook_event_name"] == "PreToolUse" and request["changed_files_requested"] is False
        assert request["delivery"] == {"version": 1, "kind": "model-context"} and "tool_input" not in request
        result, records, _ = run({**checkpoint, "agent_id": "child"})
        assert result.returncode == 0 and result.stdout == "" and not records
        for tool in ("AskUserQuestion", "ExitPlanMode"):
            result, records, _ = run({**checkpoint, "tool_name": tool})
            assert result.returncode == 0 and result.stdout == "", result.stderr
            assert [item["operation"] for item in records] == ["provider-event", "acknowledge"]
            assert records[0]["request"]["event"]["body"]["tool_name"] == tool
        hooks = json.loads((relay.parent.parent / "hooks" / "hooks.json").read_text(encoding="utf-8"))["hooks"]
        assert len(hooks["PreToolUse"]) == 1 and "matcher" not in hooks["PreToolUse"][0]
        assert hooks["PostToolUse"][0]["matcher"] == "^(AskUserQuestion|ExitPlanMode)$"
        if "--skip-reply" not in sys.argv[5:]:
            reply_evidence_contract(helper, adapter, relay, environment, capture)
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
        if os.name == "nt":
            root = Path(temporary).resolve()
            for directory_length in (241, 320):
                component_length = directory_length - len(str(root)) - 47
                assert 0 < component_length <= 255
                component = ("é" if directory_length == 320 else "p") + ("p" * (component_length - 1))
                long_root = root / component
                long_root.mkdir()
                wake = long_root / "profile" / "wake" / ("a" * 32)
                assert len(str(wake)) == directory_length and len(str(wake / (("a" * 32) + ".writing"))) > 260
                wake_owner_contract(helper, adapter, relay, environment, long_root)

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
