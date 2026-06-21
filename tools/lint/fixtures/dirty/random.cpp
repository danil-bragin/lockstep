// DIRTY: randomness. Categories: std::rand, std::random_device, engines, <random>.
#include <random>
#include <cstdlib>

namespace lockstep::bad {
int roll() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::mt19937_64 gen64(0);
    std::default_random_engine eng;
    (void)gen; (void)gen64; (void)eng;
    srand(1);
    return std::rand() + rand();
}
}  // namespace lockstep::bad
