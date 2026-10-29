#include "servicewatch.hpp"
#include <charconv>
#include <csignal>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
static unsigned number(const std::string &text, unsigned low, unsigned high) {
    unsigned n = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || n < low || n > high)
        throw std::invalid_argument("numeric option outside supported range");
    return n;
}
int main(int argc, char **argv) {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        std::string config;
        unsigned rounds = 1, workers = 4, timeout = 1000, interval = 1000, bad = 2, good = 1;
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout
                    << "ServiceWatch 1.0.0 - bounded TCP availability checks\n"
                       "Usage: servicewatch --config FILE [OPTIONS]\n"
                       "  --rounds N              1..100 (default 1)\n"
                       "  --workers N             1..16 (default 4)\n"
                       "  --timeout-ms N          1..30000 per connection (default 1000)\n"
                       "  --interval-ms N         0..60000 between completed rounds (1000)\n"
                       "  --failure-threshold N   1..10 consecutive failures (default 2)\n"
                       "  --recovery-threshold N  1..10 consecutive successes (default 1)\n"
                       "Output: JSON Lines, one observation per endpoint per completed round.\n"
                       "Exit: 0 final states UP; 1 any final state UNKNOWN/DOWN; 2 "
                       "arguments/config; 3 runtime/I/O.\n";
                return std::cout ? 0 : 3;
            }
            if (arg == "--version") {
                std::cout << "ServiceWatch 1.0.0\n";
                return std::cout ? 0 : 3;
            }
            if (!seen.insert(arg).second || i + 1 == argc)
                throw std::invalid_argument("duplicate option or missing value");
            const std::string value = argv[++i];
            if (arg == "--config")
                config = value;
            else if (arg == "--rounds")
                rounds = number(value, 1, 100);
            else if (arg == "--workers")
                workers = number(value, 1, 16);
            else if (arg == "--timeout-ms")
                timeout = number(value, 1, 30000);
            else if (arg == "--interval-ms")
                interval = number(value, 0, 60000);
            else if (arg == "--failure-threshold")
                bad = number(value, 1, 10);
            else if (arg == "--recovery-threshold")
                good = number(value, 1, 10);
            else
                throw std::invalid_argument("unknown option");
        }
        if (config.empty())
            throw std::invalid_argument("--config is required");
        std::ifstream input(config, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open configuration");
        sw::Network network;
        const auto endpoints = sw::read_config(input);
        std::vector<sw::Health> health(endpoints.size());
        for (unsigned round = 1; round <= rounds; ++round) {
            const auto results = sw::check_round(endpoints, workers,
                                                 std::chrono::milliseconds(timeout), sw::tcp_probe);
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                const auto previous = health[i].state;
                const bool changed = health[i].update(results[i].reachable, bad, good);
                const auto &e = endpoints[i];
                const auto &r = results[i];
                // Config permits only ASCII names and numeric IPs, so these strings need no
                // escaping.
                std::cout << "{\"schema_version\":1,\"round\":" << round << ",\"name\":\"" << e.name
                          << "\",\"address\":\"" << e.address << "\",\"port\":" << e.port
                          << ",\"reachable\":" << (r.reachable ? "true" : "false")
                          << ",\"state\":\"" << sw::label(health[i].state)
                          << "\",\"previous_state\":\"" << sw::label(previous)
                          << "\",\"changed\":" << (changed ? "true" : "false")
                          << ",\"consecutive_successes\":" << health[i].successes
                          << ",\"consecutive_failures\":" << health[i].failures
                          << ",\"elapsed_ms\":" << r.elapsed_ms << ",\"reason\":\"" << r.reason
                          << "\"}\n";
            }
            std::cout.flush();
            if (!std::cout)
                throw std::runtime_error("report write failed");
            if (round < rounds)
                std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
        for (const auto &h : health)
            if (h.state != sw::State::up)
                return 1;
        return 0;
    } catch (const std::invalid_argument &e) {
        std::cerr << "servicewatch: " << e.what() << "\n";
        return 2;
    } catch (const std::exception &e) {
        std::cerr << "servicewatch: " << e.what() << "\n";
        return 3;
    }
}
