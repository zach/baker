#pragma once

// The language's migrations as a call: each old form a release's migration rewrites, where
// it is, what it is, and the edits that rewrite it. For an editor's warning and quick fix in
// every file of a project, a CI step or pre-commit hook that fails on an old form, an agent
// told what to change, and baker_migrate, a loop over files around it. Part of baker::tools.

#include <baker/tools/structure.hpp>

#include <cstdint>
#include <span>

namespace baker::tools {

// A migration Baker has, named for what it rewrites.
enum class Migration : uint8_t {
    // 0.7.0: metadata is spelled with '@', (speed 4 (range 0 10)) -> (speed 4 (@range 0 10)).
    // The old rule's trailing run after a value; a shape's own words and a field's after its
    // type, in (shapes ...) or #shape; a {func}'s or {override}'s (status ...) and (doc ...).
    Metadata,
    // 0.7.0: a file declares with directives. A first (pack NAME ...) -> #pack (NAME ...);
    // (origin "source" ...), first or after the pack -> #origin ("source" ...); a top-level
    // (shapes (a ...) (b ...)) -> #shape (a ...) and #shape (b ...), each on a line of its own;
    // #define NAME (body) at the top level -> [NAME body].
    Declarations,
};
inline constexpr size_t kMigrations = 2;
// "metadata", "declarations": as baker_migrate --only names them.
[[nodiscard]] const char* migration_name(Migration migration) noexcept;

// Replaces [begin, end) of the text with `text`, which has static storage.
struct TextEdit {
    uint32_t begin = 0, end = 0;
    std::string_view text;
};

struct MigrationFinding {
    Migration migration{};
    uint32_t begin = 0, end = 0;  // the old form, or the tag a rename marks
    // What is old and what to write, as a check words it: ErrorCode::Syntax, at the line.
    Error warning;
    // The smallest edits that rewrite it, in order. None when the migration leaves it to a
    // person, and `warning` says why: an array with no untagged value whose last child is
    // tagged with a metadata word, (limits (min 0) (step 0.5)), reads as all value under both
    // rules; in prose, `(range 0 10)` names metadata without a value.
    List<TextEdit> edits;
};

struct MigrateOptions {
    // The migrations to look for, a bit for each (1 << Migration); all by default.
    uint32_t migrations = (1U << kMigrations) - 1;
    // Text whose lines are not its own, DTA in C++ string literals: a (shapes ...) splits
    // onto its line, the forms a space apart, and no line is re-indented.
    bool fixed_lines = false;
    // Prose, DTA quoted in a document: a top-level form tagged with a metadata word is a
    // finding without edits.
    bool prose = false;
    Symbol file{};  // names the warnings' and the read's errors' file
};

struct Migrated {
    // In text order. Edits never overlap, within a finding or across findings, so any of
    // them apply together (apply_findings).
    List<MigrationFinding> findings;
    List<Error> errors;  // the tolerant read's; the findings it could locate are still given
};

// Finds every old form in `text` and writes the text with all their edits applied to `out`:
// what baker_migrate writes for the file.
//  - Only the old forms change: comments, layout, macros and the forms' insides stay as
//    written, and migrating the result finds nothing with edits.
//  - Read tolerantly (read_structure): text that does not parse gets the findings that can be
//    located, and the read's errors. No file is read and no #include followed.
//  - Either migration may be asked for alone; both together are one pass, so their order does
//    not matter.
// Linear in the text.
Migrated migrate_text(std::string_view text, TextSink out, const MigrateOptions& options = {});

// Writes `text` with the edits of `findings` applied to `out`: one finding's quick fix, or
// several at once.
void apply_findings(std::string_view text, std::span<const MigrationFinding> findings,
                    TextSink out);

}  // namespace baker::tools
