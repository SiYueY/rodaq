# Third-party dependencies

RoDAQ keeps source dependencies as pinned Git submodules so a prepared source
tree can build without downloading packages during CMake configuration.

| Dependency | Version / commit | License | Use |
| --- | --- | --- | --- |
| TartanLlama/expected | `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4` | CC0-1.0 | `Result<T>` storage |
| fmt | 11.2.0 / `40626af88bd7df9a5fb80be7b25ac85b122d6c21` | MIT | Public C++17 formatting dependency for logging templates |
| spdlog | 1.17.0 / `79524ddd08a4ec981b7fea76afd08ee05f83755d` | MIT | Private logging backend |
| foxglove/mcap (C++) | `releases/cpp/v2.1.3` / `1420296ffcfdcde4b6894c0c1aba0ad083f93dde` | MIT | Standalone upstream header-only MCAP C++ library for `com.rodaq.storage.mcap` (`MCAP_COMPRESSION_NO_LZ4`, `MCAP_COMPRESSION_NO_ZSTD`) |

Initialize dependencies with:

```sh
git submodule update --init --recursive
```

RoDAQ uses spdlog as a compiled library with `SPDLOG_FMT_EXTERNAL=ON`; the
public logging API does not expose spdlog types.

The installed C++ SDK exposes exactly fmt 11.2.0 (CMake `fmt::fmt`) and the pinned
tl_expected 1.3.1 headers bundled under the SDK include directory through
`rodaq::plugin`. `rodaq::daq` inherits that contract. spdlog, nlohmann/json and
OpenSSL are implementation dependencies, not public SDK types. Normal
`find_package(RoDAQ CONFIG REQUIRED)` exports only `rodaq::plugin` and
`rodaq::daq`; ROS 2 support is available only through
`find_package(RoDAQ CONFIG REQUIRED COMPONENTS ros2_support)` when built and its
ament_index_cpp dependency is available.
