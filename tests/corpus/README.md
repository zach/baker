# Test corpus

Original DTA-style files written for Baker's tests. They exercise the syntax real data
files use (quoted symbols, macros, conditionals, includes, merges, commands, properties,
odd whitespace) without containing any third-party content.

Every `.dta` file here must parse, and must round-trip through the printer. The fuzzer
uses this directory as its seed corpus.
