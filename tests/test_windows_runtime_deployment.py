"""Portable deployment control-flow tests; real Windows DLL loading is a separate check."""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


CMAKE = shutil.which("cmake")
if __name__ == "__main__" and len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    CMAKE = sys.argv.pop(1)
ROOT = Path(__file__).resolve().parents[1]


def cmake_path(path):
    return str(path).replace("\\", "/")


def write_pe(path, machine=0x8664):
    payload = bytearray(0x88)
    payload[:2] = b"MZ"
    struct.pack_into("<I", payload, 0x3C, 0x80)
    payload[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", payload, 0x84, machine)
    path.write_bytes(payload)


@unittest.skipUnless(CMAKE, "CMake is required")
class WindowsRuntimeDeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="desktop pet deployment ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.exe = self.output / "test.exe"
        write_pe(self.exe)
        self.dependencies = self.root / "dependency bin"
        self.dependencies.mkdir()
        self.sqlite = self.root / "Qt plugins" / "sqldrivers" / "qsqlite.dll"
        self.sqlite.parent.mkdir(parents=True)
        self.sqlite.write_bytes(b"sqlite plugin")

    def run_script(self, *, config="Release", resolved=(), unresolved=(), extra=(), fail=False,
                   qt=True, deploy_sqlite=True):
        # Stub only platform-specific tools, retaining real locks, directory discovery,
        # copy commands and errors from the production deployment script.
        script = self.root / "test.cmake"
        calls = self.root / "calls.txt"
        script.write_text(f"""
cmake_minimum_required(VERSION 3.20)
set(DEPLOY_EXECUTABLE [==[{cmake_path(self.exe)}]==])
set(DEPLOY_CONFIG {config})
set(DEPLOY_QT {'ON' if qt else 'OFF'})
set(DEPLOY_WINDEPLOYQT fake-windeployqt)
set(DEPLOY_SEARCH_DIRS [==[{cmake_path(self.dependencies)};{cmake_path(self.root / 'missing')}]==])
set(DEPLOY_EXTRA_DLLS [==[{';'.join(map(cmake_path, extra))}]==])
set(ENV{{SystemRoot}} [==[{cmake_path(self.root / 'Windows')}]==])
function(execute_process)
    if(ARGV1 STREQUAL "fake-windeployqt")
        _file(APPEND [==[{cmake_path(calls)}]==] "${{ARGV}}\\n")
        set(_deploy_result {'1' if fail else '0'} PARENT_SCOPE)
        set(_deploy_error "fixture deployment error" PARENT_SCOPE)
        if(NOT {'TRUE' if fail else 'FALSE'} AND {'TRUE' if deploy_sqlite else 'FALSE'})
            _file(MAKE_DIRECTORY [==[{cmake_path(self.output / 'sqldrivers')}]==])
            _file(COPY [==[{cmake_path(self.sqlite)}]==]
                DESTINATION [==[{cmake_path(self.output / 'sqldrivers')}]==])
        endif()
    else()
        _execute_process(${{ARGV}})
        set(_copy_result "${{_copy_result}}" PARENT_SCOPE)
    endif()
endfunction()
function(file)
    if(ARGV0 STREQUAL "GET_RUNTIME_DEPENDENCIES")
        _file(APPEND [==[{cmake_path(calls)}]==] "${{ARGV}}\\n")
        set(_resolved [==[{';'.join(map(cmake_path, resolved))}]==] PARENT_SCOPE)
        set(_unresolved [==[{';'.join(unresolved)}]==] PARENT_SCOPE)
    else()
        _file(${{ARGV}})
        if(ARGV0 STREQUAL "TO_CMAKE_PATH")
            set(${{ARGV2}} "${{${{ARGV2}}}}" PARENT_SCOPE)
        elseif(ARGV0 STREQUAL "GLOB")
            set(${{ARGV1}} "${{${{ARGV1}}}}" PARENT_SCOPE)
        elseif(ARGV0 STREQUAL "READ" OR ARGV0 STREQUAL "SIZE")
            set(${{ARGV2}} "${{${{ARGV2}}}}" PARENT_SCOPE)
        endif()
    endif()
endfunction()
include([==[{cmake_path(ROOT / 'cmake/DeployWindowsRuntime.cmake')}]==])
""", encoding="utf-8")
        result = subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True)
        return result, calls.read_text() if calls.exists() else ""

    def test_configuration_and_compiler_runtime(self):
        for config in ("Debug", "Release", "RelWithDebInfo"):
            with self.subTest(config=config):
                result, calls = self.run_script(config=config)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("--debug", calls)
                self.assertNotIn("--release", calls)
                self.assertIn("--compiler-runtime", calls)
                self.assertNotIn("--skip-plugin-types", calls)
                self.assertIn("--dir;" + cmake_path(self.output), calls)
                self.assertNotIn("--no-system-d3d-compiler", calls)
                (self.root / "calls.txt").unlink()

    def test_copies_transitive_and_extra_dlls_but_not_system_libraries(self):
        runtime = self.dependencies / "libsodium.dll"
        runtime.write_bytes(b"runtime")
        provider = self.dependencies / "onnxruntime_providers_shared.dll"
        provider.write_bytes(b"provider")
        system = self.root / "Windows/System32/kernel32.dll"
        system.parent.mkdir(parents=True)
        system.touch()
        msvc = self.root / "Windows/System32/MSVCP140.dll"
        write_pe(msvc)
        result, _ = self.run_script(resolved=(runtime, system, msvc), extra=(provider,))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.output / runtime.name).read_bytes(), b"runtime")
        self.assertEqual((self.output / provider.name).read_bytes(), b"provider")
        self.assertEqual((self.output / msvc.name).read_bytes(), msvc.read_bytes())
        self.assertFalse((self.output / system.name).exists())

    def test_rejects_mismatched_msvc_runtime_architecture(self):
        msvc = self.root / "Windows/System32/VCRUNTIME140.dll"
        msvc.parent.mkdir(parents=True)
        write_pe(msvc, machine=0x014C)
        result, _ = self.run_script(resolved=(msvc,))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match executable architecture", result.stderr)

    def test_scans_required_qt_plugins(self):
        for name in ("platforms/qwindows.dll", "tls/qschannelbackend.dll",
                     "sqldrivers/qsqlmysql.dll", "sqldrivers/qsqlpsql.dll",
                     "tls/qopensslbackend.dll", "plugins/platforms/qwindows.dll"):
            plugin = self.output / name
            plugin.parent.mkdir(parents=True, exist_ok=True)
            plugin.touch()
        result, calls = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("qwindows.dll", "qsqlite.dll", "qschannelbackend.dll"):
            self.assertIn(name, calls)
        self.assertEqual((self.output / "sqldrivers/qsqlite.dll").read_bytes(),
                         b"sqlite plugin")
        self.assertFalse((self.output / "sqldrivers/qsqlpsql.dll").exists())
        self.assertFalse((self.output / "tls/qopensslbackend.dll").exists())
        self.assertFalse((self.output / "plugins/platforms").exists())
        self.assertNotIn("qsqlmysql.dll", calls)

    def test_qt_target_without_sql_preserves_shared_sqlite(self):
        result, _ = self.run_script(deploy_sqlite=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.output / "sqldrivers").exists())

        (self.root / "calls.txt").unlink()
        sqlite = self.output / "sqldrivers/qsqlite.dll"
        sqlite.parent.mkdir()
        sqlite.write_bytes(b"shared sqlite plugin")
        (sqlite.parent / "qsqlpsql.dll").touch()
        result, _ = self.run_script(deploy_sqlite=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sqlite.read_bytes(), b"shared sqlite plugin")
        self.assertFalse((sqlite.parent / "qsqlpsql.dll").exists())

    def test_missing_dependency_fails_actionably(self):
        result, _ = self.run_script(unresolved=("libstdc++-6.dll",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libstdc++-6.dll", result.stderr)
        self.assertIn("DESKTOP_PET_RUNTIME_SEARCH_DIRS", result.stderr)

    def test_failed_windeployqt_is_not_silently_ignored(self):
        result, calls = self.run_script(fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fixture deployment error", result.stderr)
        self.assertNotIn("GET_RUNTIME_DEPENDENCIES", calls)

    def test_missing_extra_dll_fails(self):
        result, _ = self.run_script(extra=(self.dependencies / "missing.dll",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Failed to deploy", result.stderr)

    def test_missing_executable_fails(self):
        self.exe.unlink()
        result, _ = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Cannot deploy missing executable", result.stderr)

    def test_non_qt_fixture_still_deploys_compiler_runtime(self):
        runtime = self.dependencies / "libstdc++-6.dll"
        runtime.write_bytes(b"compiler runtime")
        result, calls = self.run_script(qt=False, resolved=(runtime,))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("fake-windeployqt", calls)
        self.assertIn("GET_RUNTIME_DEPENDENCIES", calls)
        self.assertTrue((self.output / runtime.name).exists())

    def configure_fixture(self, config="Release", enabled=True, tool_exists=True):
        qtbin = self.root / "Qt kit/bin"
        qtbin.mkdir(parents=True, exist_ok=True)
        deploy = qtbin / "windeployqt.exe"
        if tool_exists:
            deploy.touch()
        (self.root / "main.cpp").write_text("int main() { return 0; }\n")
        (self.root / "toolchain.cmake").write_text("# Fixture toolchain.\n")
        (self.root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.20)
project(RuntimeFixture LANGUAGES CXX)
set(WIN32 TRUE)
set(MSVC FALSE)
set(CMAKE_OBJDUMP [==[{cmake_path(CMAKE)}]==])
set(DESKTOP_PET_WINDEPLOYQT [==[{cmake_path(deploy)}]==])
set(DESKTOP_PET_DEPLOY_WINDOWS_RUNTIME {'ON' if enabled else 'OFF'})
add_library(Qt6::Core SHARED IMPORTED)
set_target_properties(Qt6::Core PROPERTIES IMPORTED_LOCATION [==[{cmake_path(qtbin / 'Qt6Core.dll')}]==])
add_executable(pet main.cpp)
add_executable(last_test main.cpp)
add_executable(native_fixture main.cpp)
set_target_properties(native_fixture PROPERTIES DESKTOP_PET_DEPLOY_QT OFF)
include([==[{cmake_path(ROOT / 'cmake/WindowsRuntime.cmake')}]==])
""", encoding="utf-8")
        build = self.root / ("build-" + config)
        command = [CMAKE, "-S", str(self.root), "-B", str(build),
                   "-DCMAKE_BUILD_TYPE=" + config,
                   "-DCMAKE_TOOLCHAIN_FILE=" + str(self.root / "toolchain.cmake")]
        # A MinGW/Ninja developer need not also have CMake's default VS generator.
        generator = os.environ.get("DESKTOP_PET_TEST_CMAKE_GENERATOR")
        if generator:
            command += ["-G", generator]
        for name in ("MAKE_PROGRAM", "CXX_COMPILER"):
            value = os.environ.get("DESKTOP_PET_TEST_" + name)
            if value:
                command.append("-DCMAKE_" + name + "=" + value)
        result = subprocess.run(command, capture_output=True, text=True)
        return result, build

    def test_all_targets_are_registered_even_with_a_toolchain(self):
        for config in ("Debug", "Release"):
            with self.subTest(config=config):
                result, build = self.configure_fixture(config=config)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for target in ("pet", "last_test"):
                    script = build / "runtime-deploy" / (target + "-" + config + ".cmake")
                    content = script.read_text()
                    self.assertIn("set(DEPLOY_CONFIG [==[" + config + "]==])", content)
                    self.assertIn("Qt kit/bin", content)
                    self.assertIn("set(DEPLOY_QT ON)", content)
                native_script = build / "runtime-deploy" / ("native_fixture-" + config + ".cmake")
                self.assertIn("set(DEPLOY_QT OFF)", native_script.read_text())
                self.assertIn("_deploy_runtime", (build / "CMakeFiles/TargetDirectories.txt").read_text())

    def test_explicit_opt_out(self):
        result, build = self.configure_fixture(enabled=False, tool_exists=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((build / "runtime-deploy").exists())

    def test_missing_windeployqt_fails_during_configuration(self):
        result, _ = self.configure_fixture(tool_exists=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("windeployqt was not found", result.stderr)


if __name__ == "__main__":
    unittest.main()
