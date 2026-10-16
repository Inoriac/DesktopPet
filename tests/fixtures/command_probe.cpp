#include <cstdlib>
#include <fstream>
#include <string_view>
#include <iostream>

// A real child command for the command tool test, independent of Qt Test's runner.
int main(int argc, char* argv[]) {
    if (argc != 2) return 64;
    if (std::string_view(argv[1]) == "--crash") std::abort();
    if (std::string_view(argv[1]) == "--fail") return 17;
    std::ofstream marker("command-probe.txt");
    marker << argv[1];
    marker.close();
    if (!marker) return 74;
    std::cout << argv[1] << '\n';
    return 0;
}
