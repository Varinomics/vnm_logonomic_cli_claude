"""Exercise native Windows profile selection through the installed adapter."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from claude_helper_tests import Adapter


def account_contract(adapter, root, selected, sibling):
    for profile in (selected, sibling):
        profile.mkdir()
        (profile / ".credentials.json").write_text("{}", encoding="utf-8")
    assert not selected.samefile(sibling), "the filesystem must provide two distinct native profiles"
    program = root / "bin"
    program.mkdir()
    (program / "claude.cmd").touch()
    launcher = {"script_body": "claude", "runner_key": "direct", "environment_text": ""}

    def environment(profile):
        return {"Path": str(program), "UserProfile": str(root), "Claude_Config_Dir": str(profile)}

    def accounts(profile, operation):
        result = adapter.invoke("accounts", 2, operation, {
            "launcher": launcher, "launch_environment": {"platform": "windows-x86_64",
                "effective_environment": environment(profile)},
            "normalization_directory": str(root), "default_home_fallback": str(root)})
        assert result["status"] == "success", result
        return result["payload"]

    inventory = accounts(selected, "discover")["accounts"]
    keys = {row["profile_path"]: row["adapter_account_key"] for row in inventory}
    assert set(keys) == {str(selected), str(sibling)}, "both native profiles must be discoverable"
    assert keys[str(selected)] != keys[str(sibling)], "distinct accounts require distinct keys"
    assert accounts(str(selected) + "\\", "discover") == {"accounts": inventory}
    for profile in (selected, sibling):
        assert accounts(profile, "resolve")["adapter_account_key"] == keys[str(profile)]

    result = adapter.invoke("launcher-discovery", 1, "discover", {
        "platform": "windows-x86_64", "ambient_environment": environment(selected),
        "default_home_fallback": str(root)})
    assert result["status"] == "success", result
    candidates = result["payload"]["candidates"]
    assert len({row["candidate_id"] for row in candidates}) == 2, candidates
    assert [row["environment_text"] for row in candidates if row["default"]] == [
        "CLAUDE_CONFIG_DIR=" + str(selected)], candidates

    def prepare(profile, account):
        return adapter.invoke("launch-preparation", 3, "prepare", {
            "launcher": launcher, "launcher_argv": ["claude"], "working_directory": str(root),
            "title_instruction": "Set the session title.", "declared_assets": [],
            "account_context": account, "launch_environment": {
                "platform": "windows-x86_64", "effective_environment": environment(profile)}})

    for profile in (selected, sibling):
        current = {"version": 2, "profile_path": str(profile), "adapter_account_key": keys[str(profile)]}
        result = prepare(selected, current)
        assert result["status"] == "success" and result["payload"]["account_context"] == current, result
        assert result["payload"]["environment"]["set"]["CLAUDE_CONFIG_DIR"] == str(profile), result

    historical_key = "profile-" + hashlib.sha256(str(selected).lower().encode()).hexdigest()
    historical = {"version": 2, "profile_path": str(selected), "adapter_account_key": historical_key}
    assert historical_key not in keys.values(), "neither native sibling may inherit a shared key"
    result = prepare(selected, historical)
    assert result["status"] == "success" and result["payload"]["account_context"] == historical, result
    assert prepare(sibling, historical)["status"] != "success", "historical grants keep the selected native path"
    foreign = {"version": 2, "profile_path": str(sibling), "adapter_account_key": keys[str(selected)]}
    assert prepare(sibling, foreign)["status"] != "success", "a current key cannot identify a different native path"
    assert prepare(selected, {"version": 1, "adapter_account_key": historical_key})["status"] != "success"


def normalized_directory_contract(adapter, root):
    composed = root / ".claude-Caf\u00e9"
    decomposed = root / ".claude-Cafe\u0301"
    composed.mkdir()
    decomposed.mkdir(exist_ok=True)
    (composed / ".credentials.json").write_text("{}", encoding="utf-8")
    program = root / ("claude.cmd" if os.name == "nt" else "claude")
    program.touch()
    program.chmod(0o755)
    platform = "windows-x86_64" if os.name == "nt" else ("macos-arm64" if sys.platform == "darwin" else "linux-x86_64")
    launcher = {"script_body": "claude", "runner_key": "direct", "environment_text": ""}
    environment = {"platform": platform, "effective_environment": {
        "PATH": str(root), "CLAUDE_CONFIG_DIR": str(decomposed), "HOME": str(root)}}
    request = {"launcher": launcher, "launch_environment": environment,
               "normalization_directory": str(root), "default_home_fallback": str(root)}
    records = adapter.invoke("accounts", 2, "discover", request)["payload"]["accounts"]
    assert len(records) == 1 and records[0]["profile_path"] == str(composed), records
    account = {"version": 2, "profile_path": str(composed), "adapter_account_key": records[0]["adapter_account_key"]}
    assert adapter.invoke("accounts", 2, "resolve", request)["payload"]["adapter_account_key"] == account["adapter_account_key"]
    discovery = adapter.invoke("launcher-discovery", 1, "discover", {"platform": platform,
        "ambient_environment": environment["effective_environment"], "default_home_fallback": str(root)})
    candidates = discovery["payload"]["candidates"]
    assert len(candidates) == 1 and candidates[0]["environment_text"] == "CLAUDE_CONFIG_DIR=" + str(composed), candidates

    def prepare(grant):
        return adapter.invoke("launch-preparation", 3, "prepare", {"launcher": launcher, "launcher_argv": ["claude"],
            "working_directory": str(root), "title_instruction": "Set the session title.", "declared_assets": [],
            "account_context": grant, "launch_environment": environment})

    result = prepare(account)
    assert result["status"] == "success" and result["payload"]["environment"]["set"]["CLAUDE_CONFIG_DIR"] == str(composed), result
    original = str(decomposed)
    old_bytes = (original.lower() if os.name == "nt" else original).encode()
    old_account = {"version": 2, "profile_path": original, "adapter_account_key": "profile-" + hashlib.sha256(old_bytes).hexdigest()}
    old = prepare(old_account)
    if decomposed.samefile(composed):
        assert old["status"] == "success" and old["payload"]["account_context"] == old_account, old
    else:
        assert old["status"] != "success", "a retained source from a distinct directory cannot be reassigned"
        assert prepare({**account, "profile_path": original})["status"] != "success"

    # The real CLI writes its transcript to the composed profile on NTFS and
    # ext4. Give the other directory unrelated content for the same session ID.
    session = "39ece48b-c745-47d4-ad08-88b7b0d31534"
    def transcript(profile, text):
        project = profile / "projects" / "fixture"
        project.mkdir(parents=True)
        row = {"type": "user", "sessionId": session, "parentUuid": None, "isSidechain": False,
               "uuid": "9a4acceb-055c-4ee8-ae8d-a62d83bcc2e9", "message": {"role": "user", "content": text}}
        (project / (session + ".jsonl")).write_text(json.dumps(row) + "\n", encoding="utf-8")
    transcript(composed, "native profile")
    if not decomposed.samefile(composed):
        transcript(decomposed, "other profile")
    out = root / "export"
    out.mkdir()
    exported = adapter.invoke("conversation-export", 1, "export", {"provider_session_id": session,
        "working_directory": str(root), "launch_environment": environment, "output_directory": str(out), "format": "text"})
    assert exported["status"] == "success", exported
    contents = (out / exported["payload"]["file_name"]).read_text(encoding="utf-8")
    assert "native profile" in contents and "other profile" not in contents, contents


def main():
    executable, mode = Path(sys.argv[1]), sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="claude-native-accounts-") as temporary:
        root = Path(temporary).resolve()
        if mode == "nfc":
            adapter = Adapter(executable)
            try:
                normalized_directory_contract(adapter, root)
            finally:
                adapter.close()
            return 0
        parent = root / "profiles"
        parent.mkdir()
        if mode == "sensitive":
            result = subprocess.run(["fsutil", "file", "setCaseSensitiveInfo", str(parent), "enable"],
                                    capture_output=True)
            if result.returncode:
                print("The fixture volume cannot enable per-directory case sensitivity.")
                return 77
            selected, sibling = parent / ".claude-Work", parent / ".claude-work"
        else:
            # Unicode invariant lowercase combines these supplementary scalars;
            # the native ordinal lookup on NTFS gives them separate directories.
            selected, sibling = parent / ".claude-\U00010400", parent / ".claude-\U00010428"
        adapter = Adapter(executable)
        try:
            account_contract(adapter, root, selected, sibling)
        finally:
            adapter.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
