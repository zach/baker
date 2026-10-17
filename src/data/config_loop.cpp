// Config's reload history and loop statistics: what a reload records, and what a
// host reads back.

#include "config_impl.hpp"

#include <algorithm>

namespace baker {

size_t Config::Impl::finish(const Config& c, ReloadRecord& r, uint64_t end) {
    size_t slot = SIZE_MAX;
    if constexpr (!kShipping) {
        r.epoch = c.epoch_;
        r.end_ns = end;
        if (ring_cap) {
            if (ring.capacity() < ring_cap) ring.reserve(ring_cap);
            slot = ring.size() < ring_cap ? ring.size() : ring_next;
            if (ring.size() < ring_cap)
                ring.push_back(r);
            else
                ring[ring_next] = r;
            ring_next = (ring_next + 1) % ring_cap;
        }
        for (size_t i = 0; i < completions.size(); ++i) completions[i].fn(r);
    }
    return slot;
}

void Config::Impl::feed(Config& c, const ReloadRecord& r, size_t slot) {
    if constexpr (!kShipping) {
        ++loop_reloads;
        if (r.outcome == ReloadOutcome::Unchanged) ++loop_unchanged;
        const bool ok = r.outcome == ReloadOutcome::Ok;
        if (!ok && r.outcome != ReloadOutcome::Unchanged) ++loop_failures;
        if (r.first_file.empty() || !(ok || r.outcome == ReloadOutcome::ParseError)) return;
        if (!loop) loop = Own<LoopTable>::make(alloc);
        Pending p{slot,
                  r.start_ns,
                  r.epoch,
                  r.origin_ns,
                  {UINT32_MAX, UINT32_MAX},
                  {},
                  {UINT32_MAX, UINT32_MAX}};
        const bool edit = ok && r.origin_ns && r.end_ns >= r.origin_ns;
        for (int k = 0; k < 2; ++k) {
            const Symbol name = k == 0 ? r.first_file : r.layer;
            if (name.empty()) continue;
            const uint32_t i = loop->row(name, k == 1);
            LoopTable::Row& row = loop->rows[i];
            p.rows[k] = i;
            p.generation[k] = row.generation;
            ++row.reloads;
            if (!ok) {
                ++row.failures;
                continue;
            }
            row.time_us[row.times++ % LoopTable::kWindow] =
                LoopTable::micros(r.end_ns - r.start_ns);
            if (edit) {
                p.seq[k] = row.edits;
                row.edit_us[row.edits++ % LoopTable::kWindow] =
                    LoopTable::micros(r.end_ns - r.origin_ns);
            }
        }
        if (!ok) return;
        if (pending_n == kPending) {
            std::copy(pending + 1, pending + kPending, pending);
            --pending_n;
        }
        pending[pending_n++] = p;
        c.visible_pending_ = true;
    }
}

void Config::set_time_source(TimeSource fn, void* user) {
    impl_->clock = fn ? fn : steady_now;
    impl_->clock_user = fn ? user : nullptr;
}

void Config::set_reload_history(size_t capacity) {
    impl_->ring = Vec<ReloadRecord>();
    impl_->ring_cap = capacity;
    impl_->ring_next = 0;
    if (capacity) impl_->ring.reserve(capacity);
}

size_t Config::history_size() const noexcept {
    return impl_->ring.size();
}

const ReloadRecord& Config::history_at(size_t i) const noexcept {
    const auto& ring = impl_->ring;
    const size_t oldest = ring.size() < impl_->ring_cap ? 0 : impl_->ring_next;
    return ring[(oldest + i) % ring.size()];
}

void Config::stamp_visible() noexcept {
    visible_pending_ = false;
    if constexpr (!kShipping) {
        Impl& m = *impl_;
        const uint64_t now = m.now();
        for (uint32_t i = 0; i < m.pending_n; ++i) {
            const Impl::Pending& p = m.pending[i];
            if (p.slot < m.ring.size()) {
                ReloadRecord& r = m.ring[p.slot];
                if (r.start_ns == p.start_ns && r.epoch == p.epoch && !r.visible_ns)
                    r.visible_ns = now;
            }
            // Edit-to-see runs to now: rewrite the sample the reload's end gave, if its row
            // has not changed hands and the window still holds it.
            if (!m.loop || !p.origin_ns || now < p.origin_ns) continue;
            for (int k = 0; k < 2; ++k) {
                if (p.rows[k] == UINT32_MAX || p.seq[k] == UINT32_MAX) continue;
                LoopTable::Row& row = m.loop->rows[p.rows[k]];
                if (row.generation != p.generation[k] || row.edits > p.seq[k] + LoopTable::kWindow)
                    continue;
                row.edit_us[p.seq[k] % LoopTable::kWindow] = LoopTable::micros(now - p.origin_ns);
            }
        }
        m.pending_n = 0;
    }
}

LoopStats Config::loop_stats() const {
    LoopStats s;
    if constexpr (!kShipping) {
        const Impl& m = *impl_;
        s.reloads = m.loop_reloads;
        s.unchanged = m.loop_unchanged;
        s.failures = m.loop_failures;
        if (!m.loop) return s;
        auto rows = [&](uint32_t first, uint32_t count) {
            Vec<const LoopTable::Row*> used(m.alloc);
            for (uint32_t i = first; i < first + count; ++i)
                if (m.loop->rows[i].used) used.push_back(&m.loop->rows[i]);
            std::sort(used.begin(), used.end(),
                      [](const auto* a, const auto* b) { return a->used > b->used; });
            // The middle sample (the upper one of an even count), and the largest.
            auto summary = [](const uint32_t* window, uint32_t pushed, uint32_t& n,
                              uint64_t& median, uint64_t& worst) {
                n = std::min(pushed, LoopTable::kWindow);
                if (!n) return;
                uint32_t v[LoopTable::kWindow];
                std::copy(window, window + n, v);
                std::nth_element(v, v + n / 2, v + n);
                median = uint64_t{v[n / 2]} * 1000;
                worst = uint64_t{*std::max_element(v, v + n)} * 1000;
            };
            Vec<LoopRow> out(m.alloc);
            for (const LoopTable::Row* r : used) {
                LoopRow row;
                row.name = r->name;
                row.reloads = r->reloads;
                row.failures = r->failures;
                summary(r->edit_us, r->edits, row.edits, row.edit_median_ns, row.edit_worst_ns);
                summary(r->time_us, r->times, row.times, row.reload_median_ns, row.reload_worst_ns);
                out.push_back(row);
            }
            return List<LoopRow>(std::span<const LoopRow>(out.data(), out.size()));
        };
        s.files = rows(0, LoopTable::kFiles);
        s.layers = rows(LoopTable::kFiles, LoopTable::kLayers);
    }
    return s;
}

std::optional<ReloadRecord> Config::last_reload() const noexcept {
    if (impl_->ring.empty()) return std::nullopt;
    return history_at(impl_->ring.size() - 1);
}

}  // namespace baker
