"""Isolated native Claude stream plus production adapter inspection; no account calls."""
import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from claude_helper_tests import Adapter

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--claude', required=True, type=Path)
parser.add_argument('--adapter', required=True, type=Path)
parser.add_argument('--scratch-root', type=Path)
parser.add_argument('--require-resume-inspection', action='store_true',
    help='Require positive resume and content evidence in a supported native lookup context.')
options = parser.parse_args()
PROGRAM = options.claude.resolve()
ADAPTER = options.adapter.resolve()
class Messages(BaseHTTPRequestHandler):
    requests = []
    def log_message(self, *_): pass
    def do_GET(self):
        self.send_response(404); self.end_headers()
    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append({'path': self.path, 'model': value.get('model')})
        if self.path.split('?')[0].endswith('/messages/count_tokens'):
            body = b'{"input_tokens":1}'
            content_type = 'application/json'
        else:
            assert self.path.split('?')[0].endswith('/messages'), self.path
            message = {'id':'msg_fixture','type':'message','role':'assistant','model':'claude-native-served',
                'content':[], 'stop_reason':None,'stop_sequence':None,'usage':{'input_tokens':1,'output_tokens':1}}
            frames = [
                {'type':'message_start','message':message},
                {'type':'content_block_start','index':0,'content_block':{'type':'text','text':''}},
                {'type':'content_block_delta','index':0,'delta':{'type':'text_delta','text':'NATIVE_MODEL_CHECK_OK'}},
                {'type':'content_block_stop','index':0},
                {'type':'message_delta','delta':{'stop_reason':'end_turn','stop_sequence':None},'usage':{'output_tokens':1}},
                {'type':'message_stop'},
            ]
            body = ''.join('event: '+frame['type']+'\ndata: '+json.dumps(frame)+'\n\n' for frame in frames).encode()
            content_type = 'text/event-stream'
        self.send_response(200)
        self.send_header('Content-Type',content_type)
        self.send_header('Content-Length',str(len(body)))
        self.end_headers()
        self.wfile.write(body)
def check_saved_conversation(adapter, root, profile, session):
    platform = "windows-x86_64" if os.name == "nt" else ("macos-arm64" if sys.platform == "darwin" else "linux-x86_64")
    launcher = {"script_body": '"' + str(PROGRAM) + '"', "runner_key": "direct", "environment_text": ""}
    environment = {"platform": platform, "effective_environment": {
        "PATH": str(PROGRAM.parent), "CLAUDE_CONFIG_DIR": str(profile), "HOME": str(root)},
        "bootstrap": {"version": 1, "resolved_home": str(root)}}
    def checked(capability, version, operation, payload):
        result = adapter.invoke(capability, version, operation, payload)
        assert result["status"] == "success", result
        return result["payload"]
    key = checked("accounts", 2, "resolve", {"launcher": launcher, "launch_environment": environment,
        "normalization_directory": str(root), "default_home_fallback": str(root)})["adapter_account_key"]
    assert key
    account = {"version": 2, "profile_path": str(profile), "adapter_account_key": key}
    context = {"launcher": launcher, "launcher_argv": [str(PROGRAM)], "working_directory": str(root),
        "declared_assets": [], "account_context": account, "launch_environment": environment}
    output = root / "export"
    output.mkdir()
    exported = checked("conversation-export", 1, "export", {"provider_session_id": session,
        "working_directory": str(root), "account_context": account, "launch_environment": environment,
        "output_directory": str(output), "format": "text"})
    contents = (output / exported["file_name"]).read_text(encoding="utf-8")
    assert exported["message_count"] == 2 and "Return the fixture answer." in contents and "NATIVE_MODEL_CHECK_OK" in contents
    state = checked("interactive-resume", 4, "inspect_resume", {**context,
        "resume": {"version": 1, "record": {"provider_session_id": session}}})
    assert state["availability"] in ("available", "unknown"), state
    if options.require_resume_inspection:
        assert state["availability"] == "available", state
    if state["availability"] == "available":
        assert state["content"] == "nonempty", state
    storage = root / "storage"
    candidate = storage / "candidate"
    candidate.mkdir(parents=True)
    usage = checked("usage-scan", 3, "scan", {**context, "profile_storage": {"version": 1, "root": str(storage)},
        "mode": "repair", "cursor": None, "snapshot_id": None, "cache_selection": {
            "version": 1, "accepted": None, "candidate_root": str(candidate)}})
    record, = usage["records"]
    assert record["session_key"] == session and record["model"] == "claude-native-served", record
    assert record["input"]["value"] == 1 and record["output"]["value"] == 1 and record["total_tokens"] == 2, record
    print(json.dumps({"exported_messages": exported["message_count"], "total_tokens": record["total_tokens"],
        "resume_availability": state["availability"], "resume_content": state["content"]}), flush=True)


with tempfile.TemporaryDirectory(prefix='claude-model-native-',dir=options.scratch_root) as temporary:
    root = Path(temporary)
    profile = root/'profile'; profile.mkdir()
    server = ThreadingHTTPServer(('127.0.0.1',0), Messages)
    server.daemon_threads=True
    threading.Thread(target=server.serve_forever,daemon=True).start()
    environment = {key:value for key,value in os.environ.items() if not key.upper().startswith(
        ('CLAUDE_', 'ANTHROPIC_', 'AWS_', 'GOOGLE_', 'AZURE_', 'LOGONOMIC_', 'OTEL_'))}
    environment.update({'HOME':str(root),'USERPROFILE':str(root),'APPDATA':str(root/'appdata'),
        'LOCALAPPDATA':str(root/'localappdata'),'CLAUDE_CONFIG_DIR':str(profile),
        'ANTHROPIC_BASE_URL':f'http://127.0.0.1:{server.server_port}',
        'ANTHROPIC_API_KEY':'logonomic-scripted-no-account', 'CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC':'1',
        'CLAUDE_CODE_MAX_RETRIES':'0', 'HTTP_PROXY':'http://127.0.0.1:9', 'HTTPS_PROXY':'http://127.0.0.1:9',
        'http_proxy':'http://127.0.0.1:9','https_proxy':'http://127.0.0.1:9',
        'NO_PROXY':'127.0.0.1,localhost','no_proxy':'127.0.0.1,localhost'})
    try:
        print(subprocess.run([PROGRAM,'--version'],cwd=root,env=environment,stdin=subprocess.DEVNULL,
            capture_output=True,text=True,check=True,timeout=45).stdout.strip(),flush=True)
        process = subprocess.run([PROGRAM,'--bare','--print','--verbose','--output-format','stream-json',
            '--model','claude-scripted','--tools','','--permission-mode','plan','--strict-mcp-config',
            '--mcp-config','{"mcpServers":{}}','--disable-slash-commands',
            'Return the fixture answer.'],cwd=root,env=environment,stdin=subprocess.DEVNULL,capture_output=True,timeout=45)
        assert process.returncode==0,process.stderr.decode(errors='replace')
        records=[json.loads(line) for line in process.stdout.splitlines() if line.strip()]
        init=next(value for value in records if value.get('type')=='system' and value.get('subtype')=='init')
        served=next(value for value in records if value.get('type')=='assistant')
        assert init['model']=='claude-scripted',init
        assert served['message']['model']=='claude-native-served' and served['session_id']==init['session_id'],served
        adapter=Adapter(ADAPTER)
        try:
            state=json.dumps({'initialized':False,'pending_base64':'','result_base64':'',
                'root_session_id':'','working_directory':str(root),'task_tools':[],'expected_mcp_servers':[]})
            enc=lambda value:[base64.b64encode(value).decode()]
            payload={'purpose':'implementation','inspection':{'protocol_state_base64_chunks':enc(state.encode()),
                'stdout_base64_chunks':enc(process.stdout),'stderr_base64_chunks':[],
                'final':True,'timed_out':False,'exit_code':0},'model_state':{'version':1}}
            result=adapter.invoke('structured-session',3,'inspect',payload)
            assert result['status']=='success',result
            assert 'model_state' in result['payload'], result
            observed=result['payload']['model_state']['observation']
            assert observed=={'model':'claude-native-served','source':'claude.assistant.message'},result
            assert result['payload']['complete'] is True and 'failure_reason' not in result['payload'],result
            print('PASS: native init and served response differ; production structured inspector preserves own served model',flush=True)
            print(json.dumps({'init_model':init['model'],'served_model':served['message']['model'],
                'observation':observed,'requests':Messages.requests,'scope':'native headless CLI + scripted loopback model + production adapter'}),flush=True)
            check_saved_conversation(adapter, root, profile, init['session_id'])
        finally:
            if adapter.process.poll() is None: adapter.close()
    finally:server.shutdown()
