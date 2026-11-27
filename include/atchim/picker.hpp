#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atchim {
struct PickedCommand {
    std::string command;
    bool execute = false;
};
std::optional<PickedCommand> pickHistory(const std::filesystem::path &database,
    const std::filesystem::path &theme, const std::string &query,
    bool directoryOnly, bool details);
std::optional<std::string> pickTheme(const std::vector<std::filesystem::path> &themes);
}
