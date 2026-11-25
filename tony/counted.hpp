#pragma once
#include <string>
#include <utility>
inline int moves = 0;
struct counted {
    std::string s;
    explicit counted(std::string v) : s(std::move(v)) {}
    counted(const counted& o) : s(o.s) { ++moves; }
    counted(counted&& o) noexcept : s(std::move(o.s)) { ++moves; }
};
struct pinned {
    int v;
    explicit pinned(int x) : v(x) {}
    pinned(pinned&&) = delete;
};
