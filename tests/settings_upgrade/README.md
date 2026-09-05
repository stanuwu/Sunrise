# Settings upgrade regressions

These tests compile the production settings migration code with synthetic JSON. They need a C++20
compiler and CMake, and run on Linux or Windows without the game, Windows SDK, or third-party
dependencies.

```sh
cmake -S tests/settings_upgrade -B build/settings-upgrade-tests
cmake --build build/settings-upgrade-tests --config Release
ctest --test-dir build/settings-upgrade-tests -C Release --output-on-failure
```

The nine cases cover empty roots with JSON whitespace, nonempty roots, a closing brace inside a
string value, and existing version members. Each case checks the full migrated text, exact-fit
storage, rejection of a buffer one byte too small, and recognition of the upgraded version.
Checks remain active in Release builds.

Before the fix, the four empty-root cases produce a trailing comma after the inserted version.
Startup passes that text to the strict settings parser, which rejects it and stops initialization.
