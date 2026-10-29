#include "servicewatch.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <exception>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
namespace sw {
const char *label(State s) {
    switch (s) {
    case State::up:
        return "UP";
    case State::down:
        return "DOWN";
    default:
        return "UNKNOWN";
    }
}
bool Health::update(bool ok, unsigned bad_limit, unsigned good_limit) {
    if (!bad_limit || !good_limit)
        throw std::invalid_argument("thresholds must be positive");
    const State old = state;
    if (ok) {
        failures = 0;
        if (successes < good_limit)
            ++successes;
        if (successes >= good_limit)
            state = State::up;
    } else {
        successes = 0;
        if (failures < bad_limit)
            ++failures;
        if (failures >= bad_limit)
            state = State::down;
    }
    return old != state;
}
static std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}
std::vector<Endpoint> read_config(std::istream &in) {
    std::vector<Endpoint> result;
    std::set<std::string> names;
    std::string line;
    std::size_t total = 0, number = 0;
    // Bound the whole configuration, including comments, before retaining input.
    char ch;
    auto consume = [&] {
        ++number;
        std::string s = trim(line);
        line.clear();
        if (s.empty() || s[0] == '#')
            return;
        auto error = [&] {
            return std::invalid_argument("invalid configuration at line " + std::to_string(number));
        };
        const auto a = s.find(','), b = a == std::string::npos ? a : s.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos ||
            s.find(',', b + 1) != std::string::npos)
            throw error();
        Endpoint e{trim(s.substr(0, a)), trim(s.substr(a + 1, b - a - 1)), 0};
        if (e.name.empty() || e.name.size() > 48)
            throw error();
        for (unsigned char c : e.name)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.'))
                throw error();
        if (!numeric_address(e.address))
            throw error();
        auto port = trim(s.substr(b + 1));
        unsigned value = 0;
        auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0 ||
            value > 65535)
            throw error();
        e.port = static_cast<std::uint16_t>(value);
        if (!names.insert(e.name).second)
            throw error();
        if (result.size() == 64)
            throw std::invalid_argument("at most 64 endpoints are supported");
        result.push_back(std::move(e));
    };
    while (in.get(ch)) {
        if (++total > 65536)
            throw std::invalid_argument("configuration exceeds 65536 bytes");
        if (ch == '\n')
            consume();
        else {
            if (ch == '\0' || (static_cast<unsigned char>(ch) < 32 && ch != '\r' && ch != '\t'))
                throw std::invalid_argument("configuration contains control bytes");
            if (line.size() == 256)
                throw std::invalid_argument("configuration line exceeds 256 bytes");
            line += ch;
        }
    }
    if (in.bad() || (!in.eof() && in.fail()))
        throw std::runtime_error("configuration read failed");
    if (!line.empty())
        consume();
    if (result.empty())
        throw std::invalid_argument("configuration has no endpoints");
    return result;
}
std::vector<Observation> check_round(const std::vector<Endpoint> &endpoints, unsigned workers,
                                     std::chrono::milliseconds timeout, const Probe &probe) {
    if (!workers || workers > 16 || endpoints.empty() || endpoints.size() > 64 ||
        timeout.count() < 1 || timeout.count() > 30000)
        throw std::invalid_argument("invalid round limits");
    std::vector<Observation> results(endpoints.size());
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::exception_ptr failure;
    std::mutex guard;
    std::vector<std::thread> threads;
    auto join = [&] {
        for (auto &t : threads)
            if (t.joinable())
                t.join();
    };
    auto run = [&] {
        try {
            while (!stop.load()) {
                const auto i = next.fetch_add(1);
                if (i >= endpoints.size())
                    break;
                results[i] = probe(endpoints[i], timeout);
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(guard);
                if (!failure)
                    failure = std::current_exception();
            }
            stop.store(true);
        }
    };
    try {
        for (std::size_t i = 0; i < std::min<std::size_t>(workers, endpoints.size()); ++i)
            threads.emplace_back(run);
    } catch (...) {
        stop.store(true);
        join();
        throw;
    }
    join();
    if (failure)
        std::rethrow_exception(failure);
    return results;
}
} // namespace sw
