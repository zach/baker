#pragma once

// Where the names a thread interns come from, for a hash collision's report
// (set_symbol_collision_handler): the file a parser is reading and its current line, or the
// cache a load is reading. Set once per file, never per name; symbol.cpp keeps it.

#include <baker/error.hpp>
#include <baker/symbol.hpp>

#include <cstdint>

namespace baker::detail {

struct InternSite {
    Symbol file;
    const int32_t* line = nullptr;  // the reader's current line; null: none
};

// Sets this thread's site and returns the one it replaces, to put back.
InternSite set_intern_site(InternSite site) noexcept;

// The names a read interns that are new to the table, against ParseLimits::max_new_symbols.
// Past the limit, the table sets *trip to trip_at, the read's node limit, so the parser's
// node check, which it makes anyway, fails next and finds why here.
struct NewSymbols {
    uint64_t count = 0;
    uint64_t limit = 0;
    uint64_t* trip = nullptr;
    uint64_t trip_at = 0;
};

// Counts each name this thread interns that is new to the table into `*counter`, until it is
// set again; null counts none. Returns the counter it replaces, to put back. In every build.
NewSymbols* count_new_symbols(NewSymbols* counter) noexcept;

// Around a call into the host (its Reader, its Lister): neither the counter nor the site is
// this thread's while the host runs, since a host may switch fibers there, and another read
// on this thread must not count into this one's, or into a read that has ended.
class HostCallScope {
public:
    HostCallScope() noexcept : counter_(count_new_symbols(nullptr)) {
        if constexpr (!kShipping) site_ = set_intern_site({});
    }
    ~HostCallScope() {
        (void)count_new_symbols(counter_);
        if constexpr (!kShipping) (void)set_intern_site(site_);
    }
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;

private:
    NewSymbols* counter_;
    InternSite site_;
};

// The site for a scope, in a build that is not shipping; the outer one again after it.
class InternSiteScope {
public:
    explicit InternSiteScope(std::string_view file) {
        if constexpr (!kShipping) outer_ = set_intern_site({Symbol(file), nullptr});
    }
    ~InternSiteScope() {
        if constexpr (!kShipping) (void)set_intern_site(outer_);
    }
    InternSiteScope(const InternSiteScope&) = delete;
    InternSiteScope& operator=(const InternSiteScope&) = delete;

private:
    InternSite outer_;
};

}  // namespace baker::detail
