/**
 * @file export.h
 * @brief Symbol visibility macros for shared library builds.
 *
 * When `libmem` is built as a shared library, the build system defines
 * `LIBMEM_SHARED` (always) and `LIBMEM_BUILDING` (only during library
 * compilation). Consumers that link against the shared library see
 * `LIBMEM_SHARED` but not `LIBMEM_BUILDING`, so the import/export
 * direction is resolved automatically.
 *
 * For static builds, `LIBMEM_EXPORT` expands to nothing.
 */
#pragma once

#if defined(LIBMEM_SHARED)
#if defined(_WIN32)
#if defined(LIBMEM_BUILDING)
#define LIBMEM_EXPORT __declspec(dllexport)
#else
#define LIBMEM_EXPORT __declspec(dllimport)
#endif
#else
#define LIBMEM_EXPORT __attribute__((visibility("default")))
#endif
#else
#define LIBMEM_EXPORT
#endif
