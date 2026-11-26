#!/usr/bin/env python3
"""S7 Release Package Staging, Out-of-Tree SDK Replay, and Installed ROS 2 -> MCAP Qualification."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "s1"
WEB_DIST_DIR = REPO_ROOT / "web" / "dist"
OUT_DIR = REPO_ROOT / "build" / "s7-qualification"
RELEASE_NAME = "rodaq-0.1.0-linux-amd64-humble"
ADMIN_TOKEN = "rodaq-s7-qual-admin-secret-0123456789abc"
ROS_DOMAIN_ID = 53


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def bearer_verifier_hex(token: str) -> str:
    h = hashlib.sha256()
    h.update(b"rodaq.server-bearer.v1\x00")
    h.update(token.encode("utf-8"))
    return h.hexdigest()


def pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def chmod_tree_ro(root: Path) -> None:
    for dirpath, _, filenames in os.walk(root, topdown=False):
        dp = Path(dirpath)
        for fn in filenames:
            p = dp / fn
            mode = p.stat().st_mode
            p.chmod(0o555 if (mode & 0o111) else 0o444)
        dp.chmod(0o555)


def chmod_tree_rw(root: Path) -> None:
    if not root.exists():
        return
    for dirpath, _, filenames in os.walk(root):
        dp = Path(dirpath)
        dp.chmod(0o755)
        for fn in filenames:
            (dp / fn).chmod(0o644)


def run_checked(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    res = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(
            f"Command failed ({res.returncode}): {' '.join(cmd)}\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
        )
    return res.stdout


def http_json(
    base_url: str,
    method: str,
    path: str,
    body: dict | None = None,
    idem_key: str | None = None,
) -> tuple[int, dict, float]:
    headers = {
        "Authorization": f"Bearer {ADMIN_TOKEN}",
        "RoDAQ-API-Version": "1.0",
    }
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode("utf-8")
    if idem_key:
        headers["Idempotency-Key"] = idem_key
    req = urllib.request.Request(f"{base_url}{path}", data=data, headers=headers, method=method)
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            dt_ms = (time.perf_counter() - t0) * 1000.0
            payload = json.loads(resp.read().decode("utf-8"))
            return resp.status, payload, dt_ms
    except urllib.error.HTTPError as e:
        dt_ms = (time.perf_counter() - t0) * 1000.0
        raw = e.read().decode("utf-8", errors="replace")
        return e.code, {"raw": raw}, dt_ms


def http_bytes(base_url: str, path: str, byte_range: str | None = None) -> tuple[int, bytes, dict]:
    headers = {
        "Authorization": f"Bearer {ADMIN_TOKEN}",
        "RoDAQ-API-Version": "1.0",
    }
    if byte_range:
        headers["Range"] = byte_range
    req = urllib.request.Request(f"{base_url}{path}", headers=headers, method="GET")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read(), dict(resp.headers.items())


def stage_release_package() -> tuple[Path, Path, str]:
    print("[S7-1] Staging final release package...")
    if not (WEB_DIST_DIR / "index.html").exists():
        raise RuntimeError(f"Missing prebuilt web/dist at {WEB_DIST_DIR}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    stage_dir = OUT_DIR / "release" / RELEASE_NAME
    if stage_dir.exists():
        chmod_tree_rw(stage_dir)
        shutil.rmtree(stage_dir)
    run_checked(["cmake", "-B", str(BUILD_DIR), f"-DRODAQ_WEB_DIST_DIR={WEB_DIST_DIR}"])
    run_checked(["cmake", "--install", str(BUILD_DIR), "--prefix", str(stage_dir)])

    # Remove behavioral fake test plugins from production release staging.
    plugins_dir = stage_dir / "lib" / "rodaq" / "plugins"
    for fake in ("rodaq.fake.acquisition", "rodaq.fake.storage"):
        fp = plugins_dir / fake
        if fp.exists():
            chmod_tree_rw(fp)
            shutil.rmtree(fp)

    required_files = [
        "bin/rodaq",
        "lib/librodaq_plugin.so",
        "lib/librodaq_daq.so",
        "lib/cmake/RoDAQ/RoDAQConfig.cmake",
        "lib/cmake/RoDAQ/RoDAQTargets.cmake",
        "lib/cmake/RoDAQ/RoDAQPlugin.cmake",
        "lib/rodaq/plugins/com.rodaq.acquisition.ros2/1.0.0/manifest.json",
        "lib/rodaq/plugins/com.rodaq.storage.mcap/1.0.0/manifest.json",
        "share/rodaq/api/openapi.yaml",
        "share/rodaq/schemas/task_config.schema.json",
        "share/rodaq/web/index.html",
        "share/rodaq/web/web_manifest.json",
    ]
    for rel in required_files:
        if not (stage_dir / rel).exists():
            raise RuntimeError(f"Missing required release file: {rel}")

    targets_cmake = (stage_dir / "lib/cmake/RoDAQ/RoDAQTargets.cmake").read_text(encoding="utf-8")
    exported = [
        line.strip()
        for line in targets_cmake.splitlines()
        if line.strip().startswith("add_library(rodaq::")
    ]
    expected_exports = [
        "add_library(rodaq::plugin SHARED IMPORTED)",
        "add_library(rodaq::daq SHARED IMPORTED)",
    ]
    if sorted(exported) != sorted(expected_exports):
        raise RuntimeError(f"Unexpected exported CMake targets: {exported}")

    # Verify bin/rodaq ELF dynamic tags have zero build-tree RPATH/RUNPATH and zero node dependency.
    readelf_out = run_checked(["readelf", "-d", str(stage_dir / "bin/rodaq")])
    if str(BUILD_DIR) in readelf_out:
        raise RuntimeError(f"Build directory leaked into installed bin/rodaq ELF:\n{readelf_out}")
    if "libnode" in readelf_out.lower():
        raise RuntimeError("Unexpected Node.js shared library dependency in bin/rodaq")

    tarball = OUT_DIR / f"{RELEASE_NAME}.tar.gz"
    with tarfile.open(tarball, "w:gz") as tf:
        tf.add(stage_dir, arcname=RELEASE_NAME)
    digest = sha256_file(tarball)
    print(f"  Staged tarball: {tarball} ({tarball.stat().st_size} bytes, sha256={digest[:16]}...)")
    return stage_dir, tarball, digest


def replay_out_of_tree_sdk(tarball: Path) -> dict:
    print("[S7-2] Running out-of-tree C11/C++17 Plugin & rodaq::daq Consumer replay from relocated release tarball...")
    tmp = Path(tempfile.mkdtemp(prefix="rodaq-s7-oot-"))
    try:
        with tarfile.open(tarball, "r:gz") as tf:
            tf.extractall(tmp)
        relocated_prefix = tmp / RELEASE_NAME

        src = tmp / "oot_project"
        src.mkdir()
        (src / "CMakeLists.txt").write_text(
            """cmake_minimum_required(VERSION 3.22)
project(RoDAQOutOfTreeReplay LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(RoDAQ REQUIRED CONFIG)

add_library(oot_c_plugin SHARED c_plugin.c)
target_link_libraries(oot_c_plugin PRIVATE rodaq::plugin)
rodaq_register_plugin(oot_c_plugin
    PLUGIN_ID "com.example.s7.c_probe"
    PLUGIN_NAME "S7 OOT C Probe"
    PLUGIN_VERSION "1.0.0")

add_library(oot_cpp_plugin SHARED cpp_plugin.cpp)
target_link_libraries(oot_cpp_plugin PRIVATE rodaq::plugin)
rodaq_register_plugin(oot_cpp_plugin
    PLUGIN_ID "com.example.s7.cpp_probe"
    PLUGIN_NAME "S7 OOT C++ Probe"
    PLUGIN_VERSION "1.0.0")

add_executable(oot_consumer consumer.cpp)
target_link_libraries(oot_consumer PRIVATE rodaq::daq rodaq::plugin)
""",
            encoding="utf-8",
        )

        (src / "c_plugin.c").write_text(
            """#include <rodaq/plugin/c/plugin.h>
#include <stddef.h>

static rodaq_status_code_t c_configure(
    rodaq_plugin_context_t* context,
    const rodaq_service_registry_reference_t* services,
    const rodaq_config_t* config,
    rodaq_error_t* error) {
    (void)context;
    (void)services;
    (void)config;
    (void)error;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t c_query(
    rodaq_plugin_context_t* context,
    const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* out,
    rodaq_error_t* error) {
    (void)context;
    (void)query;
    (void)error;
    if (out != NULL) {
        out->context = NULL;
        out->vtable = NULL;
        out->vtable_size = 0;
        out->negotiated_version.major = 0;
        out->negotiated_version.minor = 0;
    }
    return RODAQ_STATUS_NOT_FOUND;
}

static rodaq_status_code_t c_shutdown(
    rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) {
    (void)context;
    (void)deadline;
    (void)error;
    return RODAQ_STATUS_OK;
}

static void c_destroy(rodaq_plugin_context_t* context) { (void)context; }

static const rodaq_plugin_instance_vtable_t kVtable = {
    {sizeof(rodaq_plugin_instance_vtable_t)},
    c_configure,
    c_query,
    c_shutdown,
    c_destroy,
};

rodaq_status_code_t rodaq_plugin_create_instance(
    rodaq_plugin_instance_t* instance, rodaq_error_t* error) {
    (void)error;
    static int marker = 1;
    if (instance == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    instance->context = (rodaq_plugin_context_t*)&marker;
    instance->vtable = &kVtable;
    return RODAQ_STATUS_OK;
}
""",
            encoding="utf-8",
        )

        (src / "cpp_plugin.cpp").write_text(
            """#include <rodaq/plugin/authoring.hpp>

class S7CppProbe : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override { return {}; }
    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

RODAQ_DEFINE_PLUGIN(S7CppProbe)
""",
            encoding="utf-8",
        )

        (src / "consumer.cpp").write_text(
            """#include <rodaq/daq/runtime.hpp>
#include <rodaq/plugin/close_future.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/registry.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "missing plugin search dir\\n";
        return 2;
    }
    rodaq::plugin::PluginManagerOptions opts;
    opts.search_paths = {std::filesystem::path(argv[1])};
    opts.max_loaded_plugins = 4;
    opts.max_instances = 4;
    opts.max_config_size = 4096;
    opts.shutdown_timeout = std::chrono::seconds(5);
    auto mgr = rodaq::plugin::PluginManager::create(std::move(opts));
    if (!mgr.ok()) {
        std::cerr << "PluginManager::create failed: " << mgr.status().message() << "\\n";
        return 3;
    }
    rodaq::plugin::ServiceRegistry reg;
    if (!reg.freeze().ok()) {
        std::cerr << "ServiceRegistry freeze failed\\n";
        return 3;
    }
    auto dl = rodaq::plugin::Deadline::after(std::chrono::seconds(5));
    if (!dl.ok()) {
        return 3;
    }
    for (const char* id : {"com.example.s7.c_probe", "com.example.s7.cpp_probe"}) {
        auto lib = mgr.value().load(id, "1.0.0");
        if (!lib.ok()) {
            std::cerr << "load failed for " << id << ": " << lib.status().message() << "\\n";
            return 4;
        }
        auto inst = lib.value().create_instance(reg, "");
        if (!inst.ok()) {
            std::cerr << "create_instance failed for " << id << ": " << inst.status().message() << "\\n";
            return 5;
        }
        auto closed = inst.value().close(dl.value());
        if (!closed.ok()) {
            std::cerr << "close failed for " << id << "\\n";
            return 6;
        }
    }
    auto sh = mgr.value().shutdown(dl.value());
    if (!sh.ok()) {
        std::cerr << "manager shutdown failed\\n";
        return 7;
    }
    // Verify public rodaq::daq symbol linkage as well.
    auto bad_open = rodaq::DaqRuntime::open(rodaq::DaqOpenOptions{});
    if (bad_open.ok()) {
        return 8;
    }
    std::cout << "OOT_REPLAY_OK\\n";
    return 0;
}
""",
            encoding="utf-8",
        )

        bld = tmp / "oot_build"
        oot_install = tmp / "oot_install"
        run_checked(
            [
                "cmake",
                "-S",
                str(src),
                "-B",
                str(bld),
                f"-DCMAKE_PREFIX_PATH={relocated_prefix}",
                "-DCMAKE_C_COMPILER=gcc-11",
                "-DCMAKE_CXX_COMPILER=g++-11",
            ]
        )
        run_checked(["cmake", "--build", str(bld), "-j4"])
        run_checked(["cmake", "--install", str(bld), "--prefix", str(oot_install)])

        oot_plugins = oot_install / "lib" / "rodaq" / "plugins"
        chmod_tree_ro(oot_plugins)

        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = f"{relocated_prefix / 'lib'}:{env.get('LD_LIBRARY_PATH', '')}"
        out = run_checked([str(bld / "oot_consumer"), str(oot_plugins)], env=env)
        if "OOT_REPLAY_OK" not in out:
            raise RuntimeError(f"Unexpected consumer output: {out}")
        print("  Out-of-tree C11 + C++17 plugin & rodaq::daq consumer replay PASSED.")
        return {
            "status": "passed",
            "c_plugin": "com.example.s7.c_probe@1.0.0",
            "cpp_plugin": "com.example.s7.cpp_probe@1.0.0",
            "relocated_prefix_tested": True,
        }
    finally:
        chmod_tree_rw(tmp)
        shutil.rmtree(tmp, ignore_errors=True)


def run_installed_ros2_mcap_workload_and_crash_recovery(tarball: Path) -> dict:
    print("[S7-3] Running relocated release ROS 2 Humble -> MCAP workload, benchmark, ros2 bag info & SIGKILL crash recovery...")
    tmp = Path(tempfile.mkdtemp(prefix="rodaq-s7-runtime-"))
    daemon: subprocess.Popen | None = None
    ros_pub: subprocess.Popen | None = None
    try:
        with tarfile.open(tarball, "r:gz") as tf:
            tf.extractall(tmp)
        prefix = tmp / RELEASE_NAME
        plugins = tmp / "plugins"
        shutil.copytree(prefix / "lib/rodaq/plugins", plugins)
        chmod_tree_ro(plugins)

        state_dir = tmp / "state"
        vol_dir = tmp / "volume"
        state_dir.mkdir()
        vol_dir.mkdir()

        cred_file = tmp / "credentials.json"
        cred_file.write_text(
            json.dumps(
                {
                    "format_version": "1.0",
                    "credentials": [
                        {
                            "credential_id": "s7-admin",
                            "principal": "s7-admin",
                            "scopes": ["read", "control", "admin"],
                            "verifier": bearer_verifier_hex(ADMIN_TOKEN),
                            "credential_epoch": "1",
                            "enabled": True,
                        }
                    ],
                },
                indent=2,
            ),
            encoding="utf-8",
        )
        cred_file.chmod(0o600)

        port = pick_free_port()
        base_url = f"http://127.0.0.1:{port}"
        dep_file = tmp / "deployment.json"

        def write_dep(mode: str) -> None:
            dep_file.write_text(
                json.dumps(
                    {
                        "format_version": "1.0",
                        "static_assets_root": str(prefix / "share/rodaq/web"),
                        "server": {
                            "format_version": "1.0",
                            "http": {"listen_address": "127.0.0.1", "port": port, "worker_threads": 2},
                            "security": {"credential_store": str(cred_file)},
                        },
                        "daq": {
                            "state_root": str(state_dir),
                            "volume_root": str(vol_dir),
                            "mode": mode,
                            "plugin_search_paths": [str(plugins)],
                            "qualified_runtime_profile_id": "s7-devhost-humble-mcap",
                            "max_tasks": 2,
                            "max_pipelines_per_task": 2,
                            "volume_name": "local",
                            "profiles": [
                                {
                                    "name": "default",
                                    "payload_pool_bytes": "2097152",
                                    "max_payload_bytes": "65536",
                                    "max_logical_bytes": "2097152",
                                    "max_segment_physical_bytes": "16777216",
                                    "max_descriptors": 32,
                                    "max_prepared_descriptors": 32,
                                    "max_streams": 8,
                                }
                            ],
                            "allowed_backends": [
                                {
                                    "plugin_id": "com.rodaq.acquisition.ros2",
                                    "plugin_version": "1.0.0",
                                    "role": "acquisition",
                                    "support_status": "required",
                                    "qualification_status": "qualified",
                                },
                                {
                                    "plugin_id": "com.rodaq.storage.mcap",
                                    "plugin_version": "1.0.0",
                                    "role": "storage",
                                    "storage_format": "mcap",
                                    "support_status": "required",
                                    "qualification_status": "qualified",
                                },
                            ],
                        },
                    },
                    indent=2,
                ),
                encoding="utf-8",
            )

        def start_daemon(mode: str) -> subprocess.Popen:
            write_dep(mode)
            cmd = (
                f'source /opt/ros/humble/setup.bash && '
                f'export LD_LIBRARY_PATH="{prefix / "lib"}:$LD_LIBRARY_PATH" && '
                f'exec "{prefix / "bin/rodaq"}" --deployment "{dep_file}"'
            )
            p = subprocess.Popen(
                ["bash", "-lc", cmd],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline:
                try:
                    code, data, _ = http_json(base_url, "GET", "/api/system")
                    if code == 200 and data.get("data", {}).get("server_phase") == "ready":
                        return p
                except Exception:
                    pass
                if p.poll() is not None:
                    out = p.stdout.read() if p.stdout else ""
                    raise RuntimeError(f"rodaq exited early ({p.returncode}):\n{out}")
                time.sleep(0.05)
            raise RuntimeError("rodaq did not reach ready phase")

        # Multi-topic ROS 2 Humble publisher: /s7_chatter (100 Hz) + /s7_telemetry (50 Hz) = 150 msg/s.
        pub_py = tmp / "s7_pub.py"
        pub_py.write_text(
            """import rclpy, time
from rclpy.node import Node
from std_msgs.msg import String
rclpy.init()
node = Node('rodaq_s7_qual_pub')
p1 = node.create_publisher(String, '/s7_chatter', 50)
p2 = node.create_publisher(String, '/s7_telemetry', 50)
payload_pad = 'X' * 512
i = 0
try:
    while rclpy.ok():
        p1.publish(String(data=f's7-chatter-{i}-{payload_pad}'))
        if i % 2 == 0:
            p2.publish(String(data=f's7-telemetry-{i}-{payload_pad}'))
        i += 1
        time.sleep(0.01)
except KeyboardInterrupt:
    pass
finally:
    node.destroy_node()
    rclpy.shutdown()
""",
            encoding="utf-8",
        )
        ros_pub = subprocess.Popen(
            [
                "bash",
                "-lc",
                f'source /opt/ros/humble/setup.bash && export ROS_DOMAIN_ID={ROS_DOMAIN_ID} && exec python3 "{pub_py}"',
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        daemon = start_daemon("initialize")

        # Verify static web root & manifest served by relocated installed binary.
        st_html, html_bytes, _ = http_bytes(base_url, "/")
        assert st_html == 200 and b"<div id=\"app\">" in html_bytes

        # Create multi-stream ROS 2 -> MCAP Task with 2s segment rotation.
        task_cfg = {
            "format_version": "1.0",
            "name": "s7-humble-mcap-qual",
            "recording_mode": "continuous",
            "start_policy": {"mode": "all_required"},
            "recovery_policy": {"mode": "manual"},
            "segment_policy": {
                "max_duration": {"value": "2", "unit": "s"},
                "max_physical_bytes": None,
            },
            "retention_policy": {
                "mode": "keep_all",
                "max_duration": None,
                "max_physical_bytes": None,
                "include_previous_continuous": False,
            },
            "pipelines": [
                {
                    "required": True,
                    "resource_profile": "default",
                    "output_root": "local",
                    "acquisition": {
                        "plugin_id": "com.rodaq.acquisition.ros2",
                        "plugin_version": "1.0.0",
                        "config": {
                            "format_version": "1.0",
                            "domain_id": ROS_DOMAIN_ID,
                            "executor_threads": 2,
                            "graph_poll_interval": {"value": "200", "unit": "ms"},
                            "qos_change_debounce": {"value": "100", "unit": "ms"},
                            "streams": [
                                {"stream_key": "chatter", "topic": "/s7_chatter", "qos": {"mode": "adapt", "depth": "50"}},
                                {"stream_key": "telemetry", "topic": "/s7_telemetry", "qos": {"mode": "adapt", "depth": "50"}},
                            ],
                        },
                    },
                    "storage": {
                        "plugin_id": "com.rodaq.storage.mcap",
                        "plugin_version": "1.0.0",
                        "config": {
                            "format_version": "1.0",
                            "compression": "none",
                            "chunk_mode": "none",
                            "chunk_crc": False,
                            "message_index": False,
                            "summary": False,
                            "rosbag2_metadata": "legacy_ros_distro",
                        },
                    },
                    "streams": [
                        {"stream_key": "chatter", "name": "chatter", "readiness_policy": "entry_ready"},
                        {"stream_key": "telemetry", "name": "telemetry", "readiness_policy": "entry_ready"},
                    ],
                }
            ],
        }
        code, created, _ = http_json(base_url, "POST", "/api/tasks", task_cfg, idem_key="s7-create-1")
        assert code == 201, f"create failed: {code} {created}"
        task_id = created["data"]["task_id"]

        def wait_op(op_id: str) -> dict:
            dl = time.monotonic() + 15.0
            while time.monotonic() < dl:
                c, body, _ = http_json(base_url, "GET", f"/api/tasks/{task_id}/operations/{op_id}")
                if c == 200 and body["data"]["state"] in ("succeeded", "failed", "partially_succeeded", "superseded"):
                    return body["data"]
                time.sleep(0.05)
            raise RuntimeError(f"Operation {op_id} timed out")

        code, op, _ = http_json(
            base_url, "POST", f"/api/tasks/{task_id}/operations", {"kind": "start", "parameters": {}}, idem_key="s7-start-1"
        )
        assert code == 202
        st = wait_op(op["data"]["operation_id"])
        assert st["state"] == "succeeded", f"start failed: {st}"

        # Sample system & runtime for 4.5s (triggers >=2 rotated MCAP segments) and measure API latency + RSS/FDs.
        api_latencies_ms: list[float] = []
        rss_samples: list[int] = []
        fd_samples: list[int] = []
        thread_samples: list[int] = []
        max_accepted_records = 0
        for _ in range(18):
            c1, sys_body, dt1 = http_json(base_url, "GET", "/api/system")
            c2, rt_body, dt2 = http_json(base_url, "GET", f"/api/tasks/{task_id}/runtime")
            assert c1 == 200 and c2 == 200
            api_latencies_ms.extend([dt1, dt2])
            res = sys_body["data"]["resources"]
            rss_samples.append(int(res["rss_bytes"]))
            fd_samples.append(int(res["open_fds"]))
            thread_samples.append(int(res["threads"]))
            for p in rt_body["data"]["pipelines"]:
                max_accepted_records = max(max_accepted_records, int(p["admission"]["accepted_records"]))
            time.sleep(0.25)

        # Pause -> Resume -> Stop lifecycle check
        for kind, key in (("pause", "s7-pause-1"), ("resume", "s7-resume-1"), ("stop", "s7-stop-1")):
            if kind == "stop":
                time.sleep(0.5)
            c, op_b, _ = http_json(
                base_url, "POST", f"/api/tasks/{task_id}/operations", {"kind": kind, "parameters": {}}, idem_key=key
            )
            assert c == 202
            res_op = wait_op(op_b["data"]["operation_id"])
            assert res_op["state"] == "succeeded", f"{kind} failed: {res_op}"

        # Query committed MCAP artifacts & verify >=2 segments committed.
        c, arts_body, _ = http_json(base_url, "GET", f"/api/artifacts?task_id={task_id}&limit=50")
        assert c == 200
        arts = arts_body["data"]["items"]
        assert len(arts) >= 2, f"Expected >=2 rotated MCAP artifacts, got {len(arts)}"

        first_art_id = arts[0]["artifact_id"]
        c, detail_body, _ = http_json(base_url, "GET", f"/api/artifacts/{first_art_id}")
        assert c == 200
        mcap_file = next(f for f in detail_body["data"]["files"] if f["display_name"].endswith(".mcap"))

        # Full download + Range 206 download check.
        sc_full, full_bytes, _ = http_bytes(base_url, mcap_file["download_url"])
        assert sc_full == 200 and len(full_bytes) == int(mcap_file["size"])
        sc_range, range_bytes, _ = http_bytes(base_url, mcap_file["download_url"], byte_range="bytes=0-15")
        assert sc_range == 206 and range_bytes == full_bytes[:16]

        # Independent ROS 2 Humble `ros2 bag info` verification on downloaded MCAP!
        dl_mcap = tmp / "downloaded_s7.mcap"
        dl_mcap.write_bytes(full_bytes)
        bag_info = run_checked(
            ["bash", "-lc", f'source /opt/ros/humble/setup.bash && ros2 bag info -s mcap "{dl_mcap}"']
        )
        assert "/s7_chatter" in bag_info and "/s7_telemetry" in bag_info, f"Missing topics in ros2 bag info:\n{bag_info}"

        # Start Run #2, let record, then SIGKILL (unclean crash), restart in `existing` mode, verify recovery!
        c, op_b, _ = http_json(
            base_url, "POST", f"/api/tasks/{task_id}/operations", {"kind": "start", "parameters": {}}, idem_key="s7-start-crash"
        )
        assert c == 202
        assert wait_op(op_b["data"]["operation_id"])["state"] == "succeeded"
        time.sleep(0.8)

        daemon.kill()  # SIGKILL sudden crash!
        daemon.wait(timeout=5)
        daemon = None

        # Restart in existing mode -> verify recovery + preserved committed artifact readback!
        t_rec0 = time.perf_counter()
        daemon = start_daemon("existing")
        recovery_ms = (time.perf_counter() - t_rec0) * 1000.0

        c, arts_after, _ = http_json(base_url, "GET", f"/api/artifacts?task_id={task_id}&limit=50")
        assert c == 200 and len(arts_after["data"]["items"]) >= len(arts)
        sc_after, after_bytes, _ = http_bytes(base_url, mcap_file["download_url"])
        assert sc_after == 200 and after_bytes == full_bytes

        # Clean SIGTERM shutdown -> exit code 0.
        daemon.send_signal(signal.SIGTERM)
        rc = daemon.wait(timeout=10)
        assert rc == 0, f"Expected exit code 0 on SIGTERM after crash recovery, got {rc}"
        daemon = None

        api_latencies_ms.sort()
        p50 = statistics.median(api_latencies_ms)
        p99 = api_latencies_ms[min(len(api_latencies_ms) - 1, int(len(api_latencies_ms) * 0.99))]
        result = {
            "status": "passed",
            "host_caveat": "Dev-host Ubuntu 22.04 measurement (NOT target industrial hardware or 100,300 rec/s heavy reference workload)",
            "committed_mcap_artifacts_before_crash": len(arts),
            "committed_mcap_artifacts_after_sigkill_recovery": len(arts_after["data"]["items"]),
            "max_accepted_records_observed": max_accepted_records,
            "mcap_download_bytes": len(full_bytes),
            "range_206_verified": True,
            "ros2_bag_info_verified": True,
            "sigkill_restart_recovery_ms": round(recovery_ms, 1),
            "api_latency_ms": {"samples": len(api_latencies_ms), "p50": round(p50, 2), "p99": round(p99, 2)},
            "process_hwm": {
                "rss_bytes_max": max(rss_samples),
                "open_fds_max": max(fd_samples),
                "threads_max": max(thread_samples),
            },
            "sigterm_clean_exit_code": 0,
        }
        print(f"  Installed ROS 2 -> MCAP workload, ros2 bag info & SIGKILL recovery PASSED: {json.dumps(result)}")
        return result
    finally:
        if ros_pub and ros_pub.poll() is None:
            ros_pub.terminate()
            try:
                ros_pub.wait(timeout=3)
            except subprocess.TimeoutExpired:
                ros_pub.kill()
        if daemon and daemon.poll() is None:
            daemon.kill()
            daemon.wait(timeout=5)
        chmod_tree_rw(tmp)
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    stage_dir, tarball, digest = stage_release_package()
    oot = replay_out_of_tree_sdk(tarball)
    runtime = run_installed_ros2_mcap_workload_and_crash_recovery(tarball)
    report = {
        "release_name": RELEASE_NAME,
        "staged_dir": str(stage_dir.relative_to(REPO_ROOT)),
        "tarball": str(tarball.relative_to(REPO_ROOT)),
        "tarball_sha256": digest,
        "out_of_tree_sdk_replay": oot,
        "installed_ros2_mcap_qualification": runtime,
    }
    report_path = OUT_DIR / "s7-release-qualification.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[S7] Wrote qualification report to {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
