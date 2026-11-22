"""Independent rosbag2 CLI playback oracle for exported Core MCAP evidence.

Run in a sourced Humble/Jazzy environment. The recorder test exports only files
read through ArtifactService; this probe uses rclpy and rosbag2, not RoDAQ code.
"""

import argparse
import json
import subprocess
import time
from pathlib import Path

import rclpy
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import String


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bag", type=Path)
    parser.add_argument("--expected-count", type=int, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=20)
    args = parser.parse_args()
    if not 1 <= args.expected_count <= 10000 or not 1 <= args.timeout <= 120:
        parser.error("probe requires bounded positive count and timeout")

    rclpy.init()
    node = rclpy.create_node("rodaq_playback_oracle")
    count = 0
    invalid = 0

    def received(message):
        nonlocal count, invalid
        count += 1
        invalid += message.data != "hello"

    node.create_subscription(
        String,
        "/rodaq_test_0",
        received,
        QoSProfile(
            depth=100,
            reliability=ReliabilityPolicy.BEST_EFFORT,
            durability=DurabilityPolicy.VOLATILE,
        ),
    )
    process = None
    try:
        with args.report.with_suffix(".play.log").open("w") as output:
            process = subprocess.Popen(
                [
                    "ros2", "bag", "play", "-s", "mcap", str(args.bag.resolve()),
                    "--delay", "1", "--disable-keyboard-controls",
                ],
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
            end = time.monotonic() + args.timeout
            while process.poll() is None and time.monotonic() < end:
                rclpy.spin_once(node, timeout_sec=0.05)
            if process.poll() is None:
                raise TimeoutError("rosbag2 playback exceeded probe budget")
            drain = time.monotonic() + 0.5
            while time.monotonic() < drain:
                rclpy.spin_once(node, timeout_sec=0.05)
            report = {
                "bag": str(args.bag.resolve()),
                "expected_count": args.expected_count,
                "received_count": count,
                "invalid_payload_count": invalid,
                "player_exit_code": process.returncode,
            }
            args.report.write_text(json.dumps(report, indent=2) + "\n")
            if process.returncode != 0 or count != args.expected_count or invalid:
                raise RuntimeError(f"playback oracle failed: {report}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
