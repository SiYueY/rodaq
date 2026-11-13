# MCAP Reference Storage Plugin

This plugin is a Framework consumer used for integration validation. Its local
open/register/write/close interface is test-only and is not the future RoDAQ
Storage contract, Volume policy, or Commit authority.

## Reference sources

- Foxglove MCAP C++ writer via `mcap_vendor` 0.15.16
- `rosbag2_storage_mcap` 0.15.17 (`ros-humble-rosbag2-storage-mcap`)
- `nlohmann/json` 3.10.5 (strict plugin-local configuration parsing)

Reference behavior used:

- MCAP writer open/write/close;
- ROS 2 profile;
- schema and channel registration;
- summary, chunk, and message-index options.

The plugin-local configuration maps:

- `chunk_size`
- `compression` (`none`, `lz4`, `zstd`)
- `compression_level` (`fastest`, `fast`, `default`, `slow`, `slowest`)
- `crc`
- `message_index`
- `summary`

Defaults are 786432 bytes, `none`, `default`, CRC enabled, message indexing
enabled, and summary enabled. Writes to an unregistered channel fail closed with
`NotFound`.

The plugin does not implement MCAP binary record encoding, chunk compression,
footer, index, summary, or compression formats itself.

## License

`mcap_vendor` and `rosbag2_storage_mcap` are provided under Apache License 2.0.
`nlohmann/json` is MIT licensed. This plugin uses installed public APIs and does
not vendor their source.
