"""Loopback-only CPR regression: never downloads or installs a device package."""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--runtime-bin', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    state = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            assert self.path == '/stream-logs/wire-test'
            case = dict(state)
            if case.get('late_connection'):
                time.sleep(0.25)
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            assert case['uploaded'].wait(4)
            if case['mode'] == 'sse-silent':
                case['close_stream'].wait(5)
                return
            time.sleep(0.3)  # Outcome is queued, but delivered after POST completed.
            code = 75 if case['mode'] == 'sse-75' else 1
            entries = [
                {'source': 'execute_script', 'level': 'ERROR',
                 'message': 'Script setup.sh failed with return code ' + str(code)},
                {'source': 'process_archive', 'level': 'ERROR',
                 'message': 'failed_rollbacked; rollback attempt finished'},
                {'source': 'system', 'level': 'SYSTEM', 'message': '__STREAM_END__'}]
            for entry in entries:
                self.wfile.write(('data: ' + json.dumps(entry) + '\n\n').encode())
                self.wfile.flush()

        def do_POST(self):
            data = self.rfile.read(int(self.headers['Content-Length']))
            assert self.headers['Authorization'] == 'Bearer wire-only-localfixture'
            if self.path == '/prepare-transfer':
                body = json.loads(data)
                assert body['filename'].startswith('tv1-updater-migration-dev-')
                status, response = 200, {'status': 'ready', 'transfer_id': 'wire-test'}
            else:
                assert self.path == '/transfer/wire-test'
                state['uploads'] = state.get('uploads', 0) + 1
                if state['mode'] == 'reset':
                    self.connection.shutdown(socket.SHUT_RDWR)
                    self.connection.close()
                    return
                status = 500
                if state['mode'].startswith('sse-'):
                    response = {'detail': 'Archive processing or script execution failed.'}
                elif state['mode'] == 'unknown':
                    response = {'detail': 'status failed_rollbacked; rollback attempt finished'}
                else:
                    response = {'detail': 'setup.sh from package root failed. Status: failed, RC: ' + state['mode']}
            encoded = json.dumps(response).encode()
            self.send_response(status)
            self.send_header('Content-Length', str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)
            if self.path.startswith('/transfer/'):
                state['uploaded'].set()

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    env = os.environ.copy()
    env['PATH'] = ';'.join((str(args.runtime_bin.resolve()), str(Path(os.environ['SystemRoot'])/'System32'), os.environ['SystemRoot']))
    results = []
    try:
        with tempfile.TemporaryDirectory(prefix='migration-upload-') as folder:
            payload = Path(folder)/'fixture.tar.xz'
            payload.write_bytes(b'Never install: loopback transfer-response fixture')
            digest = hashlib.sha256(payload.read_bytes()).hexdigest()
            for mode, expected, late in [('1', 'failed', False), ('75', 'pending', False), ('unknown', 'pending', False), ('reset', 'pending', False), ('sse-1', 'sse-failed', False), ('sse-1', 'sse-failed', True), ('sse-75', 'sse-pending', True), ('sse-silent', 'sse-pending', False)]:
                state.clear()
                state.update(mode=mode, late_connection=late, uploaded=threading.Event(), close_stream=threading.Event())
                before = time.monotonic()
                result = subprocess.run([str(args.probe.resolve()), str(server.server_port), str(payload), digest,
                    'updater-migration', 'dev', 'b'*64, expected], capture_output=True, text=True, env=env, timeout=10)
                elapsed = time.monotonic()-before
                assert result.returncode == 0, (mode, result.returncode, result.stdout, result.stderr)
                state['close_stream'].set()
                assert state.get('uploads') == 1
                assert elapsed < 5, (mode, elapsed)
                if mode in ('sse-1', 'sse-75'):
                    assert ('setup=1' if mode == 'sse-1' else 'setup=75') in result.stdout, result.stdout
                if mode == 'sse-silent':
                    assert 'setup=unknown' in result.stdout, result.stdout
                results.append({'late_connection': late, 'mode': mode, 'expected': expected, 'seconds': elapsed, 'stdout': result.stdout.strip(), 'passed': True})
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    report = {'passed': True, 'scope': 'Real CPR prepare/multipart/SSE drain/error classifier loopback; pinned generic500, queued delayed outcomes and silent stream; no GUI, cloud, device, installation or durable-success claim', 'cases': results}
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
