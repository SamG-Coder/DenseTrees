#include "launch_options.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition,const char*message) {
    if(!condition){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}
}

} // namespace

int main() {
    wchar_t executable[]=L"DenseTrees.exe";
    wchar_t testWorld[]=L"--test-world";
    wchar_t seedSwitch[]=L"--seed";
    wchar_t seedValue[]=L"8675309";
    wchar_t*defaultArguments[]{executable};
    const auto defaults=dense::parseLaunchOptions(1,defaultArguments);
    require(defaults.valid&&defaults.scene==dense::SceneMode::AiRpgWorld&&
            defaults.seed==8675309,"default launch is not the AI RPG world");
    wchar_t*testArguments[]{executable,testWorld,seedSwitch,seedValue};
    const auto test=dense::parseLaunchOptions(4,testArguments);
    require(test.valid&&test.scene==dense::SceneMode::VisualTest&&
            test.seed==8675309,"test-world launch arguments were not parsed");
    wchar_t missingSeed[]=L"--seed";
    wchar_t*invalidArguments[]{executable,missingSeed};
    require(!dense::parseLaunchOptions(2,invalidArguments).valid,
            "missing seed value was accepted");
    wchar_t emptySeedValue[]=L"";
    wchar_t*emptySeedArguments[]{executable,seedSwitch,emptySeedValue};
    require(!dense::parseLaunchOptions(3,emptySeedArguments).valid,
            "empty seed value was accepted");
    wchar_t unknown[]=L"--unknown";
    wchar_t*unknownArguments[]{executable,unknown};
    require(!dense::parseLaunchOptions(2,unknownArguments).valid,
            "unknown launch argument was accepted");
    std::cout<<"launch option checks passed\n";
}
