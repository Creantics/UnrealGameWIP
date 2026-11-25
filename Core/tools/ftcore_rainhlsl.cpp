// ftcore_rainhlsl: prints the rain cover's ring term (ftcore::rainCoverRingHlsl), the custom node's code the material
// builds put in, so it can be compiled on its own before a build. An offline check, not the game.
//
//   ftcore_rainhlsl sprite|surface
//
// Exit 0 with the code on stdout, 2 on a bad argument.

#include "ftcore/RainCover.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2 || (std::strcmp(argv[1], "sprite") != 0 && std::strcmp(argv[1], "surface") != 0)) {
        std::fprintf(stderr, "usage: ftcore_rainhlsl sprite|surface\n");
        return 2;
    }
    const std::string code = ftcore::rainCoverRingHlsl(ftcore::RainCoverSpec{}, std::strcmp(argv[1], "sprite") == 0);
    std::fputs(code.c_str(), stdout);
    return 0;
}
