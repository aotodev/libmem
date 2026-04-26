/**
 * @file libmem.cppm
 * @brief Umbrella module for the libmem allocator and container library.
 *
 * Importing `libmem` gives access to every component:
 *
 *   - **Concepts & policies**: `memory_resource`, `shrink_policy`, `default_resource`, `threshold_policy`.
 *   - **Allocators**: `slab`, `multislab`, `arena`, `typed_arena`.
 *   - **Containers**: `pool`, `sparse_set` (placeholder).
 */
export module libmem;

export import :concepts;
export import :slab;
export import :multislab;
export import :arena;
export import :typed_arena;
export import :pool;
export import :sparse_set;
