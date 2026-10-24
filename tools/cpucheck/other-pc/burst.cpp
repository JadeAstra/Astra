// Load in bursts, as a parallel section makes it: 32 threads woken together every 50 ms, each
// computing for about 20 ms, then asleep. Runs for the given number of seconds.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
    const int threads = argc > 2 ? std::atoi(argv[2]) : 32;
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + std::chrono::seconds(seconds);
    std::atomic<double> sink{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) {
        pool.emplace_back([&, t] {
            auto tick = start;
            while (tick < end) {
                tick += std::chrono::milliseconds(50);
                std::this_thread::sleep_until(tick);
                const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                double x = t + 1.0;
                while (std::chrono::steady_clock::now() < stop)
                    for (int k = 0; k < 2000; k++) x = std::sqrt(x * 1.0000001 + k);
                sink = sink + x;
            }
        });
    }
    for (auto& t : pool) t.join();
    std::printf("burst done %g\n", sink.load());
}
