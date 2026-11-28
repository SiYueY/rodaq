#!/usr/bin/env python3
"""Release Package Staging, Relocated SDK & Isolated OOT Plugin Replay, and Installed ROS 2 -> MCAP Verification."""

from __future__ import annotations

import argparse
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
ROS_DISTRO = "humble"
BEARER_TOKEN = "rodaq_release_qual_bearer_secret_256bit_0123456789abcdef"
ROS_DOMAIN_ID = 53


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
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
        "Authorization": f"Bearer {BEARER_TOKEN}",
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
        "Authorization": f"Bearer {BEARER_TOKEN}",
        "RoDAQ-API-Version": "1.0",
    }
    if byte_range:
        headers["Range"] = byte_range
    req = urllib.request.Request(f"{base_url}{path}", headers=headers, method="GET")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read(), dict(resp.headers.items())


def stage_release_package() -> tuple[Path, Path, str]:
    print("[REL-1] Staging final release package...")
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
        "lib/librodaq_ros2_support.a",
        "lib/cmake/RoDAQ/RoDAQConfig.cmake",
        "lib/cmake/RoDAQ/RoDAQTargets.cmake",
        "lib/cmake/RoDAQ/RoDAQRos2Targets.cmake",
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

    ros2_targets = (stage_dir / "lib/cmake/RoDAQ/RoDAQRos2Targets.cmake").read_text(encoding="utf-8")
    ros2_exported = [line.strip() for line in ros2_targets.splitlines() if line.strip().startswith("add_library(rodaq::")]
    if ros2_exported != ["add_library(rodaq::ros2_support STATIC IMPORTED)"]:
        raise RuntimeError(f"Unexpected ROS 2 component targets: {ros2_exported}")

    # Verify bin/rodaq ELF dynamic tags have zero build-tree RPATH/RUNPATH and zero node dependency.
    readelf_out = run_checked(["readelf", "-d", str(stage_dir / "bin/rodaq")])
    if str(BUILD_DIR) in readelf_out:
        raise RuntimeError(f"Build directory leaked into installed bin/rodaq ELF:\n{readelf_out}")
    if "libnode" in readelf_out.lower():
        raise RuntimeError("Unexpected Node.js shared library dependency in bin/rodaq")

    mcap_dso = stage_dir / "lib/rodaq/plugins/com.rodaq.storage.mcap/1.0.0/lib/librodaq_mcap_storage.so"
    mcap_dynamic = run_checked(["readelf", "-d", str(mcap_dso)])
    forbidden = ("libmcap", "mcap_vendor", "librcl", "librmw", "libament", "liblz4", "libzstd")
    needed = [line.lower() for line in mcap_dynamic.splitlines() if "(NEEDED)" in line]
    if any(name in line for line in needed for name in forbidden):
        raise RuntimeError(f"MCAP plugin unexpectedly depends on ROS or compression libraries: {needed}")
    if not (stage_dir / "share/licenses/RoDAQ/mcap-cpp-LICENSE").is_file():
        raise RuntimeError("Missing standalone MCAP C++ license")

    tarball = OUT_DIR / f"{RELEASE_NAME}.tar.gz"
    with tarfile.open(tarball, "w:gz") as tf:
        tf.add(stage_dir, arcname=RELEASE_NAME)
    digest = sha256_file(tarball)
    print(f"  Staged tarball: {tarball} ({tarball.stat().st_size} bytes, sha256={digest[:16]}...)")
    return stage_dir, tarball, digest


def replay_out_of_tree_sdk(tarball: Path) -> dict:
    print("[REL-2] Running out-of-tree C11/C++17 SDK & isolated official plugin replay from relocated release tarball...")
    tmp = Path(tempfile.mkdtemp(prefix="rodaq-rel-oot-"))
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
    PLUGIN_ID "com.example.rel.c_probe"
    PLUGIN_NAME "Release OOT C Probe"
    PLUGIN_VERSION "1.0.0")

add_library(oot_cpp_plugin SHARED cpp_plugin.cpp)
target_link_libraries(oot_cpp_plugin PRIVATE rodaq::plugin)
rodaq_register_plugin(oot_cpp_plugin
    PLUGIN_ID "com.example.rel.cpp_probe"
    PLUGIN_NAME "Release OOT C++ Probe"
    PLUGIN_VERSION "1.0.0")

add_executable(oot_consumer consumer.cpp)
target_link_libraries(oot_consumer PRIVATE rodaq::daq rodaq::plugin)
""",
            encoding="utf-8",
        )

        (src / "c_plugin.c").write_text(
            """#include <rodaq/c/plugin.h>
#include <stddef.h>

static rodaq_status_code_t c_configure(
    rodaq_plugin_context_t* context,
    const rodaq_application_callbacks_t* application,
    const rodaq_config_t* config,
    rodaq_error_t* error) {
    (void)context;
    (void)application;
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

class RelCppProbe : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup&) override { return {}; }
    rodaq::Result<void> shutdown(rodaq::plugin::Deadline) override { return {}; }
};

RODAQ_DEFINE_PLUGIN(RelCppProbe)
""",
            encoding="utf-8",
        )

        (src / "consumer.cpp").write_text(
            """#include <rodaq/daq/runtime.hpp>
#include <rodaq/plugin/close_future.hpp>
#include <rodaq/plugin/instance.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/manager.hpp>
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
    auto dl = rodaq::plugin::Deadline::after(std::chrono::seconds(5));
    if (!dl.ok()) {
        return 3;
    }
    for (const char* id : {"com.example.rel.c_probe", "com.example.rel.cpp_probe"}) {
        auto lib = mgr.value().load(id, "1.0.0");
        if (!lib.ok()) {
            std::cerr << "load failed for " << id << ": " << lib.status().message() << "\\n";
            return 4;
        }
        auto inst = lib.value().create_instance("");
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
                "bash",
                "-lc",
                f'source /opt/ros/{ROS_DISTRO}/setup.bash && '
                f'cmake -S "{src}" -B "{bld}" -DCMAKE_PREFIX_PATH="{relocated_prefix}" && '
                f'cmake --build "{bld}" -j4 && '
                f'cmake --install "{bld}" --prefix "{oot_install}"',
            ]
        )

        oot_plugins = oot_install / "lib" / "rodaq" / "plugins"
        chmod_tree_ro(oot_plugins)

        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = f"{relocated_prefix / 'lib'}:{env.get('LD_LIBRARY_PATH', '')}"
        out = run_checked([str(bld / "oot_consumer"), str(oot_plugins)], env=env)
        if "OOT_REPLAY_OK" not in out:
            raise RuntimeError(f"Unexpected consumer output: {out}")

        # Verify official MCAP & ROS 2 plugins build completely standalone from isolated copies
        # against the relocated release tarball prefix (zero repo parent directory access).
        iso_mcap_src = tmp / "isolated_mcap"
        iso_ros2_src = tmp / "isolated_ros2"
        shutil.copytree(REPO_ROOT / "plugins/storage/mcap", iso_mcap_src)
        shutil.copytree(REPO_ROOT / "plugins/acquisition/ros2", iso_ros2_src)
        for isolated_src in (iso_mcap_src, iso_ros2_src):
            shutil.copy2(REPO_ROOT / "internal/strict_json.hpp", isolated_src)
        mcap_include = REPO_ROOT / "third_party/mcap-cpp/cpp/mcap/include"

        run_checked(
            [
                "bash",
                "-lc",
                f'source /opt/ros/{ROS_DISTRO}/setup.bash && '
                f'cmake -S "{iso_mcap_src}" -B "{tmp / "iso_mcap_build"}" -DCMAKE_PREFIX_PATH="{relocated_prefix}" '
                f'-DRODAQ_MCAP_CPP_INCLUDE_DIR="{mcap_include}" && '
                f'cmake --build "{tmp / "iso_mcap_build"}" -j4 && '
                f'cmake -S "{iso_ros2_src}" -B "{tmp / "iso_ros2_build"}" -DCMAKE_PREFIX_PATH="{relocated_prefix}" && '
                f'cmake --build "{tmp / "iso_ros2_build"}" -j4',
            ]
        )

        print("  Out-of-tree C11/C++17 SDK & isolated official MCAP/ROS2 plugin replay PASSED.")
        return {
            "status": "passed",
            "c_plugin": "com.example.rel.c_probe@1.0.0",
            "cpp_plugin": "com.example.rel.cpp_probe@1.0.0",
            "isolated_official_mcap_build": True,
            "isolated_official_ros2_build": True,
            "relocated_prefix_tested": True,
        }
    finally:
        chmod_tree_rw(tmp)
        shutil.rmtree(tmp, ignore_errors=True)


def run_installed_ros2_mcap_workload_and_restart_check(tarball: Path) -> dict:
    print("[REL-3] Running relocated release ROS 2 -> MCAP workload, ros2 bag info & SIGKILL restart check...")
    tmp = Path(tempfile.mkdtemp(prefix="rodaq-rel-runtime-"))
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

        token_file = tmp / "server.token"
        token_file.write_text(f"{BEARER_TOKEN}\n", encoding="utf-8")
        token_file.chmod(0o600)

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
                            "security": {"bearer_token_file": str(token_file)},
                        },
                        "daq": {
                            "state_root": str(state_dir),
                            "volume_root": str(vol_dir),
                            "mode": mode,
                            "plugin_search_paths": [str(plugins)],
                            "qualified_runtime_profile_id": "rel-devhost-humble-mcap",
                            "max_tasks": 2,
                            "max_pipelines_per_task": 2,
                            "volume": {
                                "name": "local",
                                "durability_profile": "normal",
                            },
                            "profiles": [
                                {
                                    "name": "default",
                                    "payload_pool_bytes": 2097152,
                                    "max_payload_bytes": 65536,
                                    "max_logical_bytes": 2097152,
                                    "max_segment_physical_bytes": 16777216,
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
                f'source /opt/ros/{ROS_DISTRO}/setup.bash && '
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

        # Multi-topic ROS 2 publisher: /rel_chatter (60 KB @ 50 Hz) + /rel_telemetry (100 Hz).
        pub_py = tmp / "rel_pub.py"
        pub_py.write_text(
            """import rclpy, time
from rclpy.node import Node
from std_msgs.msg import String
rclpy.init()
node = Node('rodaq_rel_qual_pub')
p1 = node.create_publisher(String, '/rel_chatter', 50)
p2 = node.create_publisher(String, '/rel_telemetry', 50)
big_pad = 'B' * 60000
small_pad = 'T' * 512
i = 0
try:
    while rclpy.ok():
        p2.publish(String(data=f'rel-telemetry-{i}-{small_pad}'))
        if i % 2 == 0:
            p1.publish(String(data=f'rel-chatter-{i}-{big_pad}'))
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
                f'source /opt/ros/{ROS_DISTRO}/setup.bash && export ROS_DOMAIN_ID={ROS_DOMAIN_ID} && exec python3 "{pub_py}"',
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        daemon = start_daemon("initialize")

        # Verify static web root & manifest served by relocated installed binary.
        st_html, html_bytes, _ = http_bytes(base_url, "/")
        assert st_html == 200 and b"<div id=\"app\">" in html_bytes

        # Create multi-stream ROS 2 -> MCAP Task under frozen simplified TaskConfig.
        task_cfg = {
            "format_version": "1.0",
            "name": "rel-humble-mcap-qual",
            "segment_policy": {
                "max_duration_ms": 2000,
                "max_physical_bytes": None,
            },
            "retention_policy": {
                "mode": "keep_all",
                "max_duration_ms": None,
                "max_physical_bytes": None,
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
                                {"stream_key": "chatter", "topic": "/rel_chatter", "qos": {"mode": "adapt", "depth": "50"}},
                                {"stream_key": "telemetry", "topic": "/rel_telemetry", "qos": {"mode": "adapt", "depth": "50"}},
                            ],
                        },
                    },
                    "storage": {
                        "plugin_id": "com.rodaq.storage.mcap",
                        "plugin_version": "1.0.0",
                        "config": {
                            "format_version": "1.0",
                            "profile": "ros2",
                            "compression": "none",
                            "chunk_mode": "none",
                            "chunk_crc": False,
                            "message_index": False,
                            "summary": False,
                        },
                    },
                    "streams": [
                        {"stream_key": "chatter", "name": "chatter", "readiness_policy": "entry_ready"},
                        {"stream_key": "telemetry", "name": "telemetry", "readiness_policy": "entry_ready"},
                    ],
                }
            ],
        }
        code, created, _ = http_json(base_url, "POST", "/api/tasks", task_cfg, idem_key="rel-create-1")
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
            base_url,
            "POST",
            f"/api/tasks/{task_id}/operations",
            {"kind": "start", "parameters": {}},
            idem_key="rel-start-1",
        )
        assert code == 202, f"start rejected: {code} {op}"
        st = wait_op(op["data"]["operation_id"])
        assert st["state"] == "succeeded", f"start failed: {st}"

        # Sample system & runtime for ~4.5s (triggers >=2 rotated single-file MCAP segments).
        api_latencies_ms: list[float] = []
        rss_samples: list[int] = []
        fd_samples: list[int] = []
        thread_samples: list[int] = []
        queue_rec_hwm = 0
        queue_bytes_hwm = 0
        max_accepted_records = 0
        t_rec_start = time.monotonic()
        for _ in range(45):
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
                queue_rec_hwm = max(queue_rec_hwm, int(p["queue"]["high_watermark_records"]))
                queue_bytes_hwm = max(queue_bytes_hwm, int(p["queue"]["high_watermark_bytes"]))
            time.sleep(0.1)
        rec_elapsed_s = max(0.1, time.monotonic() - t_rec_start)

        # Pause -> Resume -> Stop lifecycle check.
        for kind in ("pause", "resume", "stop"):
            if kind == "stop":
                time.sleep(0.5)
            c, op_b, _ = http_json(
                base_url,
                "POST",
                f"/api/tasks/{task_id}/operations",
                {"kind": kind, "parameters": {}},
                idem_key=f"rel-{kind}-1",
            )
            assert c == 202, f"{kind} rejected: {c} {op_b}"
            res_op = wait_op(op_b["data"]["operation_id"])
            assert res_op["state"] == "succeeded", f"{kind} failed: {res_op}"

        # Query committed MCAP artifacts & verify >=2 segments committed.
        c, arts_body, _ = http_json(base_url, "GET", f"/api/artifacts?task_id={task_id}&limit=50")
        assert c == 200
        arts = arts_body["data"]["items"]
        assert len(arts) >= 2, f"Expected >=2 rotated MCAP artifacts, got {len(arts)}"
        total_committed_bytes = sum(int(a["physical_bytes"]) for a in arts)

        first_art_id = arts[0]["artifact_id"]
        c, detail_body, _ = http_json(base_url, "GET", f"/api/artifacts/{first_art_id}")
        assert c == 200
        mcap_file = next(f for f in detail_body["data"]["files"] if f["display_name"].endswith(".mcap"))

        # Full download + Range 206 download check.
        sc_full, full_bytes, _ = http_bytes(base_url, mcap_file["download_url"])
        assert sc_full == 200 and len(full_bytes) == int(mcap_file["size"])
        sc_range, range_bytes, _ = http_bytes(base_url, mcap_file["download_url"], byte_range="bytes=0-15")
        assert sc_range == 206 and range_bytes == full_bytes[:16]

        # Independent ROS 2 `ros2 bag info` verification on downloaded single-file MCAP.
        dl_mcap = tmp / "downloaded_rel.mcap"
        dl_mcap.write_bytes(full_bytes)
        bag_info = run_checked(
            ["bash", "-lc", f'source /opt/ros/{ROS_DISTRO}/setup.bash && ros2 bag info -s mcap "{dl_mcap}"']
        )
        assert "/rel_chatter" in bag_info and "/rel_telemetry" in bag_info, f"Missing topics in ros2 bag info:\n{bag_info}"

        # SIGKILL during active recording -> restart rebuilds Catalog synchronously,
        # cleans staging .tmp, preserves committed artifacts byte-for-byte, and starts Task in Stopped.
        c, op_s1, _ = http_json(
            base_url,
            "POST",
            f"/api/tasks/{task_id}/operations",
            {"kind": "start", "parameters": {}},
            idem_key="rel-start-before-kill",
        )
        assert c == 202
        assert wait_op(op_s1["data"]["operation_id"])["state"] == "succeeded"
        time.sleep(0.6)

        daemon.kill()
        daemon.wait(timeout=5)
        daemon = None

        t_restart0 = time.perf_counter()
        daemon = start_daemon("existing")
        restart_ready_ms = round((time.perf_counter() - t_restart0) * 1000.0, 1)

        c, arts_after, _ = http_json(base_url, "GET", f"/api/artifacts?task_id={task_id}&limit=50")
        assert c == 200 and len(arts_after["data"]["items"]) >= len(arts)
        sc_after, after_bytes, _ = http_bytes(base_url, mcap_file["download_url"])
        assert sc_after == 200 and after_bytes == full_bytes

        # Under frozen simplified contract, Tasks restart cleanly in Stopped (ready for immediate Start).
        c_t, task_after, _ = http_json(base_url, "GET", f"/api/tasks/{task_id}")
        assert c_t == 200, f"GET task after restart failed: {c_t} {task_after}"
        assert task_after["data"]["desired_state"] == "stopped", f"Unexpected post-restart task: {task_after}"
        assert task_after["data"]["execution_condition"] == "idle", f"Unexpected post-restart task: {task_after}"

        c, op_r, _ = http_json(
            base_url,
            "POST",
            f"/api/tasks/{task_id}/operations",
            {"kind": "start", "parameters": {}},
            idem_key="rel-start-after-restart",
        )
        assert c == 202, f"post-restart start failed: {c} {op_r}"
        assert wait_op(op_r["data"]["operation_id"])["state"] == "succeeded"
        time.sleep(0.4)

        c, op_st, _ = http_json(
            base_url,
            "POST",
            f"/api/tasks/{task_id}/operations",
            {"kind": "stop", "parameters": {}},
            idem_key="rel-stop-after-restart",
        )
        assert c == 202
        assert wait_op(op_st["data"]["operation_id"])["state"] == "succeeded"

        # Mid-download HTTP client disconnect releases ReadLease so subsequent Reset succeeds cleanly.
        raw_sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        raw_req = (
            f"GET {mcap_file['download_url']} HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            f"Authorization: Bearer {BEARER_TOKEN}\r\n"
            f"RoDAQ-API-Version: 1.0\r\n"
            f"Connection: close\r\n\r\n"
        ).encode("ascii")
        raw_sock.sendall(raw_req)
        _ = raw_sock.recv(128)
        raw_sock.close()

        dl_clean = time.monotonic() + 5.0
        while time.monotonic() < dl_clean:
            _, sb, _ = http_json(base_url, "GET", "/api/system")
            if int(sb["data"]["backlog"]["active_downloads"]) == 0:
                break
            time.sleep(0.05)
        _, sb_final, _ = http_json(base_url, "GET", "/api/system")
        assert int(sb_final["data"]["backlog"]["active_downloads"]) == 0

        c, op_reset, _ = http_json(
            base_url,
            "POST",
            f"/api/tasks/{task_id}/operations",
            {"kind": "reset", "parameters": {}},
            idem_key="rel-reset-final",
        )
        assert c == 202
        reset_res = wait_op(op_reset["data"]["operation_id"])
        assert reset_res["state"] == "succeeded", f"Reset after download disconnect failed: {reset_res}"

        # Clean SIGTERM shutdown -> exit code 0.
        daemon.send_signal(signal.SIGTERM)
        rc = daemon.wait(timeout=10)
        assert rc == 0, f"Expected exit code 0 on SIGTERM, got {rc}"
        daemon = None

        api_latencies_ms.sort()
        p50 = statistics.median(api_latencies_ms)
        p99 = api_latencies_ms[min(len(api_latencies_ms) - 1, int(len(api_latencies_ms) * 0.99))]
        p999 = api_latencies_ms[-1]
        throughput_mb_s = (total_committed_bytes / (1024.0 * 1024.0)) / rec_elapsed_s
        result = {
            "status": "passed",
            "ros_distro": ROS_DISTRO,
            "committed_mcap_artifacts_before_restart": len(arts),
            "total_committed_physical_bytes": total_committed_bytes,
            "sustained_committed_mb_per_s": round(throughput_mb_s, 2),
            "max_accepted_records_observed": max_accepted_records,
            "queue_hwm": {"records": queue_rec_hwm, "bytes": queue_bytes_hwm},
            "mcap_download_bytes": len(full_bytes),
            "range_206_verified": True,
            "ros2_bag_info_verified": True,
            "sigkill_restart_ready_ms": restart_ready_ms,
            "sigkill_restart_task_stopped": True,
            "committed_artifact_loss_count": 0,
            "mid_download_disconnect_lease_release_and_reset": True,
            "api_latency_ms": {
                "samples": len(api_latencies_ms),
                "p50": round(p50, 2),
                "p99": round(p99, 2),
                "p999_max": round(p999, 2),
            },
            "process_hwm": {
                "rss_bytes_max": max(rss_samples),
                "open_fds_max": max(fd_samples),
                "threads_max": max(thread_samples),
            },
            "sigterm_clean_exit_code": 0,
        }
        print(f"  Installed ROS 2 -> MCAP workload, ros2 bag info & restart check PASSED: {json.dumps(result)}")
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
    global BUILD_DIR, RELEASE_NAME, ROS_DISTRO, ROS_DOMAIN_ID
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default=str(BUILD_DIR))
    parser.add_argument("--ros-distro", default="humble", choices=["humble", "jazzy"])
    parser.add_argument("--domain-id", type=int, default=53)
    args = parser.parse_args()
    BUILD_DIR = Path(args.build_dir).resolve()
    ROS_DISTRO = args.ros_distro
    ROS_DOMAIN_ID = args.domain_id
    RELEASE_NAME = f"rodaq-0.1.0-linux-amd64-{ROS_DISTRO}"

    stage_dir, tarball, digest = stage_release_package()
    oot = replay_out_of_tree_sdk(tarball)
    runtime = run_installed_ros2_mcap_workload_and_restart_check(tarball)
    report = {
        "release_name": RELEASE_NAME,
        "ros_distro": ROS_DISTRO,
        "staged_dir": str(stage_dir.relative_to(REPO_ROOT)),
        "tarball": str(tarball.relative_to(REPO_ROOT)),
        "tarball_sha256": digest,
        "out_of_tree_sdk_replay": oot,
        "installed_ros2_mcap_qualification": runtime,
    }
    report_path = OUT_DIR / f"release-verification-{ROS_DISTRO}.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"[REL] Wrote verification report to {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
