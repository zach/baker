#include <baker/parse.hpp>

#include "arena.hpp"
#include "common/containers.hpp"
#include "lexer.hpp"
#include "merge.hpp"
#include "session.hpp"
#include "text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace baker {
namespace detail {
namespace {

bool read_disk_file(std::string_view path, TextSink out) {
    std::FILE* f = std::fopen(CPath(path).c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) out(std::string_view(buf, got));
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

}  // namespace

bool read_with(const LoadSession::Reader& reader, std::string_view path, Str& text) {
    text.clear();
    return reader(path, detail::str_out(text));
}

uint64_t list_with(const LoadSession::Lister& lister, std::string_view dir, Vec<Str>& entries,
                   const Allocator& alloc) {
    entries.clear();
    if (!lister(dir, [&](std::string_view p) { entries.emplace_back(p, alloc); })) return 0;
    std::sort(entries.begin(), entries.end());
    Str all(alloc);
    for (const Str& e : entries) all.append(e).append("\n");
    return content_hash(all) | 1;
}

Expected<void> directory_order(std::string_view text, std::string_view file,
                               std::span<const std::string_view> names, Vec<uint32_t>& order,
                               Vec<Error>& warnings) {
    order.clear();
    for (uint32_t i = 0; i < names.size(); ++i) order.push_back(i);
    LoadSession session;
    session.text_only = session.refuse_includes = true;
    auto tree = read_string(text, file, session);
    if (!tree) return Unexpected(std::move(tree).error());
    const Array* list = (*tree)->size() == 1 ? (**tree)[0].try_array() : nullptr;
    if (!list || list->tag() != Symbol("order"))
        return Unexpected(make_error(ErrorCode::Syntax, "order.dta holds (order NAME ...)",
                                     (*tree)->file(), list ? list->line() : 1));
    // The names it lists first, in its order; the rest stay in their order after them.
    size_t placed = 0;
    for (size_t i = 1; i < list->size(); ++i) {
        std::string_view name;
        if (const auto sym = (*list)[i].try_sym())
            name = sym->str();
        else if (const auto str = (*list)[i].try_str())
            name = *str;
        else
            return Unexpected(
                make_error(ErrorCode::Syntax, "order.dta lists names", list->file(), list->line()));
        size_t at = 0;
        while (at < order.size() && names[order[at]] != name) ++at;
        if (at < placed) continue;  // listed twice
        if (at == order.size()) {
            warnings.push_back(make_error(ErrorCode::NotFound,
                                          "order.dta lists '" + Str(name) + "', which '" +
                                              Str(parent_path(file)) + "' does not hold",
                                          list->file(), list->line()));
            continue;
        }
        std::rotate(order.begin() + static_cast<ptrdiff_t>(placed),
                    order.begin() + static_cast<ptrdiff_t>(at),
                    order.begin() + static_cast<ptrdiff_t>(at) + 1);
        ++placed;
    }
    return {};
}

}  // namespace detail

struct LoadSession::State {
    explicit State(const Allocator& a)
        : alloc(&a),
          defines(a),
          include_dirs(a),
          autorun(a),
          required(a),
          pack_levels(a),
          packs(a),
          origins(a),
          definitions(a),
          flags(a),
          sections(a),
          tested(a),
          macro_pack(a),
          warnings(a),
          files(a),
          defines_before(a),
          include_stack(a) {}
    // A copy of `o` from `a`: each table assigned, so it keeps `a`.
    State(const State& o, const Allocator& a) : State(a) {
        defines = o.defines;
        reader = o.reader;
        disk = o.disk;
        lister = o.lister;
        include_dirs = o.include_dirs;
        autorun = o.autorun;
        required = o.required;
        pack_levels = o.pack_levels;
        packs = o.packs;
        origins = o.origins;
        definitions = o.definitions;
        flags = o.flags;
        sections = o.sections;
        tested = o.tested;
        parses = o.parses;
        macro_pack = o.macro_pack;
        warnings = o.warnings;
        recorded = o.recorded;
        files = o.files;
        defines_before = o.defines_before;
        autorun_from = o.autorun_from;
        required_from = o.required_from;
        packs_from = o.packs_from;
        origins_from = o.origins_from;
        definitions_from = o.definitions_from;
        include_stack = o.include_stack;
        nodes = o.nodes;
        includes = o.includes;
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    const Allocator* alloc;
    HashMap<Symbol, Ref<Array>> defines;
    Reader reader = detail::read_disk_file;
    bool disk = true;  // `reader` is the default
    Lister lister = detail::list_disk_dir;
    Vec<Str> include_dirs;
    Vec<Node> autorun;
    Vec<Str> required;                 // the files #require has read, normalized
    Vec<Symbol> pack_levels;           // hardest first
    Vec<Pack> packs;                   // in the order they were read
    Vec<Origin> origins;               // files that declared one, in the order read
    Vec<Definition> definitions;       // macro definitions, in the order read
    Vec<ActiveDefine> flags;           // the flags that stand, in the order made
    Vec<ConditionalSection> sections;  // the #ifdef branches the reads took
    Vec<Symbol> tested;   // each name an #ifdef, #ifndef, #define or #undef named, as read
    uint64_t parses = 0;  // the files parsed: a cache's replay parses none
    HashMap<Symbol, uint32_t> macro_pack;  // the pack that defined each macro, by index
    Vec<Error> warnings;
    // What the last top-level read did, if recorded (record_for_cache).
    bool recorded = false;
    Vec<detail::CacheFile> files;
    HashMap<Symbol, Ref<Array>> defines_before;
    size_t autorun_from = 0;
    size_t required_from = 0;
    size_t packs_from = 0;
    size_t origins_from = 0;
    size_t definitions_from = 0;
    // Per top-level read.
    Vec<Str> include_stack;
    uint64_t nodes = 0;
    uint32_t includes = 0;
};

namespace detail {

class Parser {
public:
    Parser(LoadSession& session, Arena& arena, std::string_view text, Symbol file,
           uint32_t include_depth)
        : s_(session),
          arena_(arena),
          lex_(text, file),
          file_(file),
          include_depth_(include_depth),
          map_(session.source_map),
          size_(static_cast<uint32_t>(text.size())) {
        if (map_) map_->record_text(file, text);
        ++s_.st_->parses;
    }

    bool run(Ref<Array>& out, Error& err) {
        err_ = &err;
        frames_.push_back({Frame::Root, 0, 1, {}});
        Token t;
        for (;;) {
            if (!lex_.next(t, err)) return false;
            line_ = t.line;
            if (t.kind == Tok::End) break;
            if (!active()) {
                if (t.kind == Tok::Directive && !skipped_directive(t)) return false;
                continue;
            }
            bool ok = true;
            switch (t.kind) {
                case Tok::Open: ok = open(t.bracket, t.line, t.begin); break;
                case Tok::Close: ok = close(t); break;
                case Tok::Int: ok = push(Node(t.i), here(t)); break;
                case Tok::Float: ok = push(Node(t.f), here(t)); break;
                case Tok::String: ok = push(Node(arena_.make_string(t.text)), here(t)); break;
                case Tok::Symbol: ok = symbol(t); break;
                case Tok::Var: ok = push(Node::var(Symbol(t.text)), here(t)); break;
                case Tok::Directive: ok = directive(t); break;
                case Tok::End: break;
            }
            if (!ok) return false;
        }
        if (!conds_.empty())
            return fail(
                ErrorCode::Directive,
                "#" + Str(conds_.back().negated ? "ifndef" : "ifdef") + " has no matching #endif",
                conds_.back().line);
        if (frames_.size() > 1) {
            const FrameInfo& f = frames_.back();
            return fail(ErrorCode::Syntax, Str(bracket_name(f.open)) + " is never closed", f.line,
                        f.begin);
        }
        Ref<Array> root = arena_.make_array(scratch_, file_, 1);
        if (root->height() > s_.limits.max_depth + 1) return too_deep(1);
        if (map_) map_->record(root, {file_, 0, size_, {}}, spans_);
        out = std::move(root);
        // Once the file is read: a pack usually #requires what it requires after declaring it.
        if (pack_ >= 0)
            SessionAccess::warn_missing_requires(s_, s_.st_->packs[static_cast<size_t>(pack_)],
                                                 pack_line_);
        return true;
    }

private:
    enum class Frame : uint8_t { Root, Array, Command, Property, MacroDef, DefineBody, Autorun };
    struct FrameInfo {
        Frame kind;
        size_t start;
        int32_t line;
        Symbol name;  // DefineBody
        char open = '(';
        uint32_t begin = 0;    // offset of the opening bracket
        bool removes = false;  // holds #remove, which must be its one value
    };
    struct Cond {
        bool active;         // tokens in the current branch are read
        bool parent_active;  // the enclosing region is read
        bool value;          // the condition's result
        bool seen_else;
        bool negated;
        int32_t line;  // where the current branch began: its #ifdef, or its #else
        Symbol name;   // what it tests; empty inside a branch not read
    };
    // The branch `c` ends at `line`: a section of the read, if it was read.
    void section_ends(const Cond& c, int32_t line) {
        if (c.active && !c.name.empty())
            s_.st_->sections.push_back({c.name, c.negated, c.seen_else, file_, c.line, line});
    }

    static constexpr uint32_t kNoColumn = UINT32_MAX;
    // An error at byte `at`, which gives its column.
    bool fail(ErrorCode code, std::string_view msg, int32_t line, uint32_t at) {
        *err_ = make_error(code, msg, file_, line);
        if (at != kNoColumn) err_->set_column(lex_.column_at(at));
        return false;
    }
    // At `line` alone: an array's line, which keeps no column. Nothing tracks the column
    // per token, so a column comes only from an offset at hand (E4's Acton note).
    bool fail(ErrorCode code, std::string_view msg, int32_t line) {
        return fail(code, msg, line, kNoColumn);
    }
    bool too_deep(int32_t line, uint32_t at = kNoColumn) {
        return fail(ErrorCode::Limit,
                    "nesting deeper than " + detail::decimal(s_.limits.max_depth) + " levels", line,
                    at);
    }
    [[nodiscard]] bool active() const noexcept { return conds_.empty() || conds_.back().active; }

    [[nodiscard]] SourceSpan here(const Token& t) const noexcept {
        return {file_, t.begin, t.end, {}};
    }

    // Edit mode keeps spans_ parallel to scratch_. An included file's nodes were counted by
    // its own parser, so splicing them in does not count them again.
    bool push(Node n, const SourceSpan& span = {}, bool counted = false) {
        if (!counted && ++s_.st_->nodes > s_.limits.max_nodes)
            return fail(ErrorCode::Limit,
                        "more than " + detail::decimal(static_cast<int64_t>(s_.limits.max_nodes)) +
                            " nodes",
                        line_, span.file.empty() ? kNoColumn : span.begin);
        if (frames_.back().kind == Frame::Root && n.type() == Type::Property)
            return fail(ErrorCode::Syntax,
                        "a [property] cannot appear at top level, where '[' defines a macro", line_,
                        span.file.empty() ? kNoColumn : span.begin);
        scratch_.push_back(std::move(n));
        if (map_) spans_.push_back(span);
        return true;
    }

    void truncate(size_t size) {
        scratch_.resize(size);
        if (map_) spans_.resize(size);
    }
    // Records `arr` (made from the nodes from `start`) and its span, for edit mode.
    void record(const Ref<Array>& arr, size_t start, uint32_t begin, uint32_t end) {
        if (!map_) return;
        map_->record(arr, {file_, begin, end, {}},
                     std::span<const SourceSpan>(spans_).subspan(start));
    }

    bool symbol(const Token& t) {
        const Symbol sym(t.text);
        const FrameInfo& f = frames_.back();
        const bool naming_macro = f.kind == Frame::MacroDef && scratch_.size() == f.start;
        if (!t.quoted && !naming_macro && !s_.st_->defines.empty()) {
            if (const Array* body = s_.macro(sym)) {
                if (pack_ >= 0) one_way(sym, t.line);
                const SourceSpan from{file_, t.begin, t.end, sym};
                for (const Node& n : *body)
                    if (!push(n, from)) return false;
                return true;
            }
        }
        return push(Node(sym), here(t));
    }

    bool open(char bracket, int32_t line, uint32_t begin) {
        if (frames_.size() > s_.limits.max_depth) return too_deep(line, begin);
        Frame kind = Frame::Array;
        if (bracket == '{') kind = Frame::Command;
        if (bracket == '[')
            kind = frames_.back().kind == Frame::Root ? Frame::MacroDef : Frame::Property;
        frames_.push_back({kind, scratch_.size(), line, {}, bracket, begin});
        return true;
    }

    static char closer(Frame k) noexcept {
        switch (k) {
            case Frame::Command:
            case Frame::Autorun: return '}';
            case Frame::Property:
            case Frame::MacroDef: return ']';
            default: return ')';
        }
    }

    bool close(const Token& t) {
        const FrameInfo f = frames_.back();
        if (f.kind == Frame::Root)
            return fail(ErrorCode::Syntax, "unexpected " + Str(bracket_name(t.bracket)), t.line,
                        t.begin);
        if (t.bracket != closer(f.kind))
            return fail(ErrorCode::Syntax,
                        Str(bracket_name(f.open)) + " from line " + detail::decimal(f.line) +
                            " is closed by " + Str(bracket_name(t.bracket)),
                        t.line, t.begin);
        frames_.pop_back();
        std::span<Node> kids(scratch_.data() + f.start, scratch_.size() - f.start);
        if (f.removes && kids.size() != 2) return bad_remove(f.line, f.begin);

        if (f.kind == Frame::MacroDef) {
            const auto name = kids.empty() ? std::nullopt : kids[0].try_sym();
            if (!name)
                return fail(ErrorCode::Syntax, "macro definition needs a symbol name", f.line,
                            f.begin);
            Ref<Array> body = arena_.make_array(kids.subspan(1), file_, f.line);
            record(body, f.start + 1, f.begin, t.end);
            truncate(f.start);
            if (body->height() > s_.limits.max_depth) return too_deep(f.line, f.begin);
            s_.define(*name, std::move(body));
            defined(*name, f.line, false);
            return true;
        }

        Ref<Array> arr = arena_.make_array(kids, file_, f.line);
        record(arr, f.start, f.begin, t.end);
        truncate(f.start);
        if (arr->height() > s_.limits.max_depth) return too_deep(f.line, f.begin);
        const SourceSpan span{file_, f.begin, t.end, {}};
        switch (f.kind) {
            case Frame::DefineBody:
                s_.define(f.name, std::move(arr));
                s_.st_->tested.push_back(f.name);
                defined(f.name, f.line, false);
                return true;
            case Frame::Autorun:
                s_.st_->autorun.emplace_back(std::move(arr), Type::Command);
                return true;
            case Frame::Command: return push(Node(std::move(arr), Type::Command), span);
            case Frame::Property: return push(Node(std::move(arr), Type::Property), span);
            default:
                // A file's first form may declare it a pack, which the tree leaves out.
                if (frames_.size() == 1 && f.start == 0 && pack_ < 0 && !arr->empty() &&
                    (*arr)[0].is(sym_pack()))
                    return declare_pack(*arr);
                // So may (origin "source" ...), first or after the pack declaration.
                if (frames_.size() == 1 && f.start == 0 && !origin_ && arr->size() >= 2 &&
                    (*arr)[0].is(sym_origin()) && (*arr)[1].type() == Type::String)
                    return declare_origin(*arr);
                return push(Node(std::move(arr)), span);
        }
    }

    static const Symbol& sym_pack() {
        static const Symbol s("pack");
        return s;
    }
    static const Symbol& sym_origin() {
        static const Symbol s("origin");
        return s;
    }

    // A name written as a symbol or a string: (owners "td-combat" td-encounter).
    static std::optional<Symbol> name_of(const Node& n) {
        if (const auto s = n.try_sym()) return s;
        if (const auto s = n.try_str()) return Symbol(*s);
        return std::nullopt;
    }

    // The names a pack declaration lists under `key`, into `out`; false if one is not a name.
    bool names(const Array& decl, const char* key, Symbol pack, Vec<Symbol>& out) {
        const Array* a = decl.try_find(key);
        if (!a) return true;
        for (const Node& n : a->values()) {
            const auto x = name_of(n);
            if (!x)
                return fail(ErrorCode::Syntax,
                            "pack '" + Str(pack.str()) + "': (" + key + " ...) lists names",
                            a->line());
            out.push_back(*x);
        }
        return true;
    }

    // A version: dot-separated integers, as a string, a symbol, or an int, into `out` as
    // its text. A float is refused: 1.10 would read as 1.1.
    bool version_of(const Node& n, Symbol& out, int32_t line) {
        std::string_view text;
        Str number;
        if (const auto sym = n.try_sym()) {
            text = sym->str();
        } else if (const String* str = n.string()) {
            text = str->view();
        } else if (const auto i = n.try_int(); i && *i >= 0) {
            number = detail::decimal(*i);
            text = number;
        } else {
            return fail(ErrorCode::Syntax, "write a version as a string: (version \"1.10\")", line);
        }
        bool digit = false;
        for (const char c : text) {
            if (c == '.' && digit) {
                digit = false;
            } else if (c >= '0' && c <= '9') {
                digit = true;
            } else {
                digit = false;
                break;
            }
        }
        if (!digit)
            return fail(ErrorCode::Syntax,
                        "'" + Str(text) + "' is not a version: dot-separated integers, as \"1.10\"",
                        line);
        out = Symbol(text);
        return true;
    }

    // (origin "economy.xlsx" (tool sheets-export)): recorded for this file, left out of the
    // tree.
    bool declare_origin(const Array& decl) {
        origin_ = true;
        Symbol tool;
        if (const Array* t = decl.try_find("tool")) {
            const auto x = t->size() == 2 ? name_of((*t)[1]) : std::nullopt;
            if (!x)
                return fail(ErrorCode::Syntax, "write (origin \"source\" (tool name))", t->line());
            tool = *x;
        }
        SessionAccess::add_origin(s_, {file_, Symbol(*decl[1].try_str()), tool});
        return true;
    }

    // (pack name (level L) (requires other ...)): other children are metadata for later.
    bool declare_pack(const Array& decl) {
        LoadSession::State& st = *s_.st_;
        const auto name = decl.size() > 1 ? decl[1].try_sym() : std::nullopt;
        if (!name) return fail(ErrorCode::Syntax, "(pack ...) needs a name", decl.line());
        const Array* level = decl.try_find("level");
        const auto l = level && level->size() == 2 ? (*level)[1].try_sym() : std::nullopt;
        if (!l)
            return fail(ErrorCode::Syntax, "pack '" + Str(name->str()) + "' needs a (level L)",
                        decl.line());
        const auto rank = std::find(st.pack_levels.begin(), st.pack_levels.end(), *l);
        if (rank == st.pack_levels.end()) {
            Str levels;
            for (const Symbol& x : st.pack_levels)
                levels += (levels.empty() ? "" : " ") + Str(x.str());
            return fail(ErrorCode::Directive,
                        "pack '" + Str(name->str()) + "' has level '" + Str(l->str()) +
                            "', which is not one of the session's pack levels (" + levels + ")",
                        decl.line());
        }
        Vec<Symbol> needs, at_least, owners, audience;
        if (const Array* r = decl.try_find("requires"))
            for (const Node& n : r->values()) {
                if (const auto x = n.try_sym()) {
                    needs.push_back(*x);
                    at_least.emplace_back();
                    continue;
                }
                // (name "version"): that pack, at that version or later.
                const Array* v = n.try_array();
                Symbol min;
                if (!v || v->size() != 2 || v->tag().empty())
                    return fail(ErrorCode::Syntax,
                                "pack '" + Str(name->str()) +
                                    "': (requires name (name \"version\") ...) lists packs",
                                r->line());
                if (!version_of((*v)[1], min, v->line())) return false;
                needs.push_back(v->tag());
                at_least.push_back(min);
            }
        Symbol version;
        if (const Array* v = decl.try_find("version"))
            if (v->size() != 2 || !version_of((*v)[1], version, v->line())) return false;
        if (!names(decl, "owners", *name, owners) || !names(decl, "for", *name, audience))
            return false;
        Symbol stage;
        if (const Array* s = decl.try_find("stage")) {
            const auto x = s->size() == 2 ? (*s)[1].try_sym() : std::nullopt;
            if (!x || !(*x == "genesis" || *x == "custom" || *x == "product" || *x == "commodity"))
                return fail(ErrorCode::Directive,
                            "pack '" + Str(name->str()) +
                                "': (stage S) is genesis, custom, product, or commodity",
                            s->line());
            stage = *x;
        }
        pack_ = static_cast<int32_t>(SessionAccess::add_pack(
            s_, Pack{*name, *l, 0, file_, List<Symbol>({needs.data(), needs.size()}),
                     List<Symbol>({at_least.data(), at_least.size()}), version,
                     List<Symbol>({owners.data(), owners.size()}),
                     List<Symbol>({audience.data(), audience.size()}), stage}));
        pack_line_ = decl.line();
        return true;
    }

    // Records where `name` was defined, and which pack, if any, defined it.
    void defined(Symbol name, int32_t line, bool flag) {
        s_.st_->definitions.push_back({name, file_, line, flag});
        if (pack_ >= 0)
            s_.st_->macro_pack[name] = static_cast<uint32_t>(pack_);
        else
            s_.st_->macro_pack.erase(name);
    }

    // The one-way rule: a harder pack may not use a softer pack's macro. Warned
    // once per file and macro.
    void one_way(Symbol macro, int32_t line) {
        LoadSession::State& st = *s_.st_;
        const auto it = st.macro_pack.find(macro);
        if (it == st.macro_pack.end()) return;
        const Pack& mine = st.packs[static_cast<size_t>(pack_)];
        const Pack& theirs = st.packs[it->second];
        if (theirs.rank <= mine.rank) return;
        if (std::find(warned_.begin(), warned_.end(), macro) != warned_.end()) return;
        warned_.push_back(macro);
        st.warnings.push_back(
            make_error(ErrorCode::Refused,
                       "pack '" + Str(mine.name.str()) + "' (level " + Str(mine.level.str()) +
                           ") uses macro '" + Str(macro.str()) + "' from the softer pack '" +
                           Str(theirs.name.str()) + "' (level " + Str(theirs.level.str()) + ")",
                       file_, line));
    }

    // Reads the symbol argument of a directive, unexpanded.
    bool name_arg(std::string_view directive, Token& t) {
        if (!lex_.next(t, *err_)) return false;
        if (t.kind != Tok::Symbol)
            return fail(ErrorCode::Directive, "#" + Str(directive) + " needs a name", t.line,
                        t.begin);
        return true;
    }

    bool path_arg(std::string_view directive, Str& path) {
        Token t;
        if (!lex_.next(t, *err_)) return false;
        if (t.kind != Tok::Symbol && t.kind != Tok::String)
            return fail(ErrorCode::Directive, "#" + Str(directive) + " needs a file path", t.line,
                        t.begin);
        path.assign(t.text);
        return true;
    }

    bool conditional(const Token& t, bool parent_active) {
        const bool negated = t.text == "ifndef";
        bool value = false;
        Symbol tested;
        if (parent_active) {
            Token name;
            if (!name_arg(t.text, name)) return false;
            tested = Symbol(name.text);
            value = s_.is_defined(tested) != negated;
            s_.st_->tested.push_back(tested);
        }
        conds_.push_back(
            {parent_active && value, parent_active, value, false, negated, t.line, tested});
        return true;
    }

    bool else_or_endif(const Token& t) {
        if (conds_.empty())
            return fail(ErrorCode::Directive, "#" + Str(t.text) + " without #ifdef", t.line,
                        t.begin);
        if (t.text == "endif") {
            section_ends(conds_.back(), t.line);
            conds_.pop_back();
            return true;
        }
        Cond& c = conds_.back();
        if (c.seen_else)
            return fail(ErrorCode::Directive, "second #else for one #ifdef", t.line, t.begin);
        section_ends(c, t.line);
        c.seen_else = true;
        c.active = c.parent_active && !c.value;
        c.line = t.line;
        return true;
    }

    // Inside a branch that is not being read, only conditionals matter.
    bool skipped_directive(const Token& t) {
        if (t.text == "ifdef" || t.text == "ifndef") return conditional(t, false);
        if (t.text == "else" || t.text == "endif") return else_or_endif(t);
        return true;
    }

    // `(key #remove)`: the marker is the tagged array's one value.
    bool bad_remove(int32_t line, uint32_t at) {
        return fail(ErrorCode::Directive, "#remove is written as a key's only value: (key #remove)",
                    line, at);
    }
    bool removal(const Token& t) {
        FrameInfo& f = frames_.back();
        if (f.kind != Frame::Array || scratch_.size() != f.start + 1 ||
            !scratch_[f.start].try_sym())
            return bad_remove(t.line, t.begin);
        f.removes = true;
        return push(Node(remove_marker()), here(t));
    }

    bool directive(const Token& t) {
        const std::string_view d = t.text;
        if (d == "remove") return removal(t);
        if (d == "ifdef" || d == "ifndef") return conditional(t, true);
        if (d == "else" || d == "endif") return else_or_endif(t);
        if (d == "include" || d == "include_opt" || d == "merge" || d == "require") {
            Str path;
            if (!path_arg(d, path)) return false;
            Ref<Array> root;
            if (!load(path, d == "include_opt", t.line, t.begin, root, d == "require"))
                return false;
            if (!root) return true;  // optional and missing, or already required
            if (d == "merge") {
                MergeScratch merge_scratch;  // #merge is rare: its own
                merge_into(scratch_, map_ ? &spans_ : nullptr, frames_.back().start, *root, 0,
                           MergePolicy::FillMissing, map_, merge_scratch, &arena_);
                return true;
            }
            for (size_t i = 0; i < root->size(); ++i) {
                const SourceSpan* sp = map_ ? map_->span(*root, i) : nullptr;
                if (!push((*root)[i], sp ? *sp : SourceSpan{}, true)) return false;
            }
            return true;
        }
        if (d == "include_dir") return include_dir(t);
        if (d == "define") {
            Token name;
            if (!name_arg(d, name)) return false;
            const Symbol sym(name.text);
            Token next;
            if (!lex_.peek(next, *err_)) return false;
            if (next.kind == Tok::Open && next.bracket == '(' && next.line == name.line) {
                (void)lex_.next(next, *err_);
                if (!open('(', next.line, next.begin)) return false;
                frames_.back().kind = Frame::DefineBody;
                frames_.back().name = sym;
                return true;
            }
            s_.define(sym, DefineFrom::File);
            s_.st_->flags.back().file = file_;
            s_.st_->flags.back().line = name.line;
            s_.st_->tested.push_back(sym);
            defined(sym, name.line, true);
            return true;
        }
        if (d == "undef") {
            Token name;
            if (!name_arg(d, name)) return false;
            s_.undefine(Symbol(name.text));
            s_.st_->tested.emplace_back(name.text);
            return true;
        }
        if (d == "autorun") {
            Token next;
            if (!lex_.next(next, *err_)) return false;
            if (next.kind != Tok::Open || next.bracket != '{')
                return fail(ErrorCode::Directive, "#autorun needs a {command}", next.line,
                            next.begin);
            if (!open('{', next.line, next.begin)) return false;
            frames_.back().kind = Frame::Autorun;
            return true;
        }
        return fail(ErrorCode::Directive, "unknown directive '#" + Str(d) + "'", t.line, t.begin);
    }

    // Whether one more file may be included here.
    bool may_include(const Str& path, int32_t line, uint32_t at) {
        if (s_.refuse_includes)
            return fail(ErrorCode::Refused,
                        "'" + path + "' is not read: this text may include no files", line, at);
        if (include_depth_ + 1 > s_.limits.max_include_depth)
            return fail(
                ErrorCode::Limit,
                "includes nested deeper than " + detail::decimal(s_.limits.max_include_depth), line,
                at);
        if (s_.st_->includes >= s_.limits.max_includes)
            return fail(ErrorCode::Limit,
                        "more than " + detail::decimal(s_.limits.max_includes) + " included files",
                        line, at);
        return true;
    }

    // Where an included path may be: beside this file, then in each include directory.
    [[nodiscard]] Vec<Str> candidates_of(const Str& path) const {
        Vec<Str> candidates;
        if (is_absolute_path(path)) {
            candidates.push_back(normalize_path(path));
        } else {
            candidates.push_back(normalize_path(join_path(parent_path(file_.str()), path)));
            for (const Str& dir : s_.st_->include_dirs)
                candidates.push_back(normalize_path(join_path(dir, path)));
        }
        return candidates;
    }

    // Reads `path` through the session's reader, recording it for the cache.
    bool read_recorded(const Str& path, Str& text) {
        const bool read = read_with(s_.st_->reader, path, text);
        if (s_.st_->recorded)
            s_.st_->files.push_back({path, read ? static_cast<int64_t>(text.size()) : -1,
                                     read ? content_hash(text) : 0});
        return read;
    }

    // Lists `dir` through the session's lister, recording it for the cache.
    bool list_recorded(const Str& dir, Vec<Str>& entries) {
        const uint64_t listing = list_with(s_.st_->lister, dir, entries);
        if (s_.st_->recorded)
            s_.st_->files.push_back(
                {dir, listing ? kListedDirectory : kUnlistedDirectory, listing});
        return listing != 0;
    }

    // Parses `text`, read from `found` for `path` as written, as an included file.
    bool parse_included(const Str& path, const Str& found, const Str& text, int32_t line,
                        uint32_t at, Ref<Array>& out) {
        for (const Str& open_file : s_.st_->include_stack)
            if (open_file == found)
                return fail(ErrorCode::Directive, "'" + path + "' includes itself", line, at);
        ++s_.st_->includes;
        s_.st_->include_stack.push_back(found);
        Parser sub(s_, arena_, text, Symbol(found), include_depth_ + 1);
        const bool ok = sub.run(out, *err_);
        s_.st_->include_stack.pop_back();
        return ok;
    }

    // Resolves and parses an included or merged file. Leaves `out` null if `optional`
    // and the file does not exist, or if `require` and the session has read it already.
    bool load(const Str& path, bool optional, int32_t line, uint32_t at, Ref<Array>& out,
              bool require = false) {
        if (!may_include(path, line, at)) return false;
        Str text;
        const Str* found = nullptr;
        Vec<Str>& required = s_.st_->required;
        const Vec<Str> candidates = candidates_of(path);
        for (const Str& c : candidates) {
            // A required file is not read again: the first candidate the session has
            // required already ends the search.
            if (require && std::find(required.begin(), required.end(), c) != required.end())
                return true;
            if (read_recorded(c, text)) {
                found = &c;
                break;
            }
        }
        if (!found) {
            if (optional) return true;
            return fail(ErrorCode::Io, "cannot read included file '" + path + "'", line, at);
        }
        if (require) required.push_back(*found);
        return parse_included(path, *found, text, line, at, out);
    }

    // #include_dir path: a child for each .dta file in the directory, named by the file and
    // holding what it holds, and for each subdirectory, named by it and holding its own; in
    // the order of the directory's order.dta, then by name.
    bool include_dir(const Token& t) {
        Str path;
        if (!path_arg(t.text, path)) return false;
        if (s_.refuse_includes)
            return fail(ErrorCode::Refused,
                        "'" + path + "' is not read: this text may include no files", t.line,
                        t.begin);
        Vec<Str> entries;
        for (const Str& c : candidates_of(path))
            if (list_recorded(c, entries))
                return splice_dir(c, entries, t.line, t.begin, nullptr, nullptr);
        return fail(ErrorCode::Io, "cannot list the directory '" + path + "' of #include_dir",
                    t.line, t.begin);
    }

    // The children of the directory `dir`, listed as `entries`: pushed here, or added to
    // `into` (and `into_spans` in edit mode) for a subdirectory's table.
    bool splice_dir(const Str& dir, const Vec<Str>& entries, int32_t line, uint32_t at,
                    Vec<Node>* into, Vec<SourceSpan>* into_spans) {
        struct Child {
            Str name;
            Str path;
            bool folder;
        };
        Vec<Child> children;
        children.reserve(entries.size());
        for (const Str& e : entries) {
            const size_t cut = e.view().find_last_of("/\\");
            const std::string_view leaf =
                cut == std::string_view::npos ? e.view() : e.view().substr(cut + 1);
            if (leaf.empty() || leaf.front() == '.' || leaf == "order.dta") continue;  // hidden
            const bool dta = leaf.size() > 4 && leaf.substr(leaf.size() - 4) == ".dta";
            if (!dta && leaf.find('.') != std::string_view::npos) continue;  // not data
            const std::string_view name = dta ? leaf.substr(0, leaf.size() - 4) : leaf;
            if (!is_bare_symbol(name) || name.find_first_of("./") != std::string_view::npos)
                return fail(ErrorCode::Directive,
                            "'" + e + "' cannot be a record: its name is not a symbol", line, at);
            children.push_back({Str(name), e, !dta});
        }
        std::sort(children.begin(), children.end(),
                  [](const Child& a, const Child& b) { return a.name < b.name; });
        for (size_t i = 1; i < children.size(); ++i)
            if (children[i].name == children[i - 1].name)
                return fail(ErrorCode::Directive,
                            "'" + dir + "' holds two records named '" + children[i].name + "'",
                            line, at);
        Vec<uint32_t> order;
        for (uint32_t i = 0; i < children.size(); ++i) order.push_back(i);
        Str text;
        const Str order_file = normalize_path(join_path(dir, "order.dta"));
        if (read_recorded(order_file, text)) {
            Vec<std::string_view> names;
            for (const Child& c : children) names.push_back(c.name.view());
            auto r = directory_order(text, order_file, {names.data(), names.size()}, order,
                                     s_.st_->warnings);
            if (!r) {
                *err_ = std::move(r).error();
                return false;
            }
        }
        for (const uint32_t i : order) {
            const Child& c = children[i];
            Vec<Node> kids{Node(Symbol(c.name))};
            Vec<SourceSpan> spans;           // edit mode
            if (map_) spans.emplace_back();  // the name, written nowhere
            Ref<Array> record;
            if (c.folder) {
                Vec<Str> inner;
                if (!list_recorded(c.path, inner)) continue;  // a file without an extension
                if (!splice_dir(c.path, inner, line, at, &kids, map_ ? &spans : nullptr))
                    return false;
                record = arena_.make_array(kids, file_, line);
            } else {
                Ref<Array> root;
                if (!may_include(c.path, line, at)) return false;
                if (!read_recorded(c.path, text))
                    return fail(ErrorCode::Io, "cannot read '" + c.path + "'", line, at);
                if (!parse_included(c.path, c.path, text, line, at, root)) return false;
                kids.reserve(1 + root->size());
                for (size_t k = 0; k < root->size(); ++k) {
                    kids.push_back((*root)[k]);
                    if (map_) {
                        const SourceSpan* sp = map_->span(*root, k);
                        spans.push_back(sp ? *sp : SourceSpan{});
                    }
                }
                // The record is its file: provenance and write-back point into it.
                record = arena_.make_array(kids, Symbol(c.path), 1);
                if (map_)
                    map_->record(
                        record, {Symbol(c.path), 0, static_cast<uint32_t>(text.size()), {}}, spans);
            }
            if (record->height() > s_.limits.max_depth) return too_deep(line, at);
            const SourceSpan where{file_, at, at, {}};
            if (into) {
                into->emplace_back(std::move(record));
                if (into_spans) into_spans->push_back(where);
            } else if (!push(Node(std::move(record)), where)) {
                return false;
            }
        }
        return true;
    }

    LoadSession& s_;
    Arena& arena_;  // the whole load, includes and macro bodies too
    Lexer lex_;
    Symbol file_;
    uint32_t include_depth_;
    Error* err_ = nullptr;
    int32_t line_ = 1;
    SourceMap* map_;
    uint32_t size_;
    Vec<Node> scratch_;
    Vec<SourceSpan> spans_;  // parallel to scratch_ in edit mode
    Vec<FrameInfo> frames_;
    Vec<Cond> conds_;
    int32_t pack_ = -1;      // the pack this file declared, as an index; -1 for none
    int32_t pack_line_ = 0;  // where it declared it
    bool origin_ = false;    // this file declared its origin
    Vec<Symbol> warned_;     // macros this file has been warned about
};

}  // namespace detail

// Sessions belong to whoever made them: from the default allocator unless an owner passes
// its own.
LoadSession::LoadSession() : LoadSession(default_allocator()) {}
LoadSession::LoadSession(const Allocator& alloc) : st_(detail::create<State>(alloc, alloc)) {}
void LoadSession::copy_options(const LoadSession& o) noexcept {
    limits = o.limits;
    source_map = o.source_map;
    record_for_cache = o.record_for_cache;
    cipher = o.cipher;
    text_only = o.text_only;
    refuse_includes = o.refuse_includes;
    allocator = o.allocator;
}
LoadSession::LoadSession(const LoadSession& o)
    : st_(detail::create<State>(*o.st_->alloc, *o.st_, *o.st_->alloc)) {
    copy_options(o);
}
LoadSession::LoadSession(LoadSession&& o) noexcept : st_(o.st_) {
    copy_options(o);
    // The moved-from session stays usable, and empty, on its allocator.
    o.st_ = detail::create<State>(*st_->alloc, *st_->alloc);
}
LoadSession& LoadSession::operator=(const LoadSession& o) {
    if (this != &o) {  // keeps this session's allocator, as containers do
        const Allocator& a = *st_->alloc;
        State* st = detail::create<State>(a, *o.st_, a);
        detail::destroy(a, st_);
        st_ = st;
        copy_options(o);
    }
    return *this;
}
LoadSession& LoadSession::operator=(LoadSession&& o) noexcept {
    std::swap(st_, o.st_);
    copy_options(o);
    return *this;
}
LoadSession::~LoadSession() {
    detail::destroy(*st_->alloc, st_);
}

namespace {
void drop_flag(Vec<ActiveDefine>& flags, Symbol name) {
    flags.erase(std::remove_if(flags.begin(), flags.end(),
                               [&](const ActiveDefine& f) { return f.name == name; }),
                flags.end());
}
}  // namespace

void LoadSession::define(Symbol name, DefineFrom from) {
    st_->defines[name] = nullptr;
    drop_flag(st_->flags, name);
    st_->flags.push_back({name, from, {}, 0});
}
void LoadSession::define(Symbol name, Ref<Array> body) {
    if (!body) {
        define(name);
        return;
    }
    st_->defines[name] = std::move(body);
    drop_flag(st_->flags, name);  // a macro now, not a flag
}
void LoadSession::undefine(Symbol name) {
    st_->defines.erase(name);
    drop_flag(st_->flags, name);
}
std::span<const ActiveDefine> LoadSession::active_defines() const noexcept {
    return {st_->flags.data(), st_->flags.size()};
}
std::span<const ConditionalSection> LoadSession::conditional_sections() const noexcept {
    return {st_->sections.data(), st_->sections.size()};
}
std::string_view define_from_name(DefineFrom from) noexcept {
    switch (from) {
        case DefineFrom::Host: return "host";
        case DefineFrom::Project: return "project";
        case DefineFrom::File: return "file";
    }
    return "host";
}
bool LoadSession::is_defined(Symbol name) const {
    return st_->defines.count(name) != 0;
}
const Array* LoadSession::macro(Symbol name) const {
    auto it = st_->defines.find(name);
    return it == st_->defines.end() ? nullptr : it->second.get();
}
void LoadSession::set_reader(Reader reader) {
    st_->disk = !reader;
    st_->reader = reader ? std::move(reader) : Reader(detail::read_disk_file);
}
const LoadSession::Reader& LoadSession::reader() const noexcept {
    return st_->reader;
}
void LoadSession::set_lister(Lister lister) {
    st_->lister = lister ? std::move(lister) : Lister(detail::list_disk_dir);
}
const LoadSession::Lister& LoadSession::lister() const noexcept {
    return st_->lister;
}
void LoadSession::add_include_dir(std::string_view dir) {
    st_->include_dirs.emplace_back(dir);
}
void LoadSession::clear_include_dirs() {
    st_->include_dirs.clear();
}
std::span<const Node> LoadSession::autorun() const noexcept {
    return st_->autorun;
}
void LoadSession::clear_autorun() {
    st_->autorun.clear();
}
void LoadSession::set_pack_levels(std::initializer_list<Symbol> levels) {
    set_pack_levels(std::span<const Symbol>(levels.begin(), levels.size()));
}
void LoadSession::set_pack_levels(std::span<const Symbol> levels) {
    st_->pack_levels.assign(levels.data(), levels.data() + levels.size());
}
std::span<const Pack> LoadSession::packs() const noexcept {
    return {st_->packs.data(), st_->packs.size()};
}
const Pack* LoadSession::pack_of(Symbol file) const noexcept {
    for (const Pack& p : st_->packs)
        if (p.file == file) return &p;
    return nullptr;
}
std::span<const Definition> LoadSession::macro_definitions() const noexcept {
    return {st_->definitions.data(), st_->definitions.size()};
}
std::span<const Origin> LoadSession::origins() const noexcept {
    return {st_->origins.data(), st_->origins.size()};
}
const Origin* LoadSession::origin_of(Symbol file) const noexcept {
    for (const Origin& o : st_->origins)
        if (o.file == file) return &o;
    return nullptr;
}
std::span<const Error> LoadSession::warnings() const noexcept {
    return {st_->warnings.data(), st_->warnings.size()};
}
void LoadSession::clear_warnings() {
    st_->warnings.clear();
}

namespace detail {
bool SessionAccess::reads_disk(const LoadSession& s) noexcept {
    return s.st_->disk;
}
bool SessionAccess::recorded(const LoadSession& s) noexcept {
    return s.st_->recorded;
}
std::span<const CacheFile> SessionAccess::files(const LoadSession& s) noexcept {
    return {s.st_->files.data(), s.st_->files.size()};
}
const SessionAccess::Defines& SessionAccess::defines(const LoadSession& s) noexcept {
    return s.st_->defines;
}
std::span<const Symbol> SessionAccess::tested(const LoadSession& s) noexcept {
    return {s.st_->tested.data(), s.st_->tested.size()};
}
uint64_t SessionAccess::parses(const LoadSession& s) noexcept {
    return s.st_->parses;
}
const SessionAccess::Defines& SessionAccess::defines_before(const LoadSession& s) noexcept {
    return s.st_->defines_before;
}
std::span<const Node> SessionAccess::new_autorun(const LoadSession& s) noexcept {
    const Vec<Node>& a = s.st_->autorun;
    return {a.data() + std::min(s.st_->autorun_from, a.size()), a.data() + a.size()};
}
void SessionAccess::add_autorun(LoadSession& s, Node command) {
    s.st_->autorun.push_back(std::move(command));
}
void SessionAccess::add_warning(LoadSession& s, Error warning) {
    s.st_->warnings.push_back(std::move(warning));
}
namespace {
// Whether version `a` is older than `b`, part by part; a missing part counts as 0.
bool older(std::string_view a, std::string_view b) noexcept {
    while (!a.empty() || !b.empty()) {
        uint64_t x = 0, y = 0;
        for (; !a.empty() && a.front() != '.'; a.remove_prefix(1))
            x = x * 10 + static_cast<uint64_t>(a.front() - '0');
        for (; !b.empty() && b.front() != '.'; b.remove_prefix(1))
            y = y * 10 + static_cast<uint64_t>(b.front() - '0');
        if (x != y) return x < y;
        if (!a.empty()) a.remove_prefix(1);
        if (!b.empty()) b.remove_prefix(1);
    }
    return false;
}
}  // namespace

void SessionAccess::warn_missing_requires(LoadSession& s, const Pack& pack, int32_t line) {
    const auto& packs = s.st_->packs;
    for (size_t i = 0; i < pack.requires_packs.size(); ++i) {
        const Symbol need = pack.requires_packs[i];
        const Symbol min = i < pack.requires_versions.size() ? pack.requires_versions[i] : Symbol();
        const auto it =
            std::find_if(packs.begin(), packs.end(), [&](const Pack& p) { return p.name == need; });
        Str problem;
        if (it == packs.end())
            problem = ", which is not loaded";
        else if (!min.empty() && it->version.empty())
            problem = " \"" + Str(min.str()) + "\" or later, which declares no version";
        else if (!min.empty() && older(it->version.str(), min.str()))
            problem =
                " \"" + Str(min.str()) + "\" or later, which is \"" + Str(it->version.str()) + "\"";
        if (!problem.empty())
            s.st_->warnings.push_back(
                make_error(ErrorCode::NotFound,
                           "pack " + quoted(pack.name) + " requires " + quoted(need) + problem,
                           pack.file, line));
    }
}
std::span<const Str> SessionAccess::required(const LoadSession& s) noexcept {
    return {s.st_->required.data(), s.st_->required.size()};
}
size_t SessionAccess::required_before(const LoadSession& s) noexcept {
    return s.st_->recorded ? s.st_->required_from : s.st_->required.size();
}
std::span<const Symbol> SessionAccess::pack_levels(const LoadSession& s) noexcept {
    return {s.st_->pack_levels.data(), s.st_->pack_levels.size()};
}
std::span<const Pack> SessionAccess::new_packs(const LoadSession& s) noexcept {
    const auto& p = s.st_->packs;
    return {p.data() + std::min(s.st_->packs_from, p.size()), p.data() + p.size()};
}
const Pack* SessionAccess::macro_pack(const LoadSession& s, Symbol macro) noexcept {
    const auto it = s.st_->macro_pack.find(macro);
    return it == s.st_->macro_pack.end() ? nullptr : &s.st_->packs[it->second];
}
size_t SessionAccess::add_pack(LoadSession& s, const Pack& pack) {
    auto& packs = s.st_->packs;
    for (size_t i = 0; i < packs.size(); ++i)
        if (packs[i].file == pack.file) return i;  // read again
    Pack p = pack;
    const auto& levels = s.st_->pack_levels;
    p.rank =
        static_cast<uint32_t>(std::find(levels.begin(), levels.end(), p.level) - levels.begin());
    packs.push_back(std::move(p));
    return packs.size() - 1;
}
std::span<const Definition> SessionAccess::new_definitions(const LoadSession& s) noexcept {
    const auto& d = s.st_->definitions;
    return {d.data() + std::min(s.st_->definitions_from, d.size()), d.data() + d.size()};
}
void SessionAccess::add_definition(LoadSession& s, const Definition& definition) {
    s.st_->definitions.push_back(definition);
}
std::span<const Origin> SessionAccess::new_origins(const LoadSession& s) noexcept {
    const auto& o = s.st_->origins;
    return {o.data() + std::min(s.st_->origins_from, o.size()), o.data() + o.size()};
}
void SessionAccess::add_origin(LoadSession& s, const Origin& origin) {
    for (Origin& o : s.st_->origins)
        if (o.file == origin.file) {
            o = origin;
            return;
        }
    s.st_->origins.push_back(origin);
}
void SessionAccess::set_macro_pack(LoadSession& s, Symbol macro, Symbol file) {
    const auto& packs = s.st_->packs;
    for (size_t i = 0; i < packs.size(); ++i)
        if (packs[i].file == file) s.st_->macro_pack[macro] = static_cast<uint32_t>(i);
}
void SessionAccess::add_required(LoadSession& s, std::string_view path) {
    Vec<Str>& done = s.st_->required;
    if (std::find(done.begin(), done.end(), path) == done.end()) done.emplace_back(path);
}
}  // namespace detail

namespace {

// The bytes of the tree `text` makes, estimated in one fast pass: each bracket opens
// an array, each token outside a comment is a node with its tag-run key, and a string's
// bytes are stored once. Real trees run from half to eight times their text, so a fixed
// multiple wasted most of the first block on one end and doubled into a second on the other.
size_t tree_estimate(std::string_view t) noexcept {
    size_t arrays = 0, tokens = 0, strings = 0, chars = 0;
    bool in_atom = false;
    for (size_t i = 0; i < t.size();) {
        const char c = t[i];
        if (c == ';') {  // to the end of the line
            i = std::min(t.find('\n', i), t.size());
            in_atom = false;
        } else if (c == '"') {
            const size_t end = std::min(t.find('"', i + 1), t.size());
            chars += end - i;
            ++strings;
            ++tokens;
            i = end + 1;
            in_atom = false;
        } else if (c == '(' || c == '{' || c == '[') {
            ++arrays;
            ++tokens;
            ++i;
            in_atom = false;
        } else if (c == ')' || c == '}' || c == ']' || static_cast<unsigned char>(c) <= ' ') {
            ++i;
            in_atom = false;
        } else {
            tokens += in_atom ? 0 : 1;
            in_atom = true;
            ++i;
        }
    }
    // An array: its header and half a word of tag-run padding; a node: 16 bytes and its
    // 4-byte key; a string: its header, its characters, and padding.
    const size_t bytes = arrays * (sizeof(Array) + 4) + tokens * (sizeof(Node) + sizeof(uint32_t)) +
                         strings * 16 + chars;
    return bytes + bytes / 16;  // a margin for macro expansions
}

// The first block of a load's arena: the old four bytes per byte for a small text, where
// a pass would cost more than the slack; else tree_estimate of the first 16 KB, scaled, so
// the pass costs a few microseconds whatever the size.
size_t first_block(std::string_view text) noexcept {
    constexpr size_t kSample = 16384;
    if (text.size() <= kSample) return text.size() * 4;
    return static_cast<size_t>(static_cast<double>(tree_estimate(text.substr(0, kSample))) *
                               static_cast<double>(text.size()) / kSample);
}

}  // namespace

Expected<Ref<Array>> read_string(std::string_view text, std::string_view name,
                                 LoadSession& session) {
    const bool top = session.st_->include_stack.empty();
    if (top) {
        LoadSession::State& st = *session.st_;
        st.nodes = 0;
        st.includes = 0;
        st.recorded = session.record_for_cache;
        st.files.clear();
        st.defines_before.clear();
        if (st.recorded) {
            st.defines_before = st.defines;
            st.autorun_from = st.autorun.size();
            st.required_from = st.required.size();
            st.packs_from = st.packs.size();
            st.origins_from = st.origins.size();
            st.definitions_from = st.definitions.size();
        }
    }
    const Str file = detail::normalize_path(name);
    session.st_->include_stack.push_back(file);
    struct Pop {
        Vec<Str>& stack;
        ~Pop() { stack.pop_back(); }
    } pop{session.st_->include_stack};

    // One arena per top-level read; a nested read (an include) has its own parser, not this.
    // Its first block is sized from the text.
    const detail::Arena::Handle arena(detail::Arena::create(
        session.allocator ? *session.allocator : default_allocator(), first_block(text)));
    detail::Parser parser(session, *arena, text, Symbol(file), 0);
    Ref<Array> root;
    Error err;
    if (!parser.run(root, err)) return Unexpected(std::move(err));
    return root;
}

Expected<Ref<Array>> read_string(std::string_view text, std::string_view name) {
    LoadSession session;
    return read_string(text, name, session);
}

Expected<Ref<Array>> read_file(std::string_view path, LoadSession& session) {
    const Str file = detail::normalize_path(path);
    Str text;
    if (!detail::read_with(session.st_->reader, file, text))
        return Unexpected(make_error(ErrorCode::Io, "cannot read '" + file + "'"));
    auto root = read_string(text, file, session);
    if (session.st_->recorded && session.st_->include_stack.empty())
        session.st_->files.insert(  // first, so the record names the main file first
            session.st_->files.begin(),
            detail::CacheFile{file, static_cast<int64_t>(text.size()), detail::content_hash(text)});
    return root;
}

Expected<Ref<Array>> read_file(std::string_view path) {
    LoadSession session;
    return read_file(path, session);
}

}  // namespace baker
