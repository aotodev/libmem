# Fuzzing

Coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) targets for
libmem's allocators, run under AddressSanitizer + UndefinedBehaviorSanitizer.
Requires **Clang** and a **Debug** build (so the sanitizers are active).

| Target           | Exercises                                                              |
|------------------|------------------------------------------------------------------------|
| `fuzz_multislab` | `multislab` alloc/release op-streams across block sizes, caps, policies |

Each harness reads libFuzzer's random bytes as an opcode stream, issues only
valid operations (so the library's defensive asserts are never tripped), and
aborts via `FUZZ_CHECK` when a structural invariant is violated — ASan/UBSan
catch memory/UB bugs, the invariants catch logic bugs (e.g. the iteration-count
check catches lost/leaked slabs).

## Build & run

```sh
CXX=clang++ cmake -G Ninja -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DBUILD_FUZZERS=ON
cmake --build build-fuzz -j

mkdir -p corpus/multislab
./build-fuzz/fuzz/fuzz_multislab corpus/multislab            # runs until Ctrl-C
./build-fuzz/fuzz/fuzz_multislab -max_total_time=60 corpus/multislab   # time-boxed
```

A crash writes the triggering input to `crash-<hash>`; replay it with:

```sh
./build-fuzz/fuzz/fuzz_multislab crash-<hash>
```
