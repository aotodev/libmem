/**
 * @file libmem.cppm
 * @brief Umbrella module for the libmem allocator and container library.
 *
 * Importing `libmem` gives access to every component:
 *
 *   - **Concepts & policies**: `memory_resource`, `aligned_memory_resource`, `shrink_policy`.
 *   - **Resources**: `default_resource`, `resource_ref`, `allocator_resource`, `threshold_policy`.
 *   - **Identifiers**: `regular_indexable_id`, `null_id_v`, `to_index`.
 *   - **Storage**: `inline_storage`, `fixed_storage`, `dynamic_storage`, `relocate_grow`.
 *   - **Allocators**: `slab`, `multislab`, `arena`, `typed_arena`.
 *   - **Containers**: `pool`, `spsc_ring`, `sparse_set`, `sparse_map`.
 */
export module libmem;

export import :concepts;
export import :identifier;
export import :storage;
export import :slab;
export import :multislab;
export import :arena;
export import :typed_arena;
export import :pool;
export import :spsc_ring;
export import :sparse_set;
