// Copyright (c) 2026 Aoto
// SPDX-License-Identifier: MIT
/**
 * @file assert.hpp
 * @brief `LIBMEM_ASSERT`: `contract_assert` when opted in and supported, `assert` otherwise.
 *
 * Include in a module's global fragment. Opt in by defining `LIBMEM_CONTRACTS`
 * project-wide; see docs/integration.md for what a consumer then needs.
 */
#pragma once

#if defined(LIBMEM_CONTRACTS) && defined(__cpp_contracts)
#define LIBMEM_ASSERT(...) contract_assert(__VA_ARGS__) // NOLINT(cppcoreguidelines-macro-usage)
#else
#include <cassert>
#define LIBMEM_ASSERT(...) assert(__VA_ARGS__) // NOLINT(cppcoreguidelines-macro-usage)
#endif
