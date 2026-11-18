"""Exercise native Windows profile selection through the installed adapter."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

from claude_helper_tests import Adapter


def invoke(adapter, capability, version, operation, payload):
    adapter.serial += 1
    request_id = f"r{adapter.serial}"
    adapter.send({"type": "invoke", "request_id": request_id, "capability_id": capability,
                 "version": version, "operation": operation, "deadline_ms": 5000, "payload": payload})
    while True:
        result = adapter.read()
        if result["type"] == "result":
            assert result["request_id"] == request_id
            return result


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
        result = invoke(adapter, "accounts", 2, operation, {
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

    result = invoke(adapter, "launcher-discovery", 1, "discover", {
        "platform": "windows-x86_64", "ambient_environment": environment(selected),
        "default_home_fallback": str(root)})
    assert result["status"] == "success", result
    candidates = result["payload"]["candidates"]
    assert len({row["candidate_id"] for row in candidates}) == 2, candidates
    assert [row["environment_text"] for row in candidates if row["default"]] == [
        "CLAUDE_CONFIG_DIR=" + str(selected)], candidates

    def prepare(profile, account):
        return invoke(adapter, "launch-preparation", 3, "prepare", {
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


def main():
    executable, mode = Path(sys.argv[1]), sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="claude-native-accounts-") as temporary:
        root = Path(temporary).resolve()
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
