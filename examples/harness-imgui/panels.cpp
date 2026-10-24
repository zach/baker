#include "panels.hpp"

#include <baker/std_script.hpp>
#include <baker/tools/layers.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace harness {

namespace {

// A Path from a std::string: Path takes a string_view, and two implicit conversions
// are one too many.
baker::Path path_of(const std::string& s) {
    return baker::Path(std::string_view(s));
}

std::string error_text(const baker::Error& e) {
    return std::string(baker::to_string(e));
}

std::string file_name(baker::Symbol file) {
    std::string f(file.str());
    const size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? f : f.substr(slash + 1);
}

// Shows `path`'s provenance as a tooltip while the last item is hovered.
void hover_provenance(const baker::Config& cfg, std::string_view path) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", provenance(cfg, path).c_str());
}

// A tweak's values as numbers, if every one is a number.
bool numbers(const baker::TweakBase& t, std::vector<double>& out, bool& ints) {
    baker::ValueList list;
    t.values(list);
    out.clear();
    ints = true;
    for (const baker::Node& n : list.nodes()) {
        auto v = n.try_float();
        if (!v) return false;
        if (n.type() != baker::Type::Int) ints = false;
        out.push_back(*v);
    }
    return !out.empty() && out.size() <= 4;
}

// One widget for a tweak's numbers; true if the user changed them.
bool number_widget(const char* label, std::vector<double>& v, bool ints, baker::TweakRange range,
                   std::string_view kind) {
    float f[4] = {};
    for (size_t i = 0; i < v.size(); ++i) f[i] = static_cast<float>(v[i]);
    const int n = static_cast<int>(v.size());
    bool changed = false;
    if (kind == "color" && (n == 3 || n == 4)) {
        changed = n == 3 ? ImGui::ColorEdit3(label, f) : ImGui::ColorEdit4(label, f);
    } else if (ints && n == 1) {
        int i = static_cast<int>(v[0]);
        changed = range.empty() ? ImGui::DragInt(label, &i)
                                : ImGui::SliderInt(label, &i, static_cast<int>(range.min),
                                                   static_cast<int>(range.max));
        f[0] = static_cast<float>(i);
    } else if (range.empty()) {
        changed = ImGui::DragScalarN(label, ImGuiDataType_Float, f, n, 0.01f);
    } else {
        const float lo = static_cast<float>(range.min), hi = static_cast<float>(range.max);
        changed = ImGui::SliderScalarN(label, ImGuiDataType_Float, f, n, &lo, &hi);
    }
    if (changed)
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = ints ? static_cast<double>(static_cast<int>(f[i])) : f[i];
    return changed;
}

baker::TweakBase* find_tweak(baker::TweakRegistry& tweaks, std::string_view path) {
    baker::TweakBase* found = nullptr;
    tweaks.each([&](baker::TweakBase& t) {
        if (t.path() == path) found = &t;
    });
    return found;
}

std::string written(const baker::WriteReport& r) {
    std::string s = "saved:";
    for (const baker::WrittenLine& w : r.written)
        s += " " + file_name(w.file) + ":" + std::to_string(w.line);
    return s;
}

}  // namespace

std::string provenance(const baker::Config& cfg, std::string_view path) {
    auto s = cfg.source(path);
    if (!s) return error_text(s.error());
    std::string text;
    if (s->file.empty())
        text = "unsaved (layer " + std::string(s->layer.str()) + ")";
    else
        text = file_name(s->file) + ":" + std::to_string(s->line) + " (layer " +
               std::string(s->layer.str()) + ")";
    // The data's note, if it has one: (speed 4 (note "..."))
    const std::string note_path = std::string(path) + ".note";
    if (auto note = cfg.root()->get<std::string_view>(path_of(note_path), std::string_view());
        note && !note->empty())
        text += "\nnote: " + std::string(*note);
    return text;
}

// --- Tweaks ---

void tweaks_panel(baker::Config& cfg, baker::TweakRegistry& tweaks, TweaksState& state) {
    if (ImGui::Begin("Tweaks")) {
        std::string group;
        tweaks.each([&](baker::TweakBase& t) {
            if (t.group() != group) {
                group = std::string(t.group());
                ImGui::SeparatorText(group.c_str());
            }
            ImGui::PushID(&t);
            std::vector<double> v;
            bool ints = false;
            const std::string label(t.path());
            if (numbers(t, v, ints)) {
                if (number_widget(label.c_str(), v, ints, t.range(), t.kind()))
                    state.request = {TweaksState::Request::Set, label, v};
            } else {
                baker::ValueList list;
                t.values(list);
                std::string text;
                for (const baker::Node& n : list.nodes()) text += baker::to_string(n) + " ";
                ImGui::LabelText(label.c_str(), "%s", text.c_str());
            }
            hover_provenance(cfg, t.path());
            if (!t.desc().empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%.*s", int(t.desc().size()), t.desc().data());
            }
            // Unsaved: offer to save it where it came from, or to keep it for this user.
            auto src = cfg.source(t.path());
            if (src && src->file.empty() && src->layer == "scratch") {
                if (ImGui::SmallButton("Save"))
                    state.request = {TweaksState::Request::Save, label, {}};
                ImGui::SameLine();
                if (ImGui::SmallButton("Keep for me"))
                    state.request = {TweaksState::Request::KeepLocal, label, {}};
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert"))
                    state.request = {TweaksState::Request::Revert, label, {}};
            }
            ImGui::PopID();
        });
        if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::End();

    if (!state.request) return;
    const TweaksState::Request r = std::move(*state.request);
    state.request.reset();
    baker::TweakBase* t = find_tweak(tweaks, r.path);
    if (!t) {
        state.status = "no tweak " + r.path;
        return;
    }
    switch (r.kind) {
        case TweaksState::Request::Set: {
            // Ints stay ints, so a saved value reads as it was written.
            std::vector<double> old;
            bool ints = false;
            numbers(*t, old, ints);
            baker::ValueList list;
            for (double d : r.values)
                list.push(ints ? baker::Node(static_cast<int64_t>(d)) : baker::Node(d));
            auto set = t->set_values(list.nodes());
            state.status = set ? "" : error_text(set.error());
            break;
        }
        case TweaksState::Request::Save: {
            auto w = cfg.write_back(path_of(r.path));
            state.status = w ? written(*w) : error_text(w.error());
            break;
        }
        case TweaksState::Request::KeepLocal: {
            auto w = cfg.write_back_to(path_of(r.path), "local");
            state.status = w ? written(*w) : error_text(w.error());
            break;
        }
        case TweaksState::Request::Revert: {
            auto c = cfg.clear_scratch(r.path);
            state.status = c ? "reverted " + r.path : error_text(c.error());
            break;
        }
    }
}

// --- Shapes ---

namespace {

// A path segment, quoted if it needs to be.
std::string segment(baker::Symbol s) {
    std::string out(s.str());
    if (out.find_first_of("./ '") != std::string::npos ||
        out.find_first_not_of("0123456789") == std::string::npos)
        return "'" + out + "'";
    return out;
}

bool at_matches(const baker::shape::Shape& shape, const std::vector<baker::Symbol>& ancestors) {
    if (shape.at.empty()) return true;
    if (shape.at.size() != ancestors.size()) return false;
    for (size_t i = 0; i < ancestors.size(); ++i)
        if (shape.at[i] != "*" && shape.at[i] != ancestors[i]) return false;
    return true;
}

struct ShapeWalk {
    baker::Config& cfg;
    const baker::shape::Set& set;
    ShapesState& state;
    std::vector<baker::Symbol> tags;  // ancestors' tags, from the root
    std::string path;                 // the same, as a dotted path

    void field(const baker::Array& a, const baker::shape::Field& f) {
        const std::string p = path + "." + segment(f.name);
        const baker::Array* child = a.try_find(f.name);
        ImGui::PushID(p.c_str());
        const std::string label(f.name.str());
        const baker::Node* value = child && child->size() > 1 ? &(*child)[1] : nullptr;
        using T = baker::shape::Type;
        const bool numeric = value && value->try_float() &&
                             (f.type == T::Int || f.type == T::Float || f.type == T::Bool);
        if (numeric && f.type == T::Bool) {
            bool b = *value->try_float() != 0;
            if (ImGui::Checkbox(label.c_str(), &b))
                state.request = {ShapesState::Request::Set, p, b ? 1.0 : 0.0};
        } else if (numeric && f.type == T::Int) {
            int i = static_cast<int>(*value->try_int());
            const bool changed = f.ranged
                                     ? ImGui::SliderInt(label.c_str(), &i, int(f.min), int(f.max))
                                     : ImGui::DragInt(label.c_str(), &i);
            if (changed) state.request = {ShapesState::Request::Set, p, double(i)};
        } else if (numeric) {
            float x = static_cast<float>(*value->try_float());
            const bool changed =
                f.ranged ? ImGui::SliderFloat(label.c_str(), &x, float(f.min), float(f.max))
                         : ImGui::DragFloat(label.c_str(), &x, 0.01f);
            if (changed) state.request = {ShapesState::Request::Set, p, x};
        } else {
            std::string text = value ? baker::to_string(*value) : "(missing)";
            ImGui::LabelText(label.c_str(), "%s", text.c_str());
        }
        if (child) hover_provenance(cfg, p);
        if (!f.desc.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%.*s", int(f.desc.size()), f.desc.data());
        }
        auto src = child ? cfg.source(path_of(p)) : baker::Expected<baker::Source>(baker::Source{});
        if (child && src && src->layer == "scratch") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Save")) state.request = {ShapesState::Request::Save, p, 0};
        }
        ImGui::PopID();
    }

    void walk(const baker::Array& a) {
        for (const baker::Array& sub : a.subarrays()) {
            const baker::Array* child = &sub;
            if (child->empty()) continue;
            auto tag = (*child)[0].try_sym();
            if (!tag || *tag == "shapes") continue;
            // A path reaches only the first child with a tag.
            if (a.try_find(*tag) != child) continue;
            const size_t len = path.size();
            path += (path.empty() ? "" : ".") + segment(*tag);
            if (const baker::shape::Shape* s = set.find(*tag); s && at_matches(*s, tags)) {
                if (ImGui::TreeNode(path.c_str())) {
                    for (const baker::shape::Field& f : s->fields) field(*child, f);
                    ImGui::TreePop();
                }
            }
            tags.push_back(*tag);
            walk(*child);
            tags.pop_back();
            path.resize(len);
        }
    }
};

}  // namespace

void shapes_panel(baker::Config& cfg, ShapesState& state) {
    baker::Ref<baker::Array> root = cfg.root();
    if (ImGui::Begin("Shapes")) {
        baker::shape::Set set;
        baker::shape::Report report;
        set.add(*root, report);
        if (set.shapes().empty()) ImGui::TextDisabled("No (shapes ...) in the data.");
        ShapeWalk{cfg, set, state, {}, {}}.walk(*root);
        if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::End();

    if (!state.request) return;
    const ShapesState::Request r = std::move(*state.request);
    state.request.reset();
    if (r.kind == ShapesState::Request::Set) {
        // An int field stays an int.
        const baker::Node old = root->get<baker::Node>(path_of(r.path)).value_or(baker::Node());
        const baker::Node value = old.type() == baker::Type::Int
                                      ? baker::Node(static_cast<int64_t>(r.value))
                                      : baker::Node(r.value);
        auto set = cfg.set(r.path, value);
        state.status = set ? "" : error_text(set.error());
    } else {
        auto w = cfg.write_back(path_of(r.path));
        state.status = w ? written(*w) : error_text(w.error());
    }
}

// --- Console ---

void console_panel(baker::Context& ctx, ConsoleState& state) {
    if (ImGui::Begin("Console")) {
        const float footer = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("log", ImVec2(0, -footer))) {
            for (const std::string& line : state.log) ImGui::TextWrapped("%s", line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        if (ImGui::InputText("##eval", state.input, sizeof state.input,
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            state.submit = true;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Eval")) state.submit = true;
    }
    ImGui::End();

    if (!state.submit) return;
    state.submit = false;
    const std::string line = state.input;
    state.input[0] = 0;
    if (line.empty()) return;
    state.history.push_back(line);
    state.log.push_back("> " + line);
    auto r = ctx.eval(line, "<console>");
    if (r) {
        state.log.push_back("=> " + baker::to_string(*r));
    } else {
        std::string text(baker::describe(r.error()));
        for (size_t start = 0, end; start < text.size(); start = end + 1) {
            end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            if (end > start) state.log.push_back(text.substr(start, end - start));
        }
    }
}

// --- Loop ---

void loop_panel(const baker::Config& cfg) {
    if (ImGui::Begin("Loop")) {
        if constexpr (baker::kShipping) {
            ImGui::TextDisabled("Reload telemetry is compiled out of shipping builds.");
        } else {
            static const ImU32 kColors[7] = {
                IM_COL32(120, 120, 200, 255), IM_COL32(170, 120, 200, 255),
                IM_COL32(90, 170, 220, 255),  IM_COL32(90, 200, 140, 255),
                IM_COL32(220, 200, 90, 255),  IM_COL32(230, 140, 80, 255),
                IM_COL32(210, 90, 120, 255)};
            // Per file and per layer: edit-to-see (from a saved edit to the frame showing it)
            // and reload time, median and worst of the last 32.
            const baker::LoopStats stats = cfg.loop_stats();
            ImGui::Text("%u reloads, %u unchanged, %u failed", stats.reloads, stats.unchanged,
                        stats.failures);
            auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
            if (ImGui::BeginTable("loop", 6,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                for (const char* h :
                     {"file or layer", "reloads", "failed", "edit to see", "reload", ""})
                    ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                auto rows = [&](const baker::List<baker::LoopRow>& list, bool files) {
                    for (const baker::LoopRow& r : list) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const std::string name =
                            files ? file_name(r.name) : std::string(r.name.str());
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", r.reloads);
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", r.failures);
                        ImGui::TableNextColumn();
                        if (r.edits)
                            ImGui::Text("%.1f / %.1f ms", ms(r.edit_median_ns),
                                        ms(r.edit_worst_ns));
                        else
                            ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (r.times)
                            ImGui::Text("%.2f / %.2f ms", ms(r.reload_median_ns),
                                        ms(r.reload_worst_ns));
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled(files ? "file" : "layer");
                    }
                };
                rows(stats.layers, false);
                rows(stats.files, true);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("median / worst of the last 32 edits");
            ImGui::Separator();
            ImGui::TextDisabled("scan write read parse merge bind notify; newest first");
            // A host that polls for changes makes a record per check; show the changes.
            std::vector<const baker::ReloadRecord*> records;
            size_t unchanged = 0;
            cfg.each_reload([&](const baker::ReloadRecord& r) {
                if (r.outcome == baker::ReloadOutcome::Unchanged)
                    ++unchanged;
                else
                    records.push_back(&r);
            });
            ImGui::TextDisabled("%zu checks found nothing changed", unchanged);
            uint64_t longest = 1;
            for (const auto* r : records) longest = std::max(longest, r->end_ns - r->start_ns);
            const float width = std::max(ImGui::GetContentRegionAvail().x - 220.0f, 50.0f);
            for (auto it = records.rbegin(); it != records.rend() && it - records.rbegin() < 32;
                 ++it) {
                const baker::ReloadRecord& r = **it;
                const uint64_t stages[7] = {r.scan_ns,  r.write_ns, r.read_ns,  r.parse_ns,
                                            r.merge_ns, r.bind_ns,  r.notify_ns};
                const char* outcome = r.outcome == baker::ReloadOutcome::ParseError
                                          ? " (parse error)"
                                      : r.outcome == baker::ReloadOutcome::Refused ? " (refused)"
                                                                                   : "";
                ImGui::Text("%-10s %7.2f ms%s", r.trigger.c_str(),
                            double(r.end_ns - r.start_ns) / 1e6, outcome);
                ImGui::SameLine(200);
                ImVec2 at = ImGui::GetCursorScreenPos();
                const float h = ImGui::GetTextLineHeight();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                for (int s = 0; s < 7; ++s) {
                    const float w = width * float(stages[s]) / float(longest);
                    draw->AddRectFilled(at, ImVec2(at.x + w, at.y + h), kColors[s]);
                    at.x += w;
                }
                ImGui::Dummy(ImVec2(width, h));
                if (ImGui::IsItemHovered()) {
                    std::string tip = "epoch " + std::to_string(r.epoch) + ", " +
                                      std::to_string(r.files_reloaded) + " files, " +
                                      std::to_string(r.tweaks_changed) + " tweaks, " +
                                      std::to_string(r.migrated) + " migrated, " +
                                      std::to_string(r.punted) + " punted";
                    if (!r.first_file.empty()) tip += "\n" + file_name(r.first_file);
                    // Edit to see: from when the host says the edit was saved.
                    if (r.origin_ns && r.end_ns > r.origin_ns)
                        tip +=
                            "\nedit to effect: " + std::to_string((r.end_ns - r.origin_ns) / 1000) +
                            " us";
                    ImGui::SetTooltip("%s", tip.c_str());
                }
            }
        }
    }
    ImGui::End();
}

// --- Journal ---

namespace {
std::string values_text(const baker::Node* nodes, uint32_t count) {
    if (count == 0) return "(none)";
    std::string s;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, 4); ++i)
        s += (i ? " " : "") + baker::to_string(nodes[i]);
    if (count > 4) s += " ...";
    return s;
}
}  // namespace

std::string journal_line(const baker::JournalEntry& e) {
    std::string s = std::string(e.path.str()) + "  " + values_text(e.before, e.before_count) +
                    " -> " + values_text(e.after, e.after_count) + "  (";
    s += e.source.file.empty() ? std::string(e.source.layer.str())
                               : file_name(e.source.file) + ":" + std::to_string(e.source.line);
    s += ")  ";
    switch (e.fate) {
        case baker::EditFate::Unsaved: s += "unsaved"; break;
        case baker::EditFate::Superseded: s += "superseded"; break;
        case baker::EditFate::Reverted: s += "reverted"; break;
        case baker::EditFate::Written:
            s += "written " + file_name(e.written.file) + ":" + std::to_string(e.written.line);
            break;
    }
    return s;
}

void journal_panel(const baker::Config& cfg) {
    if (ImGui::Begin("Journal")) {
        std::vector<const baker::JournalEntry*> entries;
        cfg.journal([&](const baker::JournalEntry& e) { entries.push_back(&e); });
        size_t unsaved = 0, written = 0;
        for (const auto* e : entries) {
            unsaved += e->fate == baker::EditFate::Unsaved;
            written += e->fate == baker::EditFate::Written;
        }
        // Discoveries kept against discoveries at risk: what write-back is for.
        ImGui::Text("%zu edits: %zu written, %zu unsaved", entries.size(), written, unsaved);
        ImGui::Separator();
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            const baker::JournalEntry& e = **it;
            const std::string line = journal_line(e);
            const ImVec4 color = e.fate == baker::EditFate::Unsaved   ? ImVec4(1, 0.8f, 0.3f, 1)
                                 : e.fate == baker::EditFate::Written ? ImVec4(0.5f, 0.9f, 0.5f, 1)
                                                                      : ImVec4(0.6f, 0.6f, 0.6f, 1);
            ImGui::TextColored(color, "%s", line.c_str());
        }
    }
    ImGui::End();
}

// --- Layers ---

void layers_panel(baker::Config& cfg, LayersState& state) {
    const std::vector<std::string> names = baker::strings(cfg.layers());
    if (ImGui::Begin("Layers")) {
        ImGui::TextDisabled("bottom to top; hover a value for where it comes from");
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            const std::string& name = *it;
            ImGui::PushID(name.c_str());
            bool on = cfg.layer_enabled(name);
            if (ImGui::Checkbox("##on", &on)) state.toggle = name;
            ImGui::SameLine();
            if (ImGui::TreeNode(name.c_str())) {
                if (auto effect = baker::tools::layer_effect(cfg, name)) {
                    if (effect->empty()) ImGui::TextDisabled("changes nothing");
                    for (const baker::LayerEffectEntry& e : *effect) {
                        ImGui::Text("%s%s: %s -> %s", e.shadowed ? "(shadowed) " : "",
                                    e.path.c_str(),
                                    e.below ? baker::to_string(*e.below).c_str() : "(none)",
                                    baker::to_string(e.value).c_str());
                        hover_provenance(cfg, e.path.str());
                    }
                } else {
                    ImGui::TextWrapped("%s", error_text(effect.error()).c_str());
                }
                if (name != "scratch" && ImGui::Button("Promote into"))
                    state.promote = LayersState::Promote{name, state.promote_to};
                if (name != "scratch") {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140);
                    if (ImGui::BeginCombo("##to", state.promote_to.c_str())) {
                        for (const std::string& to : names)
                            if (to != "scratch" && to != name &&
                                ImGui::Selectable(to.c_str(), to == state.promote_to))
                                state.promote_to = to;
                        ImGui::EndCombo();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        for (const std::string& line : state.report) ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::End();

    if (state.toggle) {
        auto r = cfg.toggle_layer(*state.toggle);
        state.report = {r ? *state.toggle + (*r ? " enabled" : " disabled")
                          : error_text(r.error())};
        state.toggle.reset();
    }
    if (state.promote) {
        // All or nothing: on any refusal nothing is written, and every refusal is listed.
        auto r = cfg.promote(state.promote->from, state.promote->to);
        state.report.clear();
        if (!r) {
            state.report.push_back(error_text(r.error()));
        } else if (!r->ok()) {
            for (const baker::Error& e : r->refused)
                state.report.push_back("refused: " + error_text(e));
        } else {
            state.report.push_back("promoted " + state.promote->from + " into " +
                                   state.promote->to);
            for (const baker::WrittenLine& w : r->written)
                state.report.push_back("  " + file_name(w.file) + ":" + std::to_string(w.line));
        }
        state.promote.reset();
    }
}

// --- Snapshots ---

void snapshots_panel(baker::Config& cfg, SnapshotsState& state) {
    if (ImGui::Begin("Snapshots")) {
        if (ImGui::Button("Capture A")) state.request = SnapshotsState::CaptureA;
        ImGui::SameLine();
        if (ImGui::Button("Capture B")) state.request = SnapshotsState::CaptureB;
        ImGui::SameLine();
        ImGui::BeginDisabled(state.a.empty() || state.b.empty());
        if (ImGui::Button("Flip")) state.request = SnapshotsState::Flip;
        ImGui::EndDisabled();
        ImGui::Text("A: %s   B: %s   showing: %c", state.a.empty() ? "-" : "captured",
                    state.b.empty() ? "-" : "captured", state.showing ? state.showing : '-');
        if (!state.status.empty()) ImGui::TextWrapped("%s", state.status.c_str());
    }
    ImGui::End();

    switch (state.request) {
        case SnapshotsState::None: return;
        case SnapshotsState::CaptureA: state.a = cfg.snapshot(); break;
        case SnapshotsState::CaptureB: state.b = cfg.snapshot(); break;
        case SnapshotsState::Flip: {
            if (state.a.empty() || state.b.empty()) break;
            const char next = state.showing == 'A' ? 'B' : 'A';
            auto r = cfg.restore(next == 'A' ? state.a : state.b);
            if (r) state.showing = next;
            state.status = r ? "" : error_text(r.error());
            break;
        }
    }
    state.request = SnapshotsState::None;
}

}  // namespace harness
