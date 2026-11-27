import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile

binary = str(Path('build/atchim').resolve())
with tempfile.TemporaryDirectory() as tmp:
    database = str(Path(tmp) / 'history.db')
    env = dict(os.environ, ATCHIM_DB=database, ATCHIM_BIN=binary,
               ATCHIM_RECORD='1', ATCHIM_IGNORE='', ATCHIM_IGNORE_SPACE='1')
    # Migrate an existing installation without losing history.
    with sqlite3.connect(database) as db:
        db.execute('CREATE TABLE HISTORY(ID INTEGER PRIMARY KEY, COMMAND TEXT, HOST TEXT, TIME TEXT)')
        db.execute("INSERT INTO HISTORY VALUES(1, 'old', 'local', '2026-01-01')")
    def run(*args, **changes):
        return subprocess.run([binary, *args], env=env | changes, check=True, capture_output=True, text=True)
    command = "printf 'héllo\\tworld'\necho done"
    run('log', '--cwd', '/tmp/example', '--exit-status', '7', '--', command)
    details = run('show', '2').stdout
    assert command in details and '/tmp/example' in details and 'exit 7' in details
    run('log', '--', ' private')
    run('log', '--', 'secret token', ATCHIM_IGNORE='secret*\npassword*')
    run('log', '--', 'disabled', ATCHIM_RECORD='0')
    with sqlite3.connect(database) as db:
        assert db.execute('SELECT COMMAND, CWD, EXIT_STATUS FROM HISTORY ORDER BY ID').fetchall() == [('old', None, None), (command, '/tmp/example', 7)]
    # Exercise actual zsh functions and preserve the command's starting directory.
    script = '''eval "$("$ATCHIM_BIN" init zsh)"
__atchim_preexec 'failed command'
cd /
false
__atchim_precmd
__atchim_precmd
__atchim_preexec ' hidden'
__atchim_precmd
'''
    subprocess.run(['zsh', '-f', '-c', script], env=env, check=True, capture_output=True)
    with sqlite3.connect(database) as db:
        rows = db.execute('SELECT COMMAND, CWD, EXIT_STATUS FROM HISTORY ORDER BY ID').fetchall()
        assert rows[-1] == ('failed command', str(Path.cwd()), 1), rows
        assert len(rows) == 3, rows
    print('Integration checks passed: migration, metadata, multiline/Unicode, exclusions, zsh hooks.')

with tempfile.TemporaryDirectory() as tmp:
    source = str(Path(tmp) / 'atuin.db')
    destination = str(Path(tmp) / 'atchim.db')
    env = dict(os.environ, ATCHIM_DB=destination, ATUIN_DB=source)
    with sqlite3.connect(source) as db:
        db.execute('CREATE TABLE history(command TEXT, hostname TEXT, timestamp INTEGER, cwd TEXT, exit INTEGER, deleted_at INTEGER)')
        db.executemany('INSERT INTO history VALUES(?,?,?,?,?,?)', [
            ('echo héllo\ntrue', 'laptop', 1700000000000000000, '/project', 0, None),
            ('false', '', 1700000001000000000, '/tmp', 1, None),
            ('removed', 'laptop', 1700000002000000000, '/', 0, 1),
        ])
    def migrate(*args):
        return subprocess.run([binary, *args], env=env, text=True, capture_output=True, check=True)
    assert 'Imported 2' in migrate('migrate', 'atuin').stdout
    assert 'Imported 0' in migrate('import', 'atuin', source).stdout
    with sqlite3.connect(destination) as db:
        assert db.execute('SELECT COMMAND, HOST, CWD, EXIT_STATUS FROM HISTORY ORDER BY ID').fetchall() == [
            ('echo héllo\ntrue', 'laptop', '/project', 0), ('false', 'local', '/tmp', 1)]
    with sqlite3.connect(source) as db:
        assert db.execute('SELECT COUNT(*) FROM history').fetchone()[0] == 3
    # Older source schemas lack optional context/deletion columns.
    legacy = str(Path(tmp) / 'legacy.db')
    with sqlite3.connect(legacy) as db:
        db.execute('CREATE TABLE history(command TEXT, hostname TEXT, timestamp INTEGER)')
        db.execute("INSERT INTO history VALUES('legacy', 'host', 1600000000000000000)")
    assert 'Imported 1' in migrate('migrate', 'atuin', legacy).stdout
    print('Atuin migration checks passed: context, repeat imports, deleted entries, legacy schema, source preserved.')

with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ, XDG_CONFIG_HOME=tmp)
    result = subprocess.run([binary, 'themes'], env=env, capture_output=True, text=True, check=True)
    assert 'midnight' in result.stdout and 'github-light' in result.stdout
    assert '\x1b' not in result.stdout
    subprocess.run([binary, 'themes', 'set', 'nord'], env=env, capture_output=True, check=True)
    assert (Path(tmp) / 'atchim/theme').read_text() == 'nord\n'
    invalid = subprocess.run([binary, 'themes', 'preview', '../missing'], env=env, capture_output=True)
    assert invalid.returncode != 0
    print('Theme checks passed: discovery, previews, saved selection, invalid names.')

with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ, ATCHIM_DB=tmp+'/db', ATCHIM_RECORD='1', ATCHIM_IGNORE='')
    for command in ('git status old', 'git status new', 'literal* star', 'literal? question', 'literal[ bracket', 'héllo world'):
        subprocess.run([binary, 'log', '--', command], env=env, check=True)
    for prefix, expected in [('git status', 'git status new'), ('literal*', 'literal* star'), ('literal?', 'literal? question'), ('literal[', 'literal[ bracket'), ('héllo', 'héllo world'), ('missing', '')]:
        result = subprocess.run([binary, 'suggest', prefix], env=env, text=True, capture_output=True, check=True)
        assert result.stdout.rstrip('\n') == expected, (prefix, result.stdout)
    print('Indexed suggestion checks passed: newest match, literal glob characters, Unicode, missing prefix.')

# Fake ssh consumes one line, allowing the real PTY wrapper to run without a network.
import pty
with tempfile.TemporaryDirectory() as tmp:
    fake_ssh = Path(tmp) / 'ssh'
    fake_ssh.write_text('#!/bin/sh\nread -r line\nexit 0\n')
    fake_ssh.chmod(0o755)
    for index, (command, controls, expected) in enumerate([
        ('echo remote', {}, True),
        (' hidden', {}, False),
        ('secret token', {'ATCHIM_IGNORE': 'secret*'}, False),
        ('disabled', {'ATCHIM_RECORD': '0'}, False),
    ]):
        database = str(Path(tmp) / f'ssh-{index}.db')
        env = dict(os.environ, PATH=tmp+':'+os.environ['PATH'], ATCHIM_DB=database,
                   ATCHIM_RECORD='1', ATCHIM_IGNORE='', ATCHIM_IGNORE_SPACE='1') | controls
        master, slave = pty.openpty()
        process = subprocess.Popen([str(Path('build/assh').resolve()), 'test-host'],
                                   stdin=slave, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        os.close(slave)
        try:
            os.write(master, (command+'\n').encode())
            _, error = process.communicate(timeout=5)
            assert process.returncode == 0, error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
        if expected:
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT COMMAND, HOST, CWD, EXIT_STATUS FROM HISTORY').fetchall() == [(command, 'test-host', None, None)]
        else:
            assert not Path(database).exists()
    print('SSH wrapper checks passed: direct SQLite writes and recording exclusions without a server.')

# Old daemon settings must never route a command away from SQLite, even when
# a socket is listening at the formerly configured endpoint.
import select
import socket
with tempfile.TemporaryDirectory() as tmp:
    database = str(Path(tmp) / 'history.db')
    endpoint = str(Path(tmp) / 'history.sock')
    env = dict(os.environ, ATCHIM_DB=database, ATCHIM_RECORD='1',
               ATCHIM_IGNORE='', ATCHIM_DAEMON='1', ATCHIM_SOCKET=endpoint)
    commands = [('start', 'echo first\necho second', 'local'),
                ('log', 'printf "with\ttab"', 'remote')]
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(endpoint)
        listener.listen()
        for action, command, host in commands:
            args = [binary, action]
            if host != 'local':
                args += ['--host', host]
            subprocess.run([*args, '--', command], env=env, check=True, capture_output=True)
        assert not select.select([listener], [], [], 0)[0], 'Logging contacted the old server'
    with sqlite3.connect(database) as db:
        assert db.execute('SELECT COMMAND, HOST FROM HISTORY ORDER BY ID').fetchall() == [
            (command, host) for _, command, host in commands]
    writers = [subprocess.Popen([binary, 'log', '--', f'parallel {i}'], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE) for i in range(12)]
    for writer in writers:
        _, stderr = writer.communicate(timeout=10)
        assert writer.returncode == 0, stderr
    with sqlite3.connect(database) as db:
        assert db.execute('SELECT COUNT(*) FROM HISTORY').fetchone()[0] == 14
    # Updating installation removes legacy settings from the managed block and
    # preserves unrelated shell configuration.
    rc = Path(tmp) / 'zshrc'
    rc.write_text('export MY_SETTING=keep\n# >>> atchim >>>\nexport ATCHIM_DAEMON=1\n'
                  'export ATCHIM_SERVER_BIN=old-server\n# <<< atchim <<<\n')
    for _ in range(2):
        subprocess.run(['./install', '--shell', 'zsh', '--rc-file', str(rc)],
                       env=env, check=True, capture_output=True)
    installed = rc.read_text()
    assert 'export MY_SETTING=keep' in installed
    assert installed.count('# >>> atchim >>>') == 1
    assert 'ATCHIM_BIN=' in installed
    assert 'ATCHIM_DAEMON' not in installed and 'ATCHIM_SERVER_BIN' not in installed
    print('Direct logging checks passed: obsolete settings ignored, no socket traffic, exact commands, concurrent writers, installer upgrade.')
