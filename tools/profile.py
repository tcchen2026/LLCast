#!/usr/bin/env python3
"""Collect LLCast's opt-in TCP profile stream; only this process writes CSVs."""
import argparse
from collections import OrderedDict
import csv
import json
from pathlib import Path
import re
import select
import socket
import sys
import threading
import time

FATES = ('unfinished', 'shown', 'replaced', 'lost', 'skipped', 'decode_error', 'delivered', 'discarded')
NAME = re.compile(r'[A-Za-z0-9_-]{1,63}\Z')


def endpoint(value, default='127.0.0.1'):
    if value.startswith('['):
        host, sep, port = value[1:].partition(']:')
    else:
        host, sep, port = value.rpartition(':')
    if not sep:
        host, port = default, value
    if not host or not port.isdecimal() or not 0 <= int(port) <= 65535:
        raise ValueError('Expected [HOST:]PORT')
    return host, int(port)


class Sink:
    def __init__(self, directory=None, quiet=False):
        self.directory = Path(directory) if directory else None
        if self.directory:
            self.directory.mkdir(parents=True, exist_ok=True)
        self.quiet = quiet
        self.files, self.writers, self.schemas = {}, {}, {}
        self.seen = OrderedDict()  # recent service instances, bounded across endless restarts
        self.rows = 0
        self.events = (self.directory / 'profile.jsonl').open('a') if self.directory else None

    def event(self, text):
        if self.events:
            self.events.write(json.dumps({'received_unix_ns': time.time_ns(), 'message': text}) + '\n')
        if not self.quiet:
            print(text, flush=True)

    def remember(self, key, sequence):
        self.seen[key] = max(sequence, self.seen.get(key, -1))
        self.seen.move_to_end(key)
        while len(self.seen) > 1024:
            self.seen.popitem(last=False)

    def cursor(self, client, name, sequence):
        key = (client.get('instance'), name)
        expected = max(client['cursors'].get(name, 0), self.seen.get(key, -1) + 1)
        if sequence > expected:
            self.event(f'#skipped {name} {sequence - expected}')
            if key[0] is not None:
                self.remember(key, sequence - 1)
        client['cursors'][name] = sequence

    def line(self, client, text):
        if text.startswith('#llcast-profile '):
            parts = text.split()
            if len(parts) < 4 or parts[1] != '1':
                raise ValueError('Unsupported profile version')
            fields = dict(item.split('=', 1) for item in parts[2:])
            client.update(instance=fields.get('instance'), program=fields.get('program'), schemas={}, cursors={})
            self.event(text)
        elif text.startswith('#table '):
            _, name, columns = text.split(' ', 2)
            fields = columns.split(',')
            if not NAME.fullmatch(name) or not 1 <= len(fields) <= 64 or not all(NAME.fullmatch(f) for f in fields):
                raise ValueError('Invalid profile table')
            if len(client['schemas']) >= 8 and name not in client['schemas']:
                raise ValueError('Too many tables')
            if name in self.schemas and self.schemas[name] != fields:
                raise ValueError('Changed schema for table ' + name)
            client['schemas'][name] = self.schemas[name] = fields
            if self.directory and name not in self.writers:
                path = self.directory / (name + '.csv')
                exists = path.exists() and path.stat().st_size
                if exists:
                    with path.open(newline='') as old:
                        if next(csv.reader(old), None) != fields:
                            raise ValueError('Existing CSV has a different schema: ' + str(path))
                stream = path.open('a', newline='')
                self.files[name] = stream
                self.writers[name] = csv.writer(stream)
                if not exists:
                    self.writers[name].writerow(fields)
        elif text.startswith('#cursor '):
            _, name, sequence = text.split()
            if name not in client['schemas'] or int(sequence) < 0:
                raise ValueError('Invalid profile cursor')
            self.event(text + ' instance=' + str(client.get('instance')))
            self.cursor(client, name, int(sequence))
        elif text.startswith('#skipped '):
            _, name, count = text.split()
            if name not in client['cursors'] or int(count) < 0:
                raise ValueError('Invalid skipped count')
            self.cursor(client, name, client['cursors'][name] + int(count))
        elif text.startswith('#'):
            self.event(text)
        elif text:
            name, *values = text.split(',')
            fields = client['schemas'].get(name)
            if not fields or len(values) != len(fields):
                raise ValueError('Row does not match table schema')
            numbers = [int(value) for value in values]
            if any(n < -(1 << 63) or n >= 1 << 63 for n in numbers):
                raise ValueError('Profile value outside int64')
            if client.get('instance') is not None and name in client['cursors']:
                key = (client['instance'], name)
                sequence = client['cursors'][name]
                client['cursors'][name] += 1
                previous = self.seen.get(key, -1)
                if sequence <= previous:
                    return  # replayed ring history on reconnect
                self.remember(key, sequence)
            if name == 'rx':
                index = fields.index('fate')
                if not 0 <= numbers[index] < len(FATES):
                    raise ValueError('Unknown receiver fate')
                values[index] = FATES[numbers[index]]
            if name in self.writers:
                self.writers[name].writerow(values)
            self.rows += 1

    def flush(self):
        for file in self.files.values():
            file.flush()
        if self.events:
            self.events.flush()

    def close(self):
        for file in self.files.values():
            file.close()
        if self.events:
            self.events.close()


class Collector:
    """Threaded collector used by runners and tests; no connection changes stream state."""
    def __init__(self, *, listen=None, connect=None, save=None, quiet=False, once=False):
        if (listen is None) == (connect is None):
            raise ValueError('Choose listen or connect')
        self.sink = Sink(save, quiet)
        self.target = endpoint(connect) if connect is not None else None
        self.listener = None
        self.address = None
        if listen is not None:
            host, port = endpoint(str(listen))
            self.listener = socket.create_server((host, port), family=socket.AF_INET6 if ':' in host else socket.AF_INET)
            self.listener.setblocking(False)
            self.address = self.listener.getsockname()
        self.stop = threading.Event()
        self.once, self.disconnected, self.errors = once, 0, []
        self.completed_programs = {}
        self.thread = threading.Thread(target=self.run, name='profile-collector', daemon=True)

    @property
    def endpoint(self):
        host, port = self.address[:2]
        return f'[{host}]:{port}' if ':' in host else f'{host}:{port}'

    def start(self):
        self.thread.start()
        return self

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    def run(self):
        clients = {}
        retry_at = accept_at = 0
        def disconnect(stream, client, invalid=False):
            if client['buffer']:
                self.sink.event('#truncated final profile line')
            stream.close()
            del clients[stream]
            self.disconnected += 1
            program = client.get('program')
            self.completed_programs[program] = self.completed_programs.get(program, 0) + 1
            if self.once and program and not invalid:
                self.stop.set()
        try:
            while not self.stop.is_set():
                if self.target and not clients and time.monotonic() >= retry_at:
                    try:
                        stream = socket.create_connection(self.target, timeout=.5)
                        stream.setblocking(False)
                        clients[stream] = dict(buffer=b'', schemas={}, cursors={})
                    except OSError:
                        retry_at = time.monotonic() + 1
                readers = list(clients) + ([self.listener] if self.listener and time.monotonic() >= accept_at else [])
                if not readers:
                    self.stop.wait(.1)
                    continue
                ready, _, _ = select.select(readers, [], [], .1)
                for stream in ready:
                    if stream is self.listener:
                        try:
                            connection, _ = stream.accept()
                        except OSError as exc:
                            if not isinstance(exc, BlockingIOError):
                                self.sink.event('#accept-error ' + str(exc))
                                accept_at = time.monotonic() + .1
                            continue
                        if len(clients) >= 8:
                            connection.close()
                        else:
                            connection.setblocking(False)
                            clients[connection] = dict(buffer=b'', schemas={}, cursors={})
                        continue
                    client = clients[stream]
                    try:
                        data = stream.recv(65536)
                    except BlockingIOError:
                        continue
                    except OSError:
                        data = b''
                    if not data:
                        disconnect(stream, client)
                        retry_at = time.monotonic() + 1
                        continue
                    data = client['buffer'] + data
                    lines = data.split(b'\n')
                    client['buffer'] = lines.pop()
                    try:
                        if len(client['buffer']) > 16384 or any(len(line) > 16384 for line in lines):
                            raise ValueError('Profile line is too long')
                        for line in lines:
                            self.sink.line(client, line.decode('ascii'))
                    except (ValueError, KeyError) as exc:
                        self.sink.event('#truncated invalid profile connection: ' + str(exc))
                        disconnect(stream, client, invalid=True)
                        retry_at = time.monotonic() + 1
                self.sink.flush()
        except (OSError, ValueError, KeyError) as exc:
            self.errors.append(exc)
        finally:
            for stream in clients:
                stream.close()
            if self.listener:
                self.listener.close()
            self.sink.close()

    def wait_disconnect(self, count=1, timeout=5, program=None):
        deadline = time.monotonic() + timeout
        def finished():
            return self.disconnected if program is None else self.completed_programs.get(program, 0)
        while finished() < count and self.thread.is_alive() and time.monotonic() < deadline:
            time.sleep(.01)
        self.check()
        if finished() < count:
            raise RuntimeError('Profile connection did not finish')

    def check(self):
        if self.errors:
            raise RuntimeError('Profile collector: ' + str(self.errors[0]))

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise RuntimeError('Profile collector did not stop')
        self.check()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', nargs='?', help='Service HOST:PORT; retry until interrupted')
    parser.add_argument('--listen', metavar='[ADDR:]PORT',
                        help='Accept outgoing profiles; loopback by default. For a PC, use the VM LAN address, e.g. 192.168.185.100:47801')
    parser.add_argument('--save', type=Path, help='Write one CSV per table in this directory')
    parser.add_argument('--once', action='store_true', help='Explicit test: exit after the first connection ends')
    args = parser.parse_args(argv)
    if bool(args.address) == bool(args.listen):
        parser.error('Choose HOST:PORT or --listen PORT')
    with Collector(listen=args.listen, connect=args.address, save=args.save, once=args.once) as collector:
        if collector.address:
            print('Profile collector listening on ' + collector.endpoint, flush=True)
        try:
            while collector.thread.is_alive():
                collector.thread.join(.5)
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print('ERROR:', error, file=sys.stderr)
        sys.exit(1)
