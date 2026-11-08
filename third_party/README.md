# Third-party dependencies

RoDAQ keeps source dependencies as pinned Git submodules so a prepared source
tree can build without downloading packages during CMake configuration.

| Dependency | Version / commit | License | Use |
| --- | --- | --- | --- |
| TartanLlama/expected | `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4` | CC0-1.0 | `Result<T>` storage |
| fmt | 11.2.0 / `40626af88bd7df9a5fb80be7b25ac85b122d6c21` | MIT | Public C++17 formatting dependency for logging templates |
| spdlog | 1.17.0 / `79524ddd08a4ec981b7fea76afd08ee05f83755d` | MIT | Private logging backend |

Initialize dependencies with:

```sh
git submodule update --init --recursive
```

RoDAQ uses spdlog as a compiled library with `SPDLOG_FMT_EXTERNAL=ON`; the
public logging API does not expose spdlog types.
