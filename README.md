# Atchim

Atchim saves your shell history to SQLite and helps you find and reuse commands
with a native terminal picker built with [FTXUI](https://github.com/ArthurSonzogni/FTXUI).
It runs without a background service or fzf.

## Features

- Search your entire history, filtered by directory or host.
- Run a selection with Enter or put it on the prompt for editing with Tab.
- Recall multiline commands, tabs, Unicode, and trailing newlines exactly.
- Record the starting directory and exit status for local commands.
- Import Atuin history and choose from six built-in themes.
- Write directly to SQLite with no background service.

## Getting Started

### Prerequisites

Before using Atchim, you'll need to have the following prerequisites installed on your system:

- A C++23 compiler, CMake 3.20 or newer, SQLite development headers, and libuv development headers.
- zsh for shell integration; Python 3 to run the tests.
- Network access for the first build to fetch CPM.cmake and FTXUI.
- [zsh-autosuggestions](https://github.com/zsh-users/zsh-autosuggestions) if
  you want inline history suggestions while typing.

### Installation

1. Clone the Atchim repository to your local machine:

   ```bash
   git clone https://github.com/lseman/atchim.git
   ```

2. Build Atchim

   ```bash
   cd atchim
   make
   ```

3. Install the zsh integration:

   ```bash
   ./install --shell zsh
   ```

   Restart your shell, or run `source ~/.zshrc`.

The installer adds a managed Atchim block to `.zshrc`. That block logs commands
with paired `preexec` and `precmd` hooks and binds the native history picker to Ctrl-R.

### Manual zsh Setup

If you prefer to manage dotfiles yourself, add this to `.zshrc`:

   ```bash
   export ATCHIM_BIN=/path/to/atchim/build/atchim
   eval "$("$ATCHIM_BIN" init zsh)"
   ```

### Everyday use

- Use Ctrl-R to interactively search your command history.

- Press the up arrow on an empty prompt to search from your recent history.

- If `zsh-autosuggestions` is installed, Atchim adds recent history suggestions
  as you type. Press Tab to accept a visible suggestion.

- Set `ATCHIM_THEME` to change the Ctrl-R history colors. Built-in themes live
  in `themes/`: `midnight`, `github-dark`, `github-light`, `catppuccin-mocha`,
  `dracula`, and `nord`.

- Use the `assh` wrapper to record commands typed in SSH sessions.

### Themes

The zsh integration loads `midnight` by default:

```bash
export ATCHIM_THEME=dracula
```

Theme lookup supports the repository’s `themes/` directory and installed share directory. Set `ATCHIM_THEME_DIR` to use
another directory, or set `ATCHIM_THEME` to a theme file path:

```bash
export ATCHIM_THEME_DIR="$HOME/.config/atchim/themes"
export ATCHIM_THEME=my-theme
```

Theme files contain color assignments. The native picker reads these values as data:

```bash
ATCHIM_COLOR_COMMAND="#58a6ff"
ATCHIM_COLOR_STRING="#3fb950"
ATCHIM_COLOR_MUTED="#8b949e"
```

### CLI

```bash
atchim init zsh                  # print shell integration
atchim import atuin              # import ~/.local/share/atuin/history.db
atchim import atuin /path/db     # import a specific Atuin history DB
atchim start "make test"          # log a local command
atchim log --host prod "uptime"   # log a command for a host
atchim history -n 100             # print recent history
atchim search docker              # search command text and host
atchim pick --cwd docker          # search history in the current directory
atchim pick --details             # open the native picker with details
atchim suggest "git c"             # print the newest matching history prefix
atchim delete <id>               # delete a specific history entry by ID
atchim clear                     # clear all history entries
```

`start` and `log` record command text; they do not execute it.
The default database is `~/.atchim.db`. Set `ATCHIM_DB` to use a different path.

### Recording controls and command context

Local zsh commands are saved when the next prompt appears, with their starting
working directory (`CWD`) and exit status (`EXIT_STATUS`). Existing databases
are migrated automatically on the next write; older entries retain NULL context.
Commands that exit or replace the shell before the next prompt are not recorded.
The `history` output keeps its existing four-column format for scripts.

```zsh
export ATCHIM_RECORD=0             # pause recording; set to 1 to resume
export ATCHIM_IGNORE_SPACE=1       # default: skip commands starting with a space
export ATCHIM_IGNORE=$'secret*\n*password*'  # newline-separated glob patterns
```

These environment controls apply to CLI `start`/`log` calls too. Shell patterns
use zsh matching; CLI patterns use POSIX glob matching (use simple `*`/`?`
patterns for consistent behavior). Imports are not filtered by these controls. The SSH wrapper honors
`ATCHIM_RECORD`, `ATCHIM_IGNORE_SPACE`, and `ATCHIM_IGNORE`.

```bash
atchim log --cwd "$PWD" --exit-status 0 -- 'make test'
make test
```

All logging writes directly to SQLite. The SSH wrapper records command text and
host; remote directory and exit status are unknown because it observes terminal input.
Run `make test` for build and integration checks (requires Python 3 and zsh).

### History picker

Ctrl-R opens a borderless picker in the lower part of the terminal. Each row shows
compact elapsed time (`12s`, `2m`, `3h`, `5d`) on the left and a syntax-colored
command. The selected row has a subtle background. The picker uses 65% of the
terminal by default, leaving earlier shell output visible. When you select a
command or cancel, it clears its display before returning to the shell.

Typing searches command text and host across the entire database, showing the
newest 200 matches. Narrowing the query can find commands of any age; there is no
5,000-entry search window. Matching uses literal substrings, with SQLite's
case-insensitive matching for ASCII (non-ASCII characters match exactly).
Queries run in the background and are canceled when replaced.

- **Enter** runs the selected command through the zsh widget.
- **Tab** puts it on the terminal prompt for editing and manual execution.
- **Ctrl-Y** also runs the selection.
- **Ctrl-D** toggles the current-directory filter (exact directory).
- **Ctrl-T** filters by the selected command's host; press again for all hosts.
- **Ctrl-/** toggles command details, beside the list in wide terminals or below it in narrow ones.
- **Ctrl-R** refreshes history, including commands from other shells.
- **Ctrl-U** clears the query; arrows and Page Up/Down navigate results.
- **Esc / Ctrl-C** close without changing the prompt.

```zsh
export ATCHIM_HEIGHT=65%     # percentage of the terminal, or a number of rows
export ATCHIM_PREVIEW=1      # optionally start with details visible
```

`ATCHIM_FZF_HEIGHT` remains a fallback for existing configurations. `ATCHIM_HEIGHT`
takes precedence; `ATCHIM_HISTORY_LIMIT` no longer restricts interactive search.
History and theme pickers do not need fzf.

Multiline commands, tabs, Unicode, and trailing newlines are recalled exactly.
The list shows visible newline/tab markers; details show the command with wrapping.
Stored terminal controls are escaped for display. Older entries show unknown context.

Use `atchim pick [--cwd] [--details] [--] [query]` to open the picker directly.
It writes only the selected command to stdout, without adding a newline, and
returns 130 on cancellation. Standalone mode returns the selection without executing it.
Use `atchim show <id>` to inspect an entry outside the picker. Reload the shell
integration after updating: `eval "$("$ATCHIM_BIN" init zsh)"`.

### Migrate from Atuin

```bash
atchim migrate atuin                 # auto-detect the default Atuin database
atchim migrate atuin /path/history.db
```

`import atuin` remains available as an equivalent command. Set `ATUIN_DB` to
choose the source or `ATCHIM_DB` to choose the destination. Default source lookup
uses `$XDG_DATA_HOME/atuin/history.db`, or `~/.local/share/atuin/history.db`.

Migration reads the source without modifying it and imports command text, host,
timestamp, working directory, and exit status. Optional fields missing from older
Atuin schemas remain unknown. Deleted entries are skipped. Repeating an import
skips entries with the same command, host, and timestamp (at second precision).
The import runs in a transaction so a failed import rolls back its history writes.

### Browse and choose colors

```bash
./build/atchim themes                       # interactive picker with live preview
./build/atchim themes list                  # all palettes and highlighted samples
./build/atchim themes preview midnight      # inspect one palette
./build/atchim themes set catppuccin-mocha   # save your choice
```

The picker reads the saved choice whenever Ctrl-R opens; no shell restart is
needed. Themes inherit the terminal background, with separate selection highlights and
command, string, option, operator, path, and timestamp colors. Available palettes:
`midnight`, `github-dark`, `github-light`, `catppuccin-mocha`, `dracula`, and `nord`.

The preference is stored at `$XDG_CONFIG_HOME/atchim/theme` (default:
`~/.config/atchim/theme`). An explicit `ATCHIM_THEME` overrides the saved choice;
run `unset ATCHIM_THEME` to use the theme selected by the command. Preview output
uses true color in a terminal and plain text when piped or when `NO_COLOR` is set.

In a terminal, `atchim themes` opens a selector: arrow keys update the highlighted
sample immediately, Enter saves, and Escape cancels without changing preferences.
Use `atchim themes pick` to explicitly open it. The native selector previews
the highlighted palette live; the saved palette applies when history next opens.
When piped, `atchim themes` prints the theme list instead.

### Build and test

Build outputs are `build/atchim` and `build/assh`.
The local installer configures zsh to use these binaries.

Builds use C++23 and `-O2` by default. Override `CXX` and `CXXFLAGS` as needed.

The build uses CPM.cmake 0.43.2 (a FetchContent wrapper) to download and build
FTXUI 7.0.3. Both downloads are pinned with SHA-256 checksums. The first build
requires network access; subsequent builds reuse downloaded sources. Set
`CPM_SOURCE_CACHE` to share dependencies between build directories, or use
`CPM_ftxui_SOURCE` to supply a local FTXUI source checkout. CMake also works
directly, keeping the binaries in the same location:

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$PWD/build"
cmake --build build/cmake
```

```bash
make test
```

This runs logging, migration, shell integration, concurrent write, installer, and
native picker checks. Picker checks use a pseudo-terminal.

### Upgrade from the server-based version

The optional history server has been removed. Older `ATCHIM_DAEMON`,
`ATCHIM_SERVER_BIN`, and `ATCHIM_SOCKET` settings are ignored. Run
`./install --shell zsh` again to replace the old managed shell block, then reload
zsh. Existing SQLite history requires no changes.

```zsh
./install --shell zsh
source ~/.zshrc
```
