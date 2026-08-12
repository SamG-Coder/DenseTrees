#include "launch_options.hpp"

#include <cerrno>
#include <cwchar>
#include <limits>

namespace dense {
namespace {

bool equalInsensitive(const wchar_t*left,const wchar_t*right) {
    return left&&right&&_wcsicmp(left,right)==0;
}

} // namespace

LaunchOptions parseLaunchOptions(int argumentCount,wchar_t* const*arguments) {
    LaunchOptions result;
    for(int index=1;index<argumentCount;++index) {
        const wchar_t*argument=arguments[index];
        if(equalInsensitive(argument,L"--test-world")||
           equalInsensitive(argument,L"--test-scene")||
           equalInsensitive(argument,L"--legacy-test")) {
            result.scene=SceneMode::VisualTest;
        } else if(equalInsensitive(argument,L"--seed")) {
            if(index+1>=argumentCount) {
                result.valid=false;result.error=L"--seed requires a signed integer.";break;
            }
            wchar_t*end{};
            errno=0;
            const long long value=std::wcstoll(arguments[++index],&end,10);
            if(errno==ERANGE||!end||end==arguments[index]||*end!=L'\0') {
                result.valid=false;result.error=L"--seed must be a signed 64-bit integer.";break;
            }
            result.seed=static_cast<std::int64_t>(value);
        } else if(equalInsensitive(argument,L"--help")||
                  equalInsensitive(argument,L"-h")||
                  equalInsensitive(argument,L"/?")) {
            result.showHelp=true;
        } else {
            result.valid=false;
            result.error=L"Unknown launch argument: ";result.error+=argument;break;
        }
    }
    return result;
}

const wchar_t* launchUsage() {
    return L"DenseTrees.exe [--seed <integer>] [--test-world]\n\n"
           L"No arguments opens the AI RPG AOE-derived 3D world.\n"
           L"--test-world opens the retained Dense Trees visual test scene.";
}

} // namespace dense
