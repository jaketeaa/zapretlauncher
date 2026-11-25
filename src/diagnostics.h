#pragma once
#include <string>
#include <vector>

namespace diagnostics {
enum class Level { Good, Warning, Error };
struct Result {
    std::string name;
    Level level;
    std::string detail;
};
struct Report {
    std::vector<Result> network;
    std::vector<Result> system;
};
Report Run();
}
