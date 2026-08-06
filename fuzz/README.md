# Fuzzing

Coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) targets for
libmem's allocators, run under AddressSanitizer + UndefinedBehaviorSanitizer.
Requires **Clang** and a **Debug** build (so the sanitizers are active).

| Target             | Exercises                                                                |
|--------------------|--------------------------------------------------------------------------|
| `fuzz_multislab`   | `multislab` alloc/release op-streams across block sizes, caps, policies   |
| `fuzz_sparse_set`  | `sparse_set` against a `std::unordered_set` model, all three storage kinds |
| `fuzz_sparse_map`  | `sparse_map` against a `std::unordered_map` model, lifetime-trapping payload |

Each harness reads libFuzzer's random bytes as an opcode stream, issues only
valid operations (so the library's defensive asserts are never tripped), and
aborts via `FUZZ_CHECK` when a structural invariant is violated, ASan/UBSan
catch memory/UB bugs, the invariants catch logic bugs (e.g. the iteration-count
check catches lost/leaked slabs).

The two sparse harnesses are differential: every operation runs against a standard
container and the results are compared, on top of the structural invariants. The
load-bearing ones are `index_of(keys()[i]) == i` (a sparse array left pointing at
the wrong dense slot after an erase swap) and, for the map, per-slot key/payload
alignment plus a live-payload count that must equal `size()`. The map's payload
keeps a magic word cleared by its destructor, so a double destroy or a read of a
destroyed slot aborts where the mistake happens rather than surfacing later as a
wrong value.

Shared plumbing (the reader, `FUZZ_CHECK`, the counting resource) lives in
`fuzz_support.h`.

## Build & run

```sh
CXX=clang++ cmake -G Ninja -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DLIBMEM_BUILD_FUZZERS=ON
cmake --build build-fuzz -j

mkdir -p corpus/multislab
./build-fuzz/fuzz/fuzz_multislab corpus/multislab            # runs until Ctrl-C
./build-fuzz/fuzz/fuzz_multislab -max_total_time=60 corpus/multislab   # time-boxed
```

The harnesses link `-no-pie` where the linker accepts it. `-fno-rtti` leaves a
non-PIC relocation against exception typeinfo that a PIE link rejects on some libc++
builds (`relocation R_X86_64_PC32 ... recompile with -fPIC`); non-PIE costs a fuzz
harness nothing. `fuzz/CMakeLists.txt` probes for the flag rather than assuming it,
since PIE is mandatory on arm64 Darwin.

A crash writes the triggering input to `crash-<hash>`; replay it with:

```sh
./build-fuzz/fuzz/fuzz_multislab crash-<hash>
```
