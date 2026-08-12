#pragma once

#include <cstdint>
#include <string>

namespace dense {

enum class SceneMode {
    AiRpgWorld,
    VisualTest
};

struct LaunchOptions {
    SceneMode scene{SceneMode::AiRpgWorld};
    // This source-project oracle seed exercises rivers, coast, forest,
    // highland, rock, tundra and snow in the initial 513-tile window.
    std::int64_t seed{8675309};
    bool valid{true};
    bool showHelp{};
    std::wstring error;
};

LaunchOptions parseLaunchOptions(int argumentCount,wchar_t* const* arguments);
const wchar_t* launchUsage();

} // namespace dense
