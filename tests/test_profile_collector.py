import csv
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from profile import Collector, Sink


class ProfileTest(unittest.TestCase):
    def test_cursor_gaps_and_bad_peer_do_not_end_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            with Collector(listen='0', save=directory, quiet=True) as collector:
                self.assertEqual(collector.address[0], '127.0.0.1')
                for i, text in enumerate((
                    '#llcast-profile 1 program=test clock=c instance=abc\n#table sender frame\n#cursor sender 0\nsender,0\nsender,1\nsender,2\n',
                    '#llcast-profile 1 program=test clock=c instance=abc\n#table sender frame\n#cursor sender 10\nsender,10\nsender,11\n',
                    '\xffbad peer\n',
                    '#llcast-profile 1 program=test clock=c instance=late\n#table sender frame\n#cursor sender 4\nsender,4\n',
                ), 1):
                    with socket.create_connection(collector.address) as stream:
                        stream.sendall(text.encode('latin1'))
                    collector.wait_disconnect(i)
                self.assertTrue(collector.thread.is_alive())
            events = (Path(directory) / 'profile.jsonl').read_text()
            self.assertIn('#skipped sender 7', events)
            self.assertIn('#skipped sender 4', events)
            self.assertIn('#cursor sender 10 instance=abc', events)
            self.assertIn('#truncated invalid profile connection', events)
            self.assertEqual((Path(directory) / 'sender.csv').read_text(), 'frame\n0\n1\n2\n10\n11\n4\n')

    def test_reconnect_history_skips_and_fate(self):
        with tempfile.TemporaryDirectory() as directory:
            with Collector(listen='127.0.0.1:0', save=directory, quiet=True) as collector:
                for i, body in enumerate((
                    '#cursor rx 10\nrx,7,0,6\nrx,7,1,3\n',
                    '#cursor rx 10\nrx,7,0,6\nrx,7,1,3\n#skipped rx 2\nrx,7,4,1\n',
                ), 1):
                    with socket.create_connection(collector.address) as stream:
                        stream.sendall(('#llcast-profile 1 program=test clock=MONOTONIC_ns instance=abc\n'
                                        '#table rx session,frame,fate\n' + body).encode())
                    collector.wait_disconnect(i)
            with (Path(directory) / 'rx.csv').open() as file:
                rows = list(csv.DictReader(file))
            self.assertEqual([r['frame'] for r in rows], ['0', '1', '4'])
            self.assertEqual([r['fate'] for r in rows], ['delivered', 'lost', 'shown'])
            self.assertIn('#skipped rx 2', (Path(directory) / 'profile.jsonl').read_text())

    def test_cli_save(self):
        with tempfile.TemporaryDirectory() as directory, socket.create_server(('127.0.0.1', 0)) as server:
            server.settimeout(5)
            script = Path(__file__).resolve().parents[1] / 'tools/profile.py'
            process = subprocess.Popen([sys.executable, str(script), f'127.0.0.1:{server.getsockname()[1]}',
                                        '--save', directory, '--once'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                connection, _ = server.accept()
                with connection:
                    for part in (b'#llcast-profile 1 program=test clock=test instance=1\n#table sender frame,',
                                 b'bytes\n#cursor sender 0\nsender,0,', b'123\n#summary records=1\n'):
                        connection.sendall(part)
                out, error = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, error)
                self.assertIn(b'#summary records=1', out)
                self.assertEqual((Path(directory) / 'sender.csv').read_text(), 'frame,bytes\n0,123\n')
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def test_rejects_paths_schema_and_overflow(self):
        sink = Sink(quiet=True)
        client = dict(schemas={}, cursors={})
        for line in ('#table ../escape frame', '#table x ' + ','.join(['n'] * 65)):
            with self.assertRaises(ValueError):
                sink.line(client, line)
        sink.line(client, '#table x n')
        for line in ('x,9223372036854775808', 'x,1,2', '#table x changed'):
            with self.assertRaises(ValueError):
                sink.line(client, line)
        sink.close()


if __name__ == '__main__':
    unittest.main()
