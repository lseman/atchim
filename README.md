# Atchim

Atchim is a simple command-line tool for logging and visualizing your command history. It allows you to save the commands you execute in your terminal to a SQLite database and provides a convenient way to search and visualize your command history using [FZF](https://github.com/junegunn/fzf).

## Features

- Log all executed commands to an SQLite database.
- Search and filter your command history with FZF.
- Visualize and review your command history easily.

## Getting Started

### Prerequisites

Before using Atchim, you'll need to have the following prerequisites installed on your system:

- [SQLite](https://www.sqlite.org/) for the database.

### Installation

1. Clone the Atchim repository to your local machine:

   ```bash
   git clone https://github.com/yourusername/atchim.git
   ```

2. Build achim

   ```bash
   cd atchim
   g++ atchim.cpp -o atchim -lsqlite3
   ```

3. Add the _atchim_preexec and _atchim_precmd functions to your shell configuration file (e.g., ~/.zshrc for Zsh):

   ```bash
   autoload -U add-zsh-hook

   _atchim_preexec() {
      local command="$1"
      /path/to/atchim "start" "$command"
   }


   add-zsh-hook preexec _atchim_preexec
   ```

4. Reload your shell configuration or open a new terminal window for the changes to take effect.

5. Use the provided "his" script to navigate through your history with fzf.