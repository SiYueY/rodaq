# ROS 2 Reference Acquisition Plugin

This plugin is a Framework consumer used for integration validation. Its
`ControlInterface` and `SinkInterface` are plugin-local contracts and are not
part of the RoDAQ Plugin Foundation or the future RoDAQ Acquisition protocol.

## Reference sources

- `ros2/rosbag2` 0.15.15 (`ros-humble-rosbag2`)
- `rclcpp` 16.0.21 (`ros-humble-rclcpp`)
- `rosbag2_transport` 0.15.15 (`Rosbag2QoS::adapt_request_to_offers`)
- `nlohmann/json` 3.10.5 (strict plugin-local configuration parsing)
- ROS 2 Humble GenericSubscription and serialized-message APIs

Reference behavior used:

- generic, runtime-typed subscription creation;
- publisher endpoint inspection and rosbag2 QoS adaptation, with optional
  plugin-local reliability/durability/history/depth overrides;
- serialized CDR delivery;
- plugin-owned rclcpp Context, node, executor, and worker lifetime;
- system-clock receive timestamps.

The plugin-local configuration records `topic`, `type`, optional
`schema_encoding`, optional `definition`, and an optional `qos` object.
Humble does not expose a reliable public API for recovering the original ROS
message definition through GenericSubscription. The reference plugin therefore
requires the definition to be supplied explicitly when schema data is needed;
it does not claim automatic schema discovery or synthesize a definition.

The current reference implementation is deliberately bounded to one explicitly
configured topic. It does not implement unbounded graph-driven subscription.

No rosbag2 CLI, keyboard control, pause service, snapshot service, rosbag
writer, or rosbag2 recording orchestration is copied or embedded. Only the
public QoS adaptation policy is reused through `rosbag2_transport`.

## License

The referenced ROS 2 and rosbag2 packages are Apache License 2.0.
`nlohmann/json` is MIT licensed. This plugin uses installed public APIs and
does not vendor or copy their source.
