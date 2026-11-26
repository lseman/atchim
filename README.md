# Atchim

Atchim is a simple command-line tool for logging and visualizing your command history. It allows you to save the commands you execute in your terminal to a SQLite database and provides a convenient way to search and visualize your command history using [FZF](https://github.com/junegunn/fzf).

## Features

- Log all executed commands to an SQLite database.
- Search and filter your command history with FZF.
- Visualize and review your command history easily.
- SQLite-first logging with an optional daemon mode.

## Getting Started

### Prerequisites

Before using Atchim, you'll need to have the following prerequisites installed on your system:

- [SQLite](https://www.sqlite.org/) for the database.
- [FZF](https://github.com/junegunn/fzf) for interactive history search.
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
with a `preexec` hook and binds a searchable fzf history view to Ctrl-R.
Run `ATCHIM_DAEMON=1 ./install --shell zsh` if you want the optional daemon
mode enabled in that managed block.

### Manual zsh Setup

If you prefer to manage dotfiles yourself, add this to `.zshrc`:

   ```bash
   export ATCHIM_BIN=/path/to/atchim
   eval "$("$ATCHIM_BIN" init zsh)"
   ```

#### Usage

- By default, Atchim writes command history directly to SQLite.

- Set `ATCHIM_DAEMON=1` to opt into the background daemon. In daemon mode,
  Atchim starts `atchim_server`, sends log events over `ATCHIM_SOCKET`, and
  falls back to direct SQLite if the daemon is unavailable.

- History search, imports, and default logging read/write SQLite directly.

- Use ctrl + r to interactively search your command history using fzf.

- Press the up arrow on an empty prompt to search from your recent history.

- If `zsh-autosuggestions` is installed, Atchim adds recent history suggestions
  as you type. Press Tab to accept a visible suggestion.

- Set `ATCHIM_THEME` to change the Ctrl-R history colors. Built-in themes live
  in `themes/`: `github-dark`, `github-light`, `catppuccin-mocha`, `dracula`,
  and `nord`.

- Use the assh wrapper if you want to log ssh commands to the database.

### Themes

The zsh integration loads `github-dark` by default:

```bash
export ATCHIM_THEME=dracula
```

Theme lookup uses `themes/` next to `ATCHIM_BIN`. Set `ATCHIM_THEME_DIR` to use
another directory, or set `ATCHIM_THEME` to a theme file path:

```bash
export ATCHIM_THEME_DIR="$HOME/.config/atchim/themes"
export ATCHIM_THEME=my-theme
```

Theme files are shell snippets with color variables:

```bash
ATCHIM_COLOR_COMMAND="#58a6ff"
ATCHIM_COLOR_STRING="#3fb950"
ATCHIM_FZF_COLOR_BASE="fg:#c9d1d9,hl:#58a6ff,fg+:#f0f6fc,hl+:#79c0ff"
ATCHIM_FZF_COLOR_UI="border:#30363d,header:#8b949e,info:#8b949e,prompt:#58a6ff,pointer:#58a6ff,marker:#3fb950,spinner:#d29922"
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
atchim suggest "git c"             # print the newest matching history prefix
atchim delete <id>               # delete a specific history entry by ID
atchim clear                     # clear all history entries
```

Set `ATCHIM_DB` to choose a different SQLite database path, or
`ATCHIM_SOCKET` to choose a different Unix socket path in daemon mode.
