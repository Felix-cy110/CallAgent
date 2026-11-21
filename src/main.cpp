#include <iostream>

#include <hv/hversion.h>

int main() {
    std::cout << "CallAgent client skills runtime"
              << " (libhv " << hv_version() << ")\n";
    return 0;
}
