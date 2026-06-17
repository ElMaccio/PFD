#include <iostream>
#include <cstdio>

int main() {
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1.0;
    int millideg;
    fscanf(f, "%d", &millideg);
    fclose(f);
    std::cout <<  millideg / 1000.0 << "\n";
    return 0;
}
