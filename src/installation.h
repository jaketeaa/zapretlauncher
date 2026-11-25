#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace installation {
using Path = std::filesystem::path;

Path ResolveSelection(const Path& selected);
bool Prepare(const Path& directory, const Path& configPath, std::string& error);
bool IsManaged(const Path& directory, const Path& configPath);

// Returns an empty string on success. Leaves the ownership marker in place
// until the last file has been removed, so a partial removal can be retried.
std::string Remove(const Path& directory, const Path& configPath, const std::function<void()>& closeTerminal);
}
