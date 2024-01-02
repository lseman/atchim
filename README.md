# Atchim

Atchim is a simple command-line tool for logging and visualizing your command history. It allows you to save the commands you execute in your terminal to a SQLite database and provides a convenient way to search and visualize your command history using [FZF](https://github.com/junegunn/fzf).

## Features

- Log all executed commands to an SQLite database.
- Search and filter your command history with FZF.
- Visualize and review your command history easily.
- Client-Server architecture to efficiently handle command logging.

## Getting Started

### Prerequisites

Before using Atchim, you'll need to have the following prerequisites installed on your system:

- [SQLite](https://www.sqlite.org/) for the database.

### Installation

1. Clone the Atchim repository to your local machine:

   ```bash
   git clone https://github.com/lseman/atchim.git


## Getting Started

### Prerequisites

Before using Atchim, you'll need to have the following prerequisites installed on your system:

- [SQLite](https://www.sqlite.org/) for the database.

### Installation

1. Clone the Atchim repository to your local machine:

   ```bash
   git clone https://github.com/lseman/atchim.git
   ```

2. Build achim

   ```bash
   cd atchim
   g++ atchim_client.cpp -o atchim_client
   g++ atchim_server.cpp -o atchim_server -lsqlite3 -lpthread
   ```

3. Start the atchim server (it will run as a daemon):

   ```bash
   ./atchim_server
   ```

4. Add this to your shell configuration file (e.g., ~/.bashrc or ~/.zshrc)

   ```bash

   _atchim_preexec() {
      local command="$1"
      /path/to/atchim_client "start" "$command"
   }

   if [ "$SHELL" = "/bin/zsh" ]; then
      autoload -U add-zsh-hook
      add-zsh-hook preexec _atchim_preexec
   else
      trap '_atchim_preexec "$BASH_COMMAND"' DEBUG
   fi
   _his(){

      emulate -L zsh
      zle -I
      DB_PATH="$HOME/.atchim.db"

      # SQL query to fetch command history
      SQL_QUERY="SELECT ID, COMMAND FROM HISTORY ORDER BY ID ASC;"

      selected_command=$(sqlite3 -separator ' ' "$DB_PATH" "$SQL_QUERY" | fzf --tac --no-sort | awk '{$1=""; print $0}' | sed 's/^[ \t]*//')

      if [[ -n $selected_command ]]; then
         RBUFFER=""
         LBUFFER=$selected_command
         zle accept-line
      fi

   }

   zle -N _his_widget _his
   bindkey -M emacs '^r' _his_widget
   bindkey -M vicmd '^r' _his_widget
   bindkey -M viins '^r' _his_widget
   ```

#### Usage

- The atchim server runs in the background, listening for commands sent by the atchim client.

- Each time you execute a command in your shell, the atchim client sends this command to the server for logging.

- Use ctrl + r to interactively search your command history using fzf.