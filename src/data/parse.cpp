#include <baker/parse.hpp>

#include "arena.hpp"
#include "common/containers.hpp"
#include "intern_site.hpp"
#include "lexer.hpp"
#include "markers.hpp"
#include "merge.hpp"
#include "session.hpp"
#include "text.hpp"

#include <baker/path.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>

#if defined(_MSC_VER)
#define BAKER_COLD __declspec(noinline)
#else
#define BAKER_COLD [[gnu::noinline, gnu::cold]]
#endif

namespace baker {
namespace detail {
namespace {

// The nodes of `body`, every level counted and a shared array at each place it appears; stops
// once the count reaches `cap`, so a body of shared arrays costs at most `cap` steps.
uint64_t count_nodes(const Array& body, uint64_t cap) noexcept {
    uint64_t n = body.size();
    for (const Node& k : body) {
        if (n >= cap) return n;
        if (const Array* a = k.array()) n += count_nodes(*a, cap - n);
    }
    return n;
}

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
    const HostCallScope host;
    return reader(path, detail::str_out(text));
}

uint64_t list_with(const LoadSession::Lister& lister, std::string_view dir, Vec<Str>& entries,
                   const Allocator& alloc) {
    entries.clear();
    {
        const HostCallScope host;
        if (!lister(dir, [&](std::string_view p) { entries.emplace_back(p, alloc); })) return 0;
    }
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
          shapes(a),
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
        stamper = o.stamper;
        include_dirs = o.include_dirs;
        autorun = o.autorun;
        required = o.required;
        pack_levels = o.pack_levels;
        packs = o.packs;
        origins = o.origins;
        shapes = o.shapes;
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
        shapes_from = o.shapes_from;
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
    Stamper stamper;  // none: a reload's scan reads every file
    Vec<Str> include_dirs;
    Vec<Node> autorun;
    Vec<Str> required;                 // the files #require has read, normalized
    Vec<Symbol> pack_levels;           // hardest first
    Vec<Pack> packs;                   // in the order they were read
    Vec<Origin> origins;               // files that declared one, in the order read
    Vec<ShapeDecl> shapes;             // #shape declarations, in the order read
    Vec<Definition> definitions;       // macro definitions, in the order read
    Vec<ActiveDefine> flags;           // the flags that stand, in the order made
    Vec<ConditionalSection> sections;  // the #ifdef branches the reads took
    Vec<SkippedShape> skipped_shapes;  // #shape directives inside branches not taken
    Vec<Symbol> tested;   // each name an #ifdef, #ifndef, #define or #undef named, as read
    uint64_t parses = 0;  // the files parsed: a cache's replay parses none
    HashMap<Symbol, uint32_t> macro_pack;  // the pack that defined each macro, by index
    // Each macro body's nodes, every level counted and shared arrays at each use, for
    // ParseLimits::max_macro_nodes; a body with arrays only, and only once it is used.
    HashMap<Symbol, uint64_t> macro_nodes;
    Vec<Error> warnings;
    // What the last top-level read did, if recorded (record_for_cache).
    bool recorded = false;
    Vec<detail::CacheFile> files;
    HashMap<Symbol, Ref<Array>> defines_before;
    size_t autorun_from = 0;
    size_t required_from = 0;
    size_t packs_from = 0;
    size_t origins_from = 0;
    size_t shapes_from = 0;
    size_t definitions_from = 0;
    // Per top-level read.
    Vec<Str> include_stack;
    uint64_t nodes = 0;
    bool had_rows = false;  // a file of this read held rows: compact its tree
    uint32_t aliases = 0;   // arrays with an (@aka ...) or an (@id ...): the merge resolves by
                            // them
    detail::NewSymbols new_symbols;  // counted by the symbol table
    uint32_t includes = 0;
    uint64_t from_macros = 0;  // nodes taken from macros, against max_macro_nodes
    uint64_t shared = 0;       // of those, the nodes inside arrays a use shared, not copied
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
        defs_from_ = s_.st_->definitions.size();
        // A name this file brings that shares a hash with another is reported at its line.
        if constexpr (!kShipping) outer_site_ = set_intern_site({file, &line_});
    }
    ~Parser() {
        if constexpr (!kShipping) (void)set_intern_site(outer_site_);
    }
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

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
        if (s_.st_->new_symbols.count > s_.limits.max_new_symbols)  // a directive's last name
            return too_many_names(line_, kNoColumn);
        if (!rows_.empty()) {
            Str path;
            if (!fold(scratch_, map_ ? &spans_ : nullptr, 0, path, 0)) return false;
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
    // Declare: the one form of #pack, #origin or #shape. The braces last, so the
    // infix rule tests for one with one comparison.
    enum class Frame : uint8_t {
        Root,
        Array,
        Property,
        MacroDef,
        DefineBody,
        Declare,
        Command,
        Autorun
    };
    struct FrameInfo {
        Frame kind;
        size_t start;
        int32_t line;
        Symbol name;  // DefineBody; Declare: the directive's name
        char open = '(';
        uint32_t begin = 0;  // offset of the opening bracket
        bool verb = false;   // holds a layer verb: its form is checked at close
        bool row = false;    // headed by a row's key: close() wraps it in the key's segments
        // An odd slot was written as something other than a bare symbol token: a quoted symbol
        // (`{+ '-' 1}`) or a macro use, so the brace is never read infix.
        bool no_infix = false;
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
    // Past ParseLimits::max_new_symbols, the symbol table spends the node budget
    // (count_new_symbols), so push() finds it at no cost of its own.
    bool too_many_names(int32_t line, uint32_t at) {
        return fail(ErrorCode::Limit,
                    "more than " +
                        detail::decimal(static_cast<int64_t>(s_.limits.max_new_symbols)) +
                        " new names (ParseLimits::max_new_symbols)",
                    line, at);
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
        if (!counted && ++s_.st_->nodes > s_.limits.max_nodes) {
            if (s_.st_->new_symbols.count > s_.limits.max_new_symbols)
                return too_many_names(line_, span.file.empty() ? kNoColumn : span.begin);
            return fail(ErrorCode::Limit,
                        "more than " + detail::decimal(static_cast<int64_t>(s_.limits.max_nodes)) +
                            " nodes",
                        line_, span.file.empty() ? kNoColumn : span.begin);
        }
        if (frames_.back().kind == Frame::Root && n.type() == Type::Property)
            return fail(ErrorCode::Syntax,
                        "a [property] cannot appear at top level, where '[' defines a macro", line_,
                        span.file.empty() ? kNoColumn : span.begin);
        scratch_.push_back(std::move(n));
        if (map_) spans_.push_back(span);
        return true;
    }

    // Shrinks only. pop_back, not resize: once allocation took a kind, GCC 13 called resize's
    // erase out of line here, and parse took about 5% longer.
    void truncate(size_t size) {
        while (scratch_.size() > size) scratch_.pop_back();
        if (map_) spans_.resize(size);
    }
    // Records `arr` (made from the nodes from `start`) and its span, for edit mode.
    void record(const Ref<Array>& arr, size_t start, uint32_t begin, uint32_t end) {
        if (!map_) return;
        map_->record(arr, {file_, begin, end, {}},
                     std::span<const SourceSpan>(spans_).subspan(start));
    }

    bool symbol(const Token& t) {
        // A row's key, (/a.b.c ...), heads a parenthesized array: a byte compare for every
        // other symbol.
        if (t.text.size() > 1 && t.text[0] == '/' && !t.quoted &&
            frames_.back().kind == Frame::Array && scratch_.size() == frames_.back().start)
            return row_key(t);
        const Symbol sym(t.text);
        const FrameInfo& f = frames_.back();
        const bool naming_macro = f.kind == Frame::MacroDef && scratch_.size() == f.start;
        if (!t.quoted && !naming_macro && !s_.st_->defines.empty()) {
            if (const Array* body = s_.macro(sym)) {
                if (!take_macro(sym, *body, t)) return false;
                if (pack_ >= 0)
                    one_way(sym, t.line);
                else if (!s_.st_->macro_pack.empty())  // checked if a #pack follows
                    unpacked_.emplace_back(sym, t.line);
                if (body->size() != 1 || odd_slot()) frames_.back().no_infix = true;
                const SourceSpan from{file_, t.begin, t.end, sym};
                for (const Node& n : *body)
                    if (!push(n, from)) return false;
                return true;
            }
        }
        if (t.quoted && odd_slot()) frames_.back().no_infix = true;
        return push(Node(sym), here(t));
    }
    // Counts a use of macro `name` against ParseLimits::max_macro_nodes, before it is spliced.
    bool take_macro(Symbol name, const Array& body, const Token& t) {
        LoadSession::State& st = *s_.st_;
        uint64_t n = body.size();
        if (body.height() > 1) {
            const auto it = st.macro_nodes.find(name);
            if (it != st.macro_nodes.end()) {
                n = it->second;
            } else {  // a host's macro, or one from a cache: counted once, up to the limit
                const uint64_t limit = s_.limits.max_macro_nodes;
                n = detail::count_nodes(body, limit == UINT64_MAX ? limit : limit + 1);
                st.macro_nodes[name] = n;
            }
            st.shared += n - body.size();
        }
        if (n <= s_.limits.max_macro_nodes - st.from_macros) {  // never more than the limit
            st.from_macros += n;
            return true;
        }
        return fail(ErrorCode::Limit,
                    "macro '" + Str(name.str()) + "' takes the read past " +
                        detail::decimal(static_cast<int64_t>(s_.limits.max_macro_nodes)) +
                        " nodes from macros (ParseLimits::max_macro_nodes)",
                    t.line, t.begin);
    }
    [[nodiscard]] bool odd_slot() const noexcept {
        return ((scratch_.size() - frames_.back().start) & 1) != 0;
    }

    bool open(char bracket, int32_t line, uint32_t begin) {
        if (frames_.size() > s_.limits.max_depth) return too_deep(line, begin);
        Frame kind = Frame::Array;
        if (bracket == '{') kind = Frame::Command;
        if (bracket == '[')
            kind = frames_.back().kind == Frame::Root ? Frame::MacroDef : Frame::Property;
        if (kind == Frame::MacroDef) def_mark_ = s_.st_->nodes + s_.st_->shared;
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

        if (f.kind == Frame::MacroDef) {
            const auto name = kids.empty() ? std::nullopt : kids[0].try_sym();
            if (!name)
                return fail(ErrorCode::Syntax, "macro definition needs a symbol name", f.line,
                            f.begin);
            Ref<Array> body = arena_.make_array(kids.subspan(1), file_, f.line);
            record(body, f.start + 1, f.begin, t.end);
            truncate(f.start);
            if (body->height() > s_.limits.max_depth) return too_deep(f.line, f.begin);
            // Every node read inside the brackets, less the name, and what shared arrays hold.
            const uint64_t nodes = s_.st_->nodes + s_.st_->shared - def_mark_ - 1;
            const bool arrays = body->height() > 1;
            s_.define(*name, std::move(body));
            if (arrays) s_.st_->macro_nodes[*name] = nodes;
            defined(*name, f.line, false);
            return true;
        }

        // The infix rule, after a test of slot 1 that a call (`{game add_score 10}`) fails; an
        // even brace only when its head is an operand, which only malformed infix gives.
        bool infix = false;
        if (f.kind >= Frame::Command && kids.size() >= 2 &&
            ((kids.size() & 1) || is_infix_operand(kids[0])) && !f.no_infix &&
            infix_head(kids[1])) {
            const int r = infix_rule(kids.data(), kids.size(), f.start, f.line, f.begin);
            if (r < 0) return false;
            infix = r > 0;
            if (infix) kids = kids.first(static_cast<size_t>(r));
        }
        Ref<Array> arr = arena_.make_array(kids, file_, f.line, f.verb);
        if (infix) arr->set_written_infix(true);
        record(arr, f.start, f.begin, t.end);
        truncate(f.start);
        if (arr->height() > s_.limits.max_depth) return too_deep(f.line, f.begin);
        // Metadata comes after the value. Inline, so an array without metadata pays one test;
        // a command's and a property's forms are positional and keep their order.
        if (arr->meta_first() && f.kind == Frame::Array && !metadata_placed(*arr, f)) return false;
        if (arr->meta_first() && (arr->meta(detail::aka_tag()) || arr->meta(detail::id_tag())))
            ++s_.st_->aliases;
        if (f.verb && !verb_placed(*arr, f)) return false;
        if (f.row) return wrap_row(std::move(arr), f, t.end);
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
            case Frame::Declare: return declared(f, std::move(arr));
            default: return push(Node(std::move(arr)), span);
        }
    }

    // The infix rule. A brace of 2k+1 nodes whose odd slots are one bare operator,
    // `{0.3 <= $value <= 0.7}`, is the command `{<= 0.3 $value 0.7}`: the nodes, and their
    // spans, are permuted in place and `kids` shrinks to the command. Only when an operand (an
    // even slot) is a number, variable, property or command, the guard, so a brace of bare
    // symbols means what it did. An operator at slot 1 that is not one is an error at the
    // brace, and so is one after an operand head in an even brace (`{$v <= 0.7 0.3}`,
    // `{$v <=}`), which no head-first call writes: quoted, `{$obj '<=' 1 2}`, it is a call.
    static bool infix_head(const Node& n) noexcept {
        const auto s = n.try_sym();
        return s && !s->str().empty() && kOperatorByte[static_cast<unsigned char>(s->str()[0])];
    }
    // Returns the command's new size, 0 for a command as written, -1 after an error. Out of
    // line, its arguments by value: the hot loop's close keeps its registers.
    BAKER_COLD int infix_rule(Node* kids, size_t n, size_t start, int32_t line, uint32_t begin) {
        const std::string_view op = kids[1].try_sym()->str();
        if (!is_operator_symbol(op)) return 0;
        bool operand = false;
        for (size_t i = 0; i < n; i += 2) operand = operand || is_infix_operand(kids[i]);
        if (!operand) return 0;
        const Symbol head = *kids[1].try_sym();
        for (size_t i = 3; i < n; i += 2) {
            if (kids[i].is(head)) continue;
            if (quoted_in_command(kids[i]))
                return malformed(line, begin,
                                 "mixed operators in {...}: nest braces to say which applies "
                                 "first");
            return malformed(line, begin,
                             "infix {...} needs '" + Str(op) + "' between every two operands");
        }
        if (!(n & 1))
            return malformed(line, begin,
                             "infix {...} ends with '" + Str(op) + "': write an operand after it");
        // [k0 op k2 op k4 ...] -> [op k0 k2 k4 ...]: each operand moves left over the operators
        // before it, which end up past the command and are dropped.
        auto swap = [&](size_t a, size_t b) {
            kids[a].swap(kids[b]);
            if (map_) std::swap(spans_[start + a], spans_[start + b]);
        };
        swap(0, 1);
        for (size_t i = 3, j = 4; j < n; ++i, j += 2) swap(i, j);
        const size_t size = (n + 3) / 2;
        if (map_) spans_.resize(start + size);  // the operators' spans go with them
        return static_cast<int>(size);
    }
    int malformed(int32_t line, uint32_t begin, std::string_view message) {
        fail(ErrorCode::Syntax, message, line, begin);
        return -1;
    }

    // The key after '/': segments separated by '.' or '/', a 'quoted' one for a name holding
    // either, as a Path is written; each a key, never an element. The last is the
    // array's tag; close() wraps the array in the others.
    BAKER_COLD bool row_key(const Token& t) {
        auto bad = [&](const Str& why) { return fail(ErrorCode::Syntax, why, t.line, t.begin); };
        if (frames_.size() != 2)
            return bad("a row is a top-level form; write the key nested, or quote '" + Str(t.text) +
                       "' for a symbol");
        const std::string_view key = t.text.substr(1);
        row_size_ = 0;
        for (size_t i = 0, from = 0; i < key.size(); from = i) {
            if (row_size_ == Path::kMaxSegments) return bad("the key has more than 16 segments");
            std::string_view name;
            if (key[i] == '\'') {
                i = key.find('\'', i + 1);
                if (i == std::string_view::npos)
                    return bad(
                        "the key has an unclosed quote; a key holds no space or bracket: write "
                        "that key nested");
                name = key.substr(from + 1, i++ - from - 1);
            } else {
                i = std::min(key.find_first_of("./", i), key.size());
                name = key.substr(from, i - from);
                if (!name.empty() && name.find_first_not_of("0123456789") == Str::npos)
                    return bad("a row names keys, and '" + Str(name) +
                               "' is an element; an element is set with config.set or by "
                               "writing the whole list");
                if (!name.empty() && !is_bare_symbol(name))
                    return bad("the key's segment " + Str(name) + " must be quoted: '" + Str(name) +
                               "'");
            }
            if (name.empty()) return bad("the key has an empty segment");
            const auto at = static_cast<uint32_t>(t.begin + 1);
            row_[row_size_++] = {Symbol(name), at + static_cast<uint32_t>(from),
                                 at + static_cast<uint32_t>(i)};
            if (i == key.size()) break;
            if (key[i] != '.' && key[i] != '/')
                return bad("a quoted segment is followed by '.' or '/'");
            if (++i == key.size()) return bad("the key ends with a separator");
        }
        frames_.back().row = true;
        rows_.push_back({t.line, t.begin});
        s_.st_->had_rows = true;
        return push(Node(row_[row_size_ - 1].name),
                    {file_, row_[row_size_ - 1].begin, row_[row_size_ - 1].end, {}});
    }

    // `arr` in one array for each segment before its tag, each made in the load's arena with
    // the row's line. In edit mode each wrapper's extent is the row's, and the map
    // records it as row-made: written as a segment of the key, not literally.
    BAKER_COLD bool wrap_row(Ref<Array> arr, const FrameInfo& f, uint32_t end) {
        const SourceSpan extent{file_, f.begin, end, {}};
        for (size_t i = row_size_ - 1; i-- > 0;) {
            Node pair[2] = {Node(row_[i].name), Node(std::move(arr))};
            arr = arena_.make_array(pair, file_, f.line);
            const SourceSpan spans[2] = {{file_, row_[i].begin, row_[i].end, {}}, extent};
            if (map_) map_->record(arr, extent, spans, true);
        }
        s_.st_->nodes += 2 * (row_size_ - 1);  // push() counts them against the limit
        if (arr->height() > s_.limits.max_depth) return too_deep(f.line, f.begin);
        return push(Node(std::move(arr)), extent);
    }

    // In a file with rows, each tag heads one array at each level: the arrays sharing a tag
    // in nodes[from..] are joined, in the order written, at the first one's place, and the
    // joined children folded in turn. A key written twice, by two rows or by a row and a
    // literal, is an error at the second: first-wins holds only in files without rows.
    // Grouped by sorting, in buffers kept a depth (as MergeScratch's): no allocation
    // once they are warm.
    BAKER_COLD bool fold(Vec<Node>& nodes, Vec<SourceSpan>* spans, size_t from, Str& path,
                         size_t depth) {
        if (fold_.empty()) fold_.reserve(s_.limits.max_depth + 2);  // a level never moves
        while (fold_.size() <= depth) fold_.emplace_back(alloc_);
        FoldLevel& l = fold_[depth];
        l.order.clear();
        // A key's #append and #drop rows stay apart, in the order written: the merge applies
        // each in turn, as it does a nested file's.
        for (size_t i = from; i < nodes.size(); ++i)
            if (!tag_of(nodes[i]).empty() && !detail::composes(*nodes[i].try_array()))
                l.order.push_back(static_cast<uint32_t>(i));
        std::sort(l.order.begin(), l.order.end(), [&](uint32_t x, uint32_t y) {
            const void *a = tag_of(nodes[x]).id(), *b = tag_of(nodes[y]).id();
            return a != b ? std::less<>()(a, b) : x < y;
        });
        l.next.assign(nodes.size(), kNoNext);  // each array's next with its tag; kLater: not first
        bool joins = false;
        for (size_t k = 1; k < l.order.size(); ++k) {
            const uint32_t x = l.order[k - 1], y = l.order[k];
            if (tag_of(nodes[x]) != tag_of(nodes[y])) continue;
            l.next[x] = (l.next[x] & kLater) | y;
            l.next[y] |= kLater;
            joins = true;
        }
        size_t out = from;
        for (size_t i = from; i < nodes.size() && joins; ++i) {
            if (l.next[i] & kLater) continue;  // joined into the first with its tag
            if (l.next[i] != kNoNext && !join(nodes, spans, i, path, depth)) return false;
            nodes[out] = std::move(nodes[i]);
            if (spans) (*spans)[out] = (*spans)[i];
            ++out;
        }
        // pop_back, not resize: a second caller of vector<Node>'s erase made GCC stop inlining
        // it into truncate(), every close() (+1.4% parse instructions, measured).
        while (joins && nodes.size() > out) nodes.pop_back();
        if (spans) spans->resize(nodes.size());
        return true;
    }

    // nodes[h] and the arrays after it with its tag, as one array: the one value run first,
    // whichever member wrote it, then every tagged child in the order written, so a value
    // never follows metadata.
    bool join(Vec<Node>& nodes, Vec<SourceSpan>* spans, size_t h, Str& path, size_t depth) {
        FoldLevel& l = fold_[depth];
        const Array& a = *nodes[h].try_array();
        const size_t mark = path.size();
        if (mark) path += '/';
        append_path_name(path, a.tag().str());
        // The member with values, if any; a second one, or a removal joined with anything,
        // is written twice.
        uint32_t valued = has_values(a) ? static_cast<uint32_t>(h) : kNoNext;
        bool removed = detail::is_removal(a);
        for (uint32_t m = l.next[h]; m != kNoNext; m = l.next[m] & ~kLater) {
            const Array& b = *nodes[m].try_array();
            if ((valued != kNoNext && has_values(b)) || removed || detail::is_removal(b))
                return written_twice(b, path, valued != kNoNext && has_values(b));
            if (has_values(b)) valued = m;
        }
        l.kids.assign(1, a[0]);
        l.spans.clear();
        auto take = [&](const Array& x, bool values) {
            const auto known = spans ? map_->spans(x) : std::nullopt;
            for (size_t k = 1; k < x.size(); ++k) {
                if (tag_of(x[k]).empty() != values) continue;
                l.kids.push_back(x[k]);
                if (spans) l.spans.push_back(known ? (*known)[k] : SourceSpan{});
            }
        };
        if (spans) {
            const auto known = map_->spans(a);
            l.spans.push_back(known ? (*known)[0] : SourceSpan{});
        }
        if (valued != kNoNext) take(*nodes[valued].try_array(), true);
        take(a, false);
        for (uint32_t m = l.next[h]; m != kNoNext; m = l.next[m] & ~kLater)
            take(*nodes[m].try_array(), false);
        if (!fold(l.kids, spans ? &l.spans : nullptr, 1, path, depth + 1)) return false;
        path.resize(mark);
        Ref<Array> joined = arena_.make_array(l.kids, a.file(), a.line());
        while (!l.kids.empty()) l.kids.pop_back();  // moved from; see fold()
        const SourceSpan* ext = map_ ? map_->extent(a) : nullptr;
        if (map_) map_->record(joined, ext ? *ext : SourceSpan{}, l.spans, true);
        nodes[h] = Node(std::move(joined));
        return true;
    }

    static bool has_values(const Array& a) noexcept {
        for (size_t i = 1; i < a.size(); ++i)
            if (tag_of(a[i]).empty()) return true;
        return false;
    }

    // At the later writing, `b`, and at its row's key when a row wrote it. A key written
    // twice by literals only is the list a file of rows cannot hold: the error says so.
    bool written_twice(const Array& b, const Str& path, bool twice) {
        const auto r = std::find_if(rows_.begin(), rows_.end(),
                                    [&](const RowAt& x) { return x.line == b.line(); });
        Str why = "'" + path + (twice ? "' is written twice" : "' is removed and also written");
        if (r == rows_.end() && b.file() == file_)
            why += "; this file holds rows (the first at line " + detail::decimal(rows_[0].line) +
                   "), so each key is written once: a list of records is written without rows";
        *err_ = make_error(ErrorCode::Syntax, why, b.file(), b.line());
        if (b.file() == file_ && r != rows_.end()) err_->set_column(lex_.column_at(r->key));
        return false;
    }

    // A declaration's form, which the tree leaves out: #pack (NAME ...), #origin ("SRC" ...),
    // #shape (NAME ...), the name or source first.
    bool declared(const FrameInfo& f, Ref<Array> form) {
        if (f.name == "pack") return declare_pack(*form);
        if (f.name == "shape") {
            if (form->tag().empty() || form->tag().is_meta())
                return fail(ErrorCode::Directive,
                            "#shape takes a named form: #shape (NAME (FIELD TYPE ...) ...)", f.line,
                            f.begin);
            SessionAccess::add_shape(
                s_, {std::move(form), file_, f.line, Symbol(s_.st_->include_stack.front())});
            return true;
        }
        if (form->empty() || (*form)[0].type() != Type::String)
            return fail(ErrorCode::Directive,
                        "#origin takes a string first: #origin (\"source\" (tool NAME))", f.line,
                        f.begin);
        return declare_origin(*form);
    }

    // #pack, #origin, #shape: one form at the file's top level; a pack and an origin once.
    bool declaration(const Token& t) {
        const Str d(t.text);
        if (frames_.size() != 1)
            return fail(ErrorCode::Directive, "#" + d + " is written at a file's top level", t.line,
                        t.begin);
        if ((d == "pack" && pack_ >= 0) || (d == "origin" && origin_))
            return fail(ErrorCode::Directive,
                        "second #" + d + " in this file: a file declares one " + d, t.line,
                        t.begin);
        Token next;
        if (!lex_.next(next, *err_)) return false;
        if (next.kind != Tok::Open || next.bracket != '(')
            return fail(ErrorCode::Directive,
                        "#" + d + " takes one form: #" + d +
                            (d == "pack"     ? " (NAME (level L) ...)"
                             : d == "origin" ? " (\"source\" (tool NAME))"
                                             : " (NAME (FIELD TYPE ...) ...)"),
                        next.line, next.begin);
        if (!open('(', next.line, next.begin)) return false;
        frames_.back().kind = Frame::Declare;
        frames_.back().name = Symbol(d);
        return true;
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

    // #origin ("economy.xlsx" (tool sheets-export)): recorded for this file.
    bool declare_origin(const Array& decl) {
        origin_ = true;
        Symbol tool;
        if (const Array* t = decl.try_find("tool")) {
            const auto x = t->size() == 2 ? name_of((*t)[1]) : std::nullopt;
            if (!x)
                return fail(ErrorCode::Syntax, "write #origin (\"source\" (tool name))", t->line());
            tool = *x;
        }
        SessionAccess::add_origin(s_, {file_, Symbol(*decl[0].try_str()), tool});
        return true;
    }

    // #pack (name (level L) (requires other ...)): other children are metadata for later.
    bool declare_pack(const Array& decl) {
        LoadSession::State& st = *s_.st_;
        const auto name = decl.tag().empty() ? std::nullopt : std::optional<Symbol>(decl.tag());
        if (!name) return fail(ErrorCode::Syntax, "#pack (...) needs a name", decl.line());
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
        // A pack is its whole file's: what the file defined above #pack is the pack's, and the
        // macros it used above #pack are checked now.
        for (size_t i = defs_from_; i < st.definitions.size(); ++i)
            if (st.definitions[i].file == file_)
                st.macro_pack[st.definitions[i].name] = static_cast<uint32_t>(pack_);
        for (const auto& [macro, line] : unpacked_) one_way(macro, line);
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
        if (t.text == "shape")  // for shape::contract_set: a view's shape set is one per build
            s_.st_->skipped_shapes.push_back(
                {file_, t.line, Symbol(s_.st_->include_stack.front())});
        return true;
    }

    // In the parenthesized array `a` just closed, every child after the first metadata
    // child, (@tag ...), is a tagged array: a field or more metadata, never a value. Located
    // at the array: a child's own place is not known once macros have spliced theirs in.
    BAKER_COLD bool metadata_placed(const Array& a, const FrameInfo& f) {
        const uint32_t m = a.meta_first();
        for (uint32_t i = m + 1; i < a.size(); ++i) {
            if (!detail::tag_of(a[i]).empty()) continue;
            return fail(
                ErrorCode::Syntax,
                "metadata comes after the value: move " + preview(a[m]) + " after " + preview(a[i]),
                f.line, f.begin);
        }
        return true;
    }

    // Whether the verb in `a`'s slot has the form it is written in; else the located error.
    BAKER_COLD bool verb_placed(const Array& a, const FrameInfo& f) {
        if (detail::verb_of(a) != detail::Marker::None) return true;
        return fail(ErrorCode::Directive, detail::marker_hint(detail::marker_of(a[1])), f.line,
                    f.begin);
    }

    // A layer verb, `#remove`, `#append` or `#drop`: the first value of a parenthesized
    // array tagged with a key, metadata's included. The rest of its form (how many
    // values follow) is checked when the array closes.
    bool verb(const Token& t, detail::Marker m) {
        FrameInfo& f = frames_.back();
        const auto tag =
            scratch_.size() == f.start + 1 ? scratch_[f.start].try_sym() : std::nullopt;
        if (f.kind != Frame::Array || !tag || tag->empty())
            return fail(ErrorCode::Directive, detail::marker_hint(m), t.line, t.begin);
        f.verb = true;
        return push(Node(detail::marker_symbol(m)), here(t));
    }

    bool directive(const Token& t) {
        const std::string_view d = t.text;
        if (const detail::Marker m = detail::marker_named(d); m != detail::Marker::None)
            return verb(t, m);
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
                // The older macro form, still read; [NAME ...] is the one a check suggests.
                s_.st_->warnings.push_back(make_error(
                    ErrorCode::Syntax,
                    "#define " + Str(name.text) + " (...) is the older macro form; write [" +
                        Str(name.text) + " ...]",
                    file_, name.line));
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
        if (d == "pack" || d == "origin" || d == "shape") return declaration(t);
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
        // A path the session records is copied there, with its allocator: the default's,
        // which outlives the session, not this parser's.
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
        Str text(alloc_);
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
        Str text(alloc_);
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
    // The session's allocator, tagged Config, for the parser's own buffers: before the
    // containers that take it.
    const Allocator alloc_{with_kind(*s_.st_->alloc, MemoryKind::Config)};
    Vec<Node> scratch_{alloc_};
    Vec<SourceSpan> spans_{alloc_};  // parallel to scratch_ in edit mode
    Vec<FrameInfo> frames_{alloc_};
    Vec<Cond> conds_{alloc_};
    int32_t pack_ = -1;           // the pack this file declared, as an index; -1 for none
    int32_t pack_line_ = 0;       // where it declared it
    bool origin_ = false;         // this file declared its origin
    Vec<Symbol> warned_{alloc_};  // macros this file has been warned about
    uint64_t def_mark_ = 0;       // nodes and shared at the open of a [NAME ...] definition
    size_t defs_from_ = 0;        // the session's definitions before this file
    Vec<std::pair<Symbol, int32_t>> unpacked_{alloc_};  // macros used before a #pack, and where
    InternSite outer_site_;  // the intern site of the read that included this file
    struct RowSegment {
        Symbol name;
        uint32_t begin = 0, end = 0;  // its bytes in the text, quotes included
    };
    struct RowAt {
        int32_t line;
        uint32_t key;  // the offset of the key
    };
    // A depth's buffers for the fold: the nodes by tag, each array's next with its tag, and
    // a joined array's children with their spans.
    struct FoldLevel {
        explicit FoldLevel(const Allocator& a) : order(a), next(a), kids(a), spans(a) {}
        Vec<uint32_t> order, next;
        Vec<Node> kids;
        Vec<SourceSpan> spans;
    };
    static constexpr uint32_t kLater = 0x80000000u, kNoNext = 0x7FFFFFFFu;
    RowSegment row_[Path::kMaxSegments];  // the open row's key: rows do not nest
    size_t row_size_ = 0;
    Vec<RowAt> rows_{alloc_};  // the rows read, for the fold and its errors
    Vec<FoldLevel> fold_{alloc_};
};

}  // namespace detail

// Sessions belong to whoever made them: from the default allocator unless an owner passes
// its own.
LoadSession::LoadSession() : LoadSession(default_allocator(MemoryKind::Config)) {}
LoadSession::LoadSession(const Allocator& alloc) : st_(detail::create<State>(alloc, alloc)) {}
void LoadSession::copy_options(const LoadSession& o) noexcept {
    limits = o.limits;
    source_map = o.source_map;
    record_for_cache = o.record_for_cache;
    cipher = o.cipher;
    text_only = o.text_only;
    refuse_includes = o.refuse_includes;
    drop_metadata = o.drop_metadata;
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
    st_->macro_nodes.erase(name);
    drop_flag(st_->flags, name);
    st_->flags.push_back({name, from, {}, 0});
}
void LoadSession::define(Symbol name, Ref<Array> body) {
    if (!body) {
        define(name);
        return;
    }
    st_->defines[name] = std::move(body);
    st_->macro_nodes.erase(name);  // counted at its first use
    drop_flag(st_->flags, name);   // a macro now, not a flag
}
void LoadSession::undefine(Symbol name) {
    st_->defines.erase(name);
    st_->macro_nodes.erase(name);
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
void LoadSession::set_stamper(Stamper stamper) {
    st_->stamper = std::move(stamper);
}
const LoadSession::Stamper& LoadSession::stamper() const noexcept {
    return st_->stamper;
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
std::span<const ShapeDecl> LoadSession::shapes() const noexcept {
    return {st_->shapes.data(), st_->shapes.size()};
}
std::span<const SkippedShape> LoadSession::skipped_shapes() const noexcept {
    return {st_->skipped_shapes.data(), st_->skipped_shapes.size()};
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
uint32_t SessionAccess::aliases(const LoadSession& s) noexcept {
    return s.st_->aliases;
}
void SessionAccess::set_aliases(LoadSession& s, uint32_t count) noexcept {
    s.st_->aliases = count;
}
void SessionAccess::set_recorded(LoadSession& s, bool recorded) noexcept {
    s.st_->recorded = recorded;
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
std::span<const ShapeDecl> SessionAccess::new_shapes(const LoadSession& s) noexcept {
    const auto& d = s.st_->shapes;
    return {d.data() + std::min(s.st_->shapes_from, d.size()), d.data() + d.size()};
}
void SessionAccess::add_shape(LoadSession& s, ShapeDecl shape) {
    for (ShapeDecl& d : s.st_->shapes)
        if (d.file == shape.file && d.line == shape.line && d.form->tag() == shape.form->tag()) {
            d = std::move(shape);
            return;
        }
    s.st_->shapes.push_back(std::move(shape));
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

namespace {
// The tree a read built in `from`, copied into an arena of its own: an array or a string of
// `from` is copied once (an array that macro expansion shared stays shared), and one of
// another owner, a define from an earlier file, is kept as it is.
class Compactor {
public:
    Compactor(detail::Arena& from, const Allocator& alloc)
        : from_(&from.owner()), to_(detail::Arena::create(alloc)) {}

    Ref<Array> array(const Array& a) {
        if (const auto it = done_.find(&a); it != done_.end()) return it->second;
        Vec<Node> kids;
        kids.reserve(a.size());
        for (const Node& n : a) kids.push_back(node(n));
        Ref<Array> copy = to_->make_array(kids, a.file(), a.line());
        if (a.written_infix()) copy->set_written_infix(true);
        if (a.sorted_for_lookup()) copy->sort_for_lookup();
        done_.emplace(&a, copy);
        return copy;
    }

private:
    Node node(const Node& n) {
        switch (n.type()) {
            case Type::String:
                if (&detail::owner_of(n.string()) != from_) return n;
                return Node(to_->make_string(n.string()->view()));
            case Type::Array:
            case Type::Command:
            case Type::Property: {
                const Array* a = n.array();
                if (&detail::owner_of(a) != from_) return n;
                return Node(array(*a), n.type());
            }
            default: return n;
        }
    }

    detail::Owner* from_;
    detail::Arena::Handle to_;
    HashMap<const Array*, Ref<Array>> done_;
};

Ref<Array> compact(const Array& root, detail::Arena& from) {
    Compactor c(from, from.allocator());
    return c.array(root);
}
}  // namespace

Expected<Ref<Array>> read_string(std::string_view text, std::string_view name,
                                 LoadSession& session) {
    const bool top = session.st_->include_stack.empty();
    if (top) {
        LoadSession::State& st = *session.st_;
        st.nodes = 0;
        st.had_rows = false;
        st.aliases = 0;
        st.new_symbols = {0, session.limits.max_new_symbols, &st.nodes, session.limits.max_nodes};
        st.includes = 0;
        st.from_macros = 0;
        st.shared = 0;
        st.recorded = session.record_for_cache;
        st.files.clear();
        st.defines_before.clear();
        if (st.recorded) {
            st.defines_before = st.defines;
            st.autorun_from = st.autorun.size();
            st.required_from = st.required.size();
            st.packs_from = st.packs.size();
            st.origins_from = st.origins.size();
            st.shapes_from = st.shapes.size();
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
    // A top-level read counts the names it interns; an include counts into the same total.
    struct CountNames {
        bool top;
        detail::NewSymbols* outer = nullptr;
        CountNames(bool is_top, detail::NewSymbols* counter) : top(is_top) {
            if (top) outer = detail::count_new_symbols(counter);
        }
        ~CountNames() {
            if (top) (void)detail::count_new_symbols(outer);
        }
        CountNames(const CountNames&) = delete;
        CountNames& operator=(const CountNames&) = delete;
    } count(top, &session.st_->new_symbols);
    detail::Parser parser(session, *arena, text, Symbol(file), 0);
    Ref<Array> root;
    Error err;
    if (!parser.run(root, err)) return Unexpected(std::move(err));
    // Rows build each key's arrays twice, a wrapper per segment and then the joined level,
    // and the first copies stay in the arena: a file of rows held 2.3 times what the same
    // file written nested holds. Its tree goes into an arena of its own, and the first goes
    // with the read. Not under a source map, which knows each array by its address.
    if (top && session.st_->had_rows && !session.source_map) root = compact(*root, *arena);
    return root;
}

Expected<Ref<Array>> read_string(std::string_view text, std::string_view name) {
    LoadSession session;
    return read_string(text, name, session);
}

Expected<Ref<Array>> read_file(std::string_view path, LoadSession& session) {
    // The file's text is the session's, tagged. Not its path: the session keeps a copy
    // of that, and a copy keeps its allocator, which must outlive it.
    const Allocator alloc = with_kind(*session.st_->alloc, MemoryKind::Config);
    const Str file = detail::normalize_path(path);
    Str text(alloc);
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
