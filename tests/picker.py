"""Exercise the real native picker through a controlling PTY, without fzf."""
import contextlib
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import sqlite3
import struct
import subprocess
import tempfile
import termios
import time

BINARY = str(Path('build/atchim').resolve())


class Picker:
    def __init__(self, arguments, env, rows=40, columns=120):
        self.master, self.slave = pty.openpty()
        self.resize(rows, columns, notify=False)
        self.before = termios.tcgetattr(self.slave)
        self.output = bytearray()
        self.reports = 0

        def terminal():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.process = subprocess.Popen(
            [BINARY, *arguments], stdin=self.slave,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, TERM='xterm-256color', **env), preexec_fn=terminal,
        )

    def resize(self, rows, columns, notify=True):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        if notify:
            os.kill(self.process.pid, signal.SIGWINCH)

    def pump(self):
        ready, _, _ = select.select([self.master], [], [], 0.05)
        if ready:
            self.output.extend(os.read(self.master, 65536))
            reports = self.output.count(b'\x1b[6n')
            if reports > self.reports:
                os.write(self.master, b'\x1b[1;1R' * (reports - self.reports))
                self.reports = reports

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise AssertionError(f'Picker timeout; rc={self.process.poll()}; UI tail={bytes(self.output[-2500:])!r}')
            self.pump()

    def wait(self, value):
        self.until(lambda: value in self.output)

    def send(self, value):
        # Finish outstanding terminal queries before injecting a user key. This
        # also avoids accidentally joining Escape with a cursor-report reply.
        deadline = time.monotonic() + 2
        quiet = time.monotonic() + 0.15
        while time.monotonic() < quiet and time.monotonic() < deadline:
            before = len(self.output)
            self.pump()
            if len(self.output) != before:
                quiet = time.monotonic() + 0.15
        self.output.clear()
        self.reports = 0
        os.write(self.master, value)

    def finish(self, key, expected=0):
        self.send(key)
        self.until(lambda: self.process.poll() is not None)
        stdout, stderr = self.process.communicate(timeout=2)
        assert self.process.returncode == expected, (self.process.returncode, stderr)
        assert termios.tcgetattr(self.slave) == self.before, 'Terminal settings were not restored'
        assert b'\x1b[?1049h' not in self.output, 'Picker entered the full-screen alternate buffer'
        return stdout

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.communicate()
        os.close(self.slave)
        os.close(self.master)


@contextlib.contextmanager
def picker(arguments, env, **size):
    instance = Picker(arguments, env, **size)
    try:
        yield instance
    finally:
        instance.close()


with tempfile.TemporaryDirectory() as tmp:
    database = str(Path(tmp) / 'history.db')
    env = {'ATCHIM_DB': database, 'XDG_CONFIG_HOME': tmp, 'ATCHIM_HEIGHT': '65%',
           'ATCHIM_BIN': BINARY, 'ATCHIM_DAEMON': '0', 'ATCHIM_RECORD': '1',
           'ATCHIM_IGNORE': '', 'ATCHIM_THEME': 'midnight'}
    subprocess.run([BINARY, 'log', '--', 'initialize'], env=os.environ | env, check=True)
    exact = "printf 'héllo\t世界'\necho done\n\n"
    with sqlite3.connect(database) as db:
        db.execute('DELETE FROM HISTORY')
        db.execute('INSERT INTO HISTORY(COMMAND,HOST,CWD,EXIT_STATUS) VALUES(?,?,?,?)',
                   (exact, 'laptop', str(Path.cwd()), 7))
        # Put the target beyond the old 5,000-entry window and use a unique ID
        # outside 32-bit range to guard against selection through truncated IDs.
        db.executemany('INSERT INTO HISTORY(COMMAND,HOST,CWD) VALUES(?,?,?)',
                       [(f'newer command {i}', 'remote', '/other') for i in range(6000)])
        db.execute("INSERT INTO HISTORY(ID,COMMAND,HOST,CWD) VALUES(3000000000,'literal%_ marker','laptop','/other')")

    with picker(['pick', '--details', '--', 'héllo'], env) as ui:
        ui.wait(b'1 matches')
        ui.wait(b'COMMAND DETAILS')
        ui.wait(b'exit 7')
        assert b' ago' not in ui.output
        assert '╭'.encode() not in ui.output
        assert b'\x1b[?1049h' not in ui.output
        assert ui.finish(b'\r') == exact.encode()
    for key, action in [(b'\r', b'execute:'), (b'\t', b'insert:'), (b'\x19', b'execute:')]:
        with picker(['pick', '--shell', '--', 'héllo'], env) as ui:
            ui.wait(b'1 matches')
            assert ui.finish(key) == action + exact.encode()
    with picker(['pick', '--', 'literal%_'], env) as ui:
        ui.wait(b'1 matches')
        assert ui.finish(b'\r') == b'literal%_ marker'

    # Incremental search can reach old commands without loading the full table.
    with picker(['pick'], env) as ui:
        ui.wait(b'200 newest matches')
        ui.send('héllo'.encode())
        ui.wait(b'1 matches')
        assert ui.finish(b'\r') == exact.encode()

    with picker(['pick', '--cwd'], env) as ui:
        ui.wait(b'1 matches')
        ui.send(b'\x04')  # All directories.
        ui.wait(b'200 newest matches')
        ui.send(b'\x04')  # Current directory again.
        ui.wait(b'1 matches')
        assert ui.finish(b'\r') == exact.encode()

    with picker(['pick'], env) as ui:
        ui.wait(b'200 newest matches')
        ui.send(b'\x14')  # Host of selected entry: laptop.
        ui.wait(b'2 matches')
        ui.send(b'\x1b[B')
        # Wait until the selection has moved, then accept the multiline command.
        ui.until(lambda: len(ui.output) > 10)
        assert ui.finish(b'\r') == exact.encode()

    # Resize with details visible, including a terminal too small for all widgets.
    with picker(['pick', '--details', '--', 'héllo'], env, rows=24, columns=70) as ui:
        ui.wait(b'1 matches')
        ui.resize(10, 30)
        ui.until(lambda: len(ui.output) > 100)
        ui.resize(35, 110)
        assert ui.finish(b'\x1b', expected=130) == b''
    with picker(['pick', '--', 'no such command'], env) as ui:
        ui.wait(b'0 matches')
        assert ui.finish(b'\x03', expected=130) == b''
    with picker(['pick'], env) as ui:
        ui.wait(b'200 newest matches')
        ui.process.terminate()
        ui.until(lambda: ui.process.poll() is not None)
        assert termios.tcgetattr(ui.slave) == ui.before

    # Theme selection is native too; no fzf executable is involved.
    with picker(['themes', 'pick'], env) as ui:
        ui.wait(b'catppuccin-mocha')
        ui.finish(b'\x1b', expected=0)
        assert not (Path(tmp) / 'atchim/theme').exists()
    with picker(['themes', 'pick'], env) as ui:
        ui.wait(b'catppuccin-mocha')
        ui.finish(b'\r')
        assert (Path(tmp) / 'atchim/theme').read_text() == 'catppuccin-mocha\n'

    # Test the generated widget's byte-preserving handoff, without a live ZLE.
    script = '''eval "$("$ATCHIM_BIN" init zsh)"
function zle() { return 0; }
function __atchim_search_cmd() { printf 'insert:%s' "$EXACT_COMMAND"; }
BUFFER=old
_atchim_search
print -rn -- "$LBUFFER"
'''
    result = subprocess.run(['zsh', '-f', '-c', script], env=os.environ | env | {'EXACT_COMMAND': exact},
                            capture_output=True, check=True)
    assert result.stdout == exact.encode(), result.stdout

    legacy = str(Path(tmp) / 'legacy.db')
    with sqlite3.connect(legacy) as db:
        db.execute('CREATE TABLE HISTORY(ID INTEGER PRIMARY KEY,COMMAND TEXT,HOST TEXT,TIME TEXT)')
        db.execute("INSERT INTO HISTORY VALUES(1,'old command','local','2020-01-01')")
    with picker(['pick', '--details'], env | {'ATCHIM_DB': legacy}) as ui:
        ui.wait(b'1 matches')
        ui.wait(b'unknown')
        assert ui.finish(b'\r') == b'old command'
    with picker(['pick'], env | {'ATCHIM_DB': str(Path(tmp) / 'missing.db')}) as ui:
        ui.wait(b'0 matches')
        assert ui.finish(b'\x1b', expected=130) == b''
        assert not (Path(tmp) / 'missing.db').exists()

print('Native picker checks passed: exact recall, full-history search, filters, literal queries, resize, cancellation, terminal restoration, themes, zsh handoff, legacy/empty databases.')
