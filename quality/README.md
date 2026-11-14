# Engineering checks

The authoritative design and stage status live in [Notion 00](https://app.notion.com/p/418da2b2a80f8141a08ae0cc08a73962).
Baseline files contain source identities only; this directory is not a design mirror.

Prepare the pinned submodules (`git submodule update --init --recursive`), GCC 11,
CMake 3.22+, clang-format/clang-tidy 14, nlohmann/json 3.10.5,
OpenSSL 3.x development packages and GoogleTest 1.11 (test-only).
The SDK builds without ROS 2, Drogon, Web or Node.js:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
python3 tools/check_format.py
```

Incremental work should build the affected targets and select related CTest tests
with `-R`. Enable `RODAQ_ENABLE_CLANG_TIDY=ON` for project translation units.
Separate `asan-ubsan` and `tsan` presets are qualification entry points, not
claims that those matrices have passed. Sanitizers never enter installed SDK
interface options. `BUILD_TESTING=OFF` removes GoogleTest from the normal build.

`rodaq_package_contract` installs into a temporary prefix, relocates it, configures
a separate consumer and checks that the only RoDAQ imports are `rodaq::plugin`
and `rodaq::daq`. At S0 the latter is a composition boundary without Task services;
neither target establishes Stable ABI or backend readiness.

Build logs and JUnit output belong under `build/<preset>/`; CTest retains details
under `Testing/Temporary`. CI archives build/test/cache evidence for 30 days.
Do not treat an expired CI artifact or historical run as current verification.

Before stage exit, perform a separate static review pass covering public API
necessity, responsibilities/dependencies, ownership and borrowed lifetime,
exception/cleanup failures, bounded execution, lock/callback ordering and tests.
Check blank lines between function definitions independently of formatting.
Record findings, evidence and unresolved qualifications in Notion 00. Stage
entry/exit must bind the actual code commit and the design baseline identity.
