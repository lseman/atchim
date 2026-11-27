#include "atchim/picker.hpp"
#include "atchim/db.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace atchim {
namespace {
using namespace ftxui;

// FTXUI owns the terminal lifecycle. Keep its display bytes separate from the
// selected command on stdout, including when invoked via shell substitution.
class TerminalIO {
    int input_ = -1;
    int output_ = -1;
public:
    TerminalIO() {
        const int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (tty < 0) throw std::runtime_error("Interactive picker needs a controlling terminal");
        std::cout.flush();
        input_ = dup(STDIN_FILENO);
        output_ = dup(STDOUT_FILENO);
        if (input_ < 0 || output_ < 0 || dup2(tty, STDIN_FILENO) < 0 || dup2(tty, STDOUT_FILENO) < 0) {
            close(tty);
            restore();
            throw std::runtime_error("Cannot connect picker to terminal");
        }
        close(tty);
    }
    ~TerminalIO() { std::cout.flush(); restore(); }
    TerminalIO(const TerminalIO &) = delete;
    TerminalIO &operator=(const TerminalIO &) = delete;
private:
    void restore() noexcept {
        if (input_ >= 0) { dup2(input_, STDIN_FILENO); close(input_); input_ = -1; }
        if (output_ >= 0) { dup2(output_, STDOUT_FILENO); close(output_); output_ = -1; }
    }
};

void runTransientPicker(ScreenInteractive &screen, Component component) {
    screen.Loop(std::move(component));
    // TerminalOutput leaves the last frame visible and the cursor one line
    // below it. Erase that frame before returning the selection to the shell.
    // ResetPosition clears only the rendered rows, preserving output above it.
    if (screen.dimy() > 0)
        std::cout << "\r\033[2K\033[1A" << screen.ResetPosition(true) << std::flush;
}

// Stored commands are untrusted terminal content. Preserve original bytes for
// selection, but render control bytes visibly instead of interpreting them.
std::string display(std::string_view value, bool multiline = false) {
    std::string out;
    for (unsigned char c : value) {
        if (c == '\n') out += multiline ? "\n" : " ↵ ";
        else if (c == '\t') out += " ⇥ ";
        else if (c < 32 || c == 127) out += '?';
        else out += static_cast<char>(c);
    }
    return out;
}

struct Palette {
    std::map<std::string, Color> colors{{"FG", Color::RGB(203, 213, 225)},
        {"MUTED", Color::RGB(148, 163, 184)}, {"COMMAND", Color::RGB(125, 211, 252)},
        {"STRING", Color::RGB(167, 243, 208)}, {"OPTION", Color::RGB(196, 181, 253)},
        {"OPERATOR", Color::RGB(240, 171, 252)}, {"PATH", Color::RGB(252, 211, 77)}};
    bool enabled = std::getenv("NO_COLOR") == nullptr;
    Color selection = Color::RGB(30, 41, 59);
    explicit Palette(const std::filesystem::path &file) {
        std::ifstream input(file);
        std::string line;
        while (std::getline(input, line)) {
            const auto equals = line.find('=');
            if (!line.starts_with("ATCHIM_COLOR_") || equals == std::string::npos) continue;
            const auto role = line.substr(13, equals - 13);
            if (!colors.contains(role)) continue;
            auto hex = line.substr(equals + 1);
            if (hex.size() == 9 && hex.front() == '"' && hex.back() == '"') hex = hex.substr(1, 7);
            if (hex.size() != 7 || hex[0] != '#' || !std::all_of(hex.begin() + 1, hex.end(),
                [](unsigned char c) { return std::isxdigit(c); })) continue;
            if (role == "FG" && std::stoi(hex.substr(1, 2), nullptr, 16) < 128)
                selection = Color::RGB(226, 232, 240);
            colors[role] = Color::RGB(std::stoi(hex.substr(1, 2), nullptr, 16),
                std::stoi(hex.substr(3, 2), nullptr, 16), std::stoi(hex.substr(5, 2), nullptr, 16));
        }
    }
    Element paint(Element element, const std::string &role) const {
        return enabled ? element | color(colors.at(role)) : element;
    }
};

// Lightweight shell token coloring; command bytes are never evaluated.
Element commandLine(std::string_view command, const Palette &theme) {
    Elements tokens;
    size_t pos = 0;
    bool executable = true;
    while (pos < command.size()) {
        const size_t start = pos;
        std::string role = "FG";
        const char c = command[pos];
        if (c == '\'' || c == '"') {
            role = "STRING";
            const char quote = command[pos++];
            while (pos < command.size()) {
                if (command[pos] == '\\' && quote == '"' && pos + 1 < command.size()) pos += 2;
                else if (command[pos++] == quote) break;
            }
            executable = false;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            while (pos < command.size() && std::isspace(static_cast<unsigned char>(command[pos]))) ++pos;
            if (command.substr(start, pos - start).find('\n') != std::string_view::npos) executable = true;
        } else if (std::string_view("|&;<>()").find(c) != std::string_view::npos) {
            role = "OPERATOR"; ++pos; executable = true;
        } else {
            while (pos < command.size() && !std::isspace(static_cast<unsigned char>(command[pos])) &&
                std::string_view("|&;<>()'\"").find(command[pos]) == std::string_view::npos) ++pos;
            role = executable ? "COMMAND" : c == '-' ? "OPTION" :
                command.substr(start, pos - start).find('/') != std::string_view::npos ? "PATH" : "FG";
            executable = false;
        }
        tokens.push_back(theme.paint(text(display(command.substr(start, pos - start))), role));
    }
    if (tokens.empty()) tokens.push_back(text(""));
    return hbox(std::move(tokens));
}
struct Height {
    int value = 65;
    bool percentage = true;
    Height() {
        const char *setting = std::getenv("ATCHIM_HEIGHT");
        if (!setting) setting = std::getenv("ATCHIM_FZF_HEIGHT"); // Existing configurations.
        if (!setting) return;
        std::string_view input(setting);
        percentage = input.ends_with('%');
        if (percentage) input.remove_suffix(1);
        const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
        if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || value < 1 ||
            value > (percentage ? 100 : 10000))
            throw std::invalid_argument("ATCHIM_HEIGHT must be 1..100% or a positive number of rows");
    }
    int rows() const {
        const int available = std::max(1, Terminal::Size().dimy - 1);
        return std::clamp(percentage ? available * value / 100 : value, std::min(4, available), available);
    }
};

struct Entry {
    std::string command, host, time, cwd, result, epoch;
};
struct Request {
    std::string query, cwd, host;
    unsigned long generation = 0;
};
struct Response {
    std::vector<Entry> rows;
    std::string error;
    unsigned long generation = 0;
};

// Separate SQLite connection per worker; canceled queries never replace newer results.
class Search {
    std::filesystem::path database_;
    std::function<void()> notify_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Request request_;
    std::optional<Response> response_;
    std::atomic<unsigned long> generation_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
    struct Progress { Search *owner; unsigned long generation; };
    static int cancel(void *data) {
        const auto &p = *static_cast<Progress *>(data);
        return p.owner->stop_ || p.owner->generation_ != p.generation;
    }
    void run() {
        unsigned long previous = 0;
        while (true) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop_ || request_.generation != previous; });
                if (stop_) return;
                request = request_; previous = request.generation;
            }
            Response response{{}, {}, request.generation};
            try {
                if (std::filesystem::exists(database_)) {
                    SQLiteDatabase db(database_, SQLITE_OPEN_READONLY);
                    // Do not stall shutdown behind a writer's lock.
                    sqlite3_busy_timeout(db.get(), 100);
                    Progress progress{this, request.generation};
                    sqlite3_progress_handler(db.get(), 1000, cancel, &progress);
                    if (tableHasColumn(db, "HISTORY", "COMMAND")) {
                        const bool cwd = tableHasColumn(db, "HISTORY", "CWD");
                        const bool exit = tableHasColumn(db, "HISTORY", "EXIT_STATUS");
                        const std::string directory = cwd ? "CWD" : "NULL";
                        const std::string result = exit ? "EXIT_STATUS" : "NULL";
                        Statement rows(db.get(), "SELECT COMMAND, HOST, TIME, " + directory + ", " + result +
                            ", strftime('%s', TIME) FROM HISTORY WHERE (?1='' OR instr(lower(COMMAND),lower(?1))>0 OR instr(lower(HOST),lower(?1))>0)"
                            " AND (?2='' OR " + directory + "=?2) AND (?3='' OR HOST=?3) ORDER BY ID DESC LIMIT 200;");
                        rows.bindText(1, request.query); rows.bindText(2, request.cwd); rows.bindText(3, request.host);
                        int rc;
                        while ((rc = rows.step()) == SQLITE_ROW) {
                            response.rows.push_back({rows.columnText(0), rows.columnText(1), rows.columnText(2), rows.columnText(3), rows.columnText(4), rows.columnText(5)});
                        }
                        if (rc != SQLITE_DONE && rc != SQLITE_INTERRUPT)
                            throw std::runtime_error(sqlite3_errmsg(db.get()));
                    }
                }
            } catch (const std::exception &e) { response.error = e.what(); }
            {
                std::lock_guard lock(mutex_);
                if (generation_ == request.generation && !stop_) response_ = std::move(response);
            }
            notify_();
        }
    }
public:
    Search(const std::filesystem::path &database, std::function<void()> notify)
        : database_(database), notify_(std::move(notify)), worker_([this] { run(); }) {}
    ~Search() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        changed_.notify_all();
        worker_.join();
    }
    unsigned long submit(std::string query, std::string cwd, std::string host) {
        std::lock_guard lock(mutex_);
        const auto next = ++generation_;
        request_ = {std::move(query), std::move(cwd), std::move(host), next};
        response_.reset();
        changed_.notify_one();
        return next;
    }
    std::optional<Response> poll() {
        std::lock_guard lock(mutex_);
        auto out = std::move(response_); response_.reset(); return out;
    }
};

std::string relativeAge(const std::string &value) {
    long long epoch = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), epoch);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return "unknown";
    const long long now = static_cast<long long>(std::time(nullptr));
    // Reject outlandish imported timestamps without risking signed overflow.
    if (epoch < -62135596800LL || epoch > 253402300799LL) return "unknown";
    const bool future = epoch > now;
    const auto seconds = future ? epoch - now : now - epoch;
    if (seconds < 60) return (future ? "+" : "") + std::to_string(seconds) + "s";
    const auto units = seconds < 3600 ? std::pair{60LL, "m"} :
        seconds < 86400 ? std::pair{3600LL, "h"} :
        seconds < 2592000 ? std::pair{86400LL, "d"} :
        seconds < 31536000 ? std::pair{2592000LL, "mo"} : std::pair{31536000LL, "y"};
    const auto duration = std::to_string(seconds / units.first) + units.second;
    return (future ? "+" : "") + duration;
}

Element details(const Entry &entry, const Palette &theme) {
    Elements lines{
        theme.paint(text("COMMAND DETAILS"), "COMMAND") | bold,
        theme.paint(text("Directory  " + display(entry.cwd.empty() ? "unknown" : entry.cwd)), "MUTED"),
        theme.paint(text("Host       " + display(entry.host)), "MUTED"),
        theme.paint(text("Executed   " + display(entry.time) + " UTC"), "MUTED"),
        theme.paint(text("Result     " + (entry.result.empty() ? std::string("unknown") :
            entry.result == "0" ? std::string("success") : "exit " + display(entry.result))), "MUTED"),
        separator(),
    };
    // Paragraphs wrap each actual command line without exposing terminal escapes.
    size_t pos = 0;
    do {
        const auto end = entry.command.find('\n', pos);
        lines.push_back(theme.paint(paragraph(display(entry.command.substr(pos, end - pos))), "FG"));
        if (end == std::string::npos) break;
        pos = end + 1;
    } while (pos < entry.command.size());
    return vbox(std::move(lines)) | yframe;
}
}

std::optional<PickedCommand> pickHistory(const std::filesystem::path &database,
    const std::filesystem::path &themeFile, const std::string &initial,
    bool directoryOnly, bool showDetails) {
    const Height height;
    TerminalIO io;
    auto screen = ScreenInteractive::TerminalOutput();
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);
    const Palette theme(themeFile);
    Search search(database, [&] { screen.PostEvent(Event::Custom); });
    std::string query = initial, host;
    const std::string directory = std::filesystem::current_path().string();
    Response response;
    size_t selected = 0;
    bool loading = true;
    std::optional<PickedCommand> selection;
    unsigned long generation = 0;
    auto submit = [&] {
        generation = search.submit(query, directoryOnly ? directory : "", host);
        response.rows.clear(); selected = 0; loading = true;
    };
    InputOption inputOption;
    inputOption.multiline = false;
    inputOption.cursor_position = static_cast<int>(query.size());
    inputOption.transform = [&](InputState state) {
        auto element = theme.paint(std::move(state.element), state.is_placeholder ? "MUTED" : "FG");
        return state.is_placeholder ? element | dim : element;
    };
    inputOption.on_change = submit;
    auto input = Input(&query, "Search command or host…", inputOption);
    submit();
    auto view = Renderer(input, [&] {
        if (auto next = search.poll(); next && next->generation == generation) {
            response = std::move(*next); loading = false; selected = 0;
        }
        const auto dimensions = Terminal::Size();
        Elements rows;
        for (size_t i = 0; i < response.rows.size(); ++i) {
            const auto &entry = response.rows[i];
            auto age = theme.paint(text(relativeAge(entry.epoch)) | align_right |
                size(WIDTH, EQUAL, 6), "MUTED") | dim;
            auto pointer = theme.paint(text(i == selected ? "  › " : "    "), "STRING");
            auto row = hbox({text(" "), age, pointer, commandLine(entry.command, theme) | flex, text(" ")});
            if (i == selected) {
                row = row | focus;
                row = theme.enabled ? row | bgcolor(theme.selection) : row | bold;
            }
            rows.push_back(std::move(row));
        }
        if (rows.empty()) rows.push_back(theme.paint(text(loading ? "  Searching…" : "  No matching commands"), "MUTED"));
        auto list = vbox(std::move(rows)) | yframe | flex;
        Element body = list;
        if (showDetails && !response.rows.empty()) {
            if (dimensions.dimx >= 100) body = hbox({list, theme.paint(separator(), "MUTED") | dim, details(response.rows[selected], theme) |
                size(WIDTH, EQUAL, std::max(30, dimensions.dimx * 2 / 5))});
            else body = vbox({list, text(""), details(response.rows[selected], theme) |
                size(HEIGHT, EQUAL, std::max(3, height.rows() / 3))});
        }
        std::string footer = loading ? "Searching all history…" : response.error.empty()
            ? std::to_string(response.rows.size()) + (response.rows.size() == 200 ? " newest matches · refine to find older entries" : " matches")
            : "Search error: " + response.error;
        const std::string scope = display(directoryOnly ? directory : "all directories") +
            (host.empty() ? " · all hosts" : " · " + display(host));
        const std::string help = dimensions.dimx < 80
            ? " ↑↓ select · enter run · tab edit · esc close"
            : " ↑↓ select · enter run · tab edit · ^D directory · ^T host · ^/ details · esc close";
        return vbox({
            hbox({theme.paint(text(" atchim"), "COMMAND") | bold,
                theme.paint(text("  history"), "MUTED"), filler(),
                theme.paint(text(scope + " "), "MUTED") | dim}),
            text(""),
            hbox({theme.paint(text("  ❯ "), "STRING"), input->Render() | flex}),
            text(""), body | flex, text(""),
            theme.paint(text(" " + footer), "MUTED"),
            theme.paint(text(help), "MUTED") | dim,
        }) | size(HEIGHT, EQUAL, height.rows());
    });
    auto component = CatchEvent(view, [&](Event event) {
        if (event == Event::Escape || event == Event::CtrlC || event == Event::CtrlG) {
            screen.Exit(); return true;
        }
        if (event == Event::Return || event == Event::Tab || event == Event::CtrlY) {
            if (!loading && !response.rows.empty()) {
                selection = PickedCommand{response.rows[selected].command, event != Event::Tab};
                screen.Exit();
            }
            return true;
        }
        if (event == Event::CtrlD) { directoryOnly = !directoryOnly; submit(); return true; }
        if (event == Event::CtrlT) {
            if (!host.empty()) host.clear();
            else if (!loading && !response.rows.empty()) host = response.rows[selected].host;
            submit(); return true;
        }
        if (event == Event::Special({31})) { showDetails = !showDetails; return true; }
        if (event == Event::ArrowUp || event == Event::PageUp || event == Event::CtrlP) {
            const size_t step = event == Event::PageUp ? 10 : 1;
            selected = selected > step ? selected - step : 0; return true;
        }
        if (event == Event::ArrowDown || event == Event::PageDown || event == Event::CtrlN) {
            const size_t step = event == Event::PageDown ? 10 : 1;
            if (!response.rows.empty()) selected = std::min(selected + step, response.rows.size() - 1);
            return true;
        }
        if (event == Event::CtrlU) { query.clear(); submit(); return true; }
        // Ctrl-R now refreshes the database, including commands from other shells.
        if (event == Event::CtrlR) { submit(); return true; }
        return false;
    });
    runTransientPicker(screen, component);
    return selection;
}

std::optional<std::string> pickTheme(const std::vector<std::filesystem::path> &themes) {
    if (themes.empty()) throw std::runtime_error("No themes available");
    const Height height;
    TerminalIO io;
    auto screen = ScreenInteractive::TerminalOutput();
    screen.TrackMouse(false);
    screen.ForceHandleCtrlC(false);
    size_t selected = 0;
    std::optional<std::string> selection;
    auto renderer = Renderer([&] {
        const Palette theme(themes[selected]);
        Elements rows;
        for (size_t i = 0; i < themes.size(); ++i) {
            auto row = theme.paint(text("  " + themes[i].stem().string()), "COMMAND");
            if (i == selected) {
                row = row | focus;
                row = theme.enabled ? row | bgcolor(theme.selection) : row | bold;
            }
            rows.push_back(std::move(row));
        }
        Elements swatches;
        for (const auto *role : {"COMMAND", "STRING", "OPTION", "OPERATOR", "PATH", "MUTED"})
            swatches.push_back(theme.paint(text("██  "), role));
        return vbox({theme.paint(text(" atchim  /  themes"), "COMMAND") | bold,
            theme.paint(text(" ↑/↓ preview · Enter save · Esc cancel"), "MUTED") | dim, text(""),
            vbox(std::move(rows)) | yframe | flex, text(""),
            hbox(std::move(swatches)), commandLine("git commit -m \"ship it\" && ls ./src", theme),
            theme.paint(text(" Palette applies when history next opens."), "MUTED") | dim}) |
            size(HEIGHT, EQUAL, height.rows());
    });
    auto component = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Escape || event == Event::CtrlC || event == Event::CtrlG) {
            screen.Exit(); return true;
        }
        if (event == Event::Return) { selection = themes[selected].stem().string(); screen.Exit(); return true; }
        if ((event == Event::ArrowUp || event == Event::Character('k')) && selected > 0) --selected;
        if ((event == Event::ArrowDown || event == Event::Character('j')) && selected + 1 < themes.size()) ++selected;
        return false;
    });
    runTransientPicker(screen, component);
    return selection;
}
}
