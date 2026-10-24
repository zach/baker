#pragma once

// BAKER_DEV_TEST_CASE(name): a test of what only a development build does: live edits,
// reloads, toggling a layer, write-back, promotion, caches. It compiles in every build, and a
// shipping build reports it skipped. tools/check_tests.py fails a plain TEST_CASE that calls
// one of those without handling shipping.

#include <doctest.h>
#include <baker/error.hpp>

#define BAKER_DEV_TEST_CASE(name) TEST_CASE((name) * doctest::skip(baker::kShipping))
