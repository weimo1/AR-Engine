"""Command-table verification tests: schema, parsers and the runner.

The fixtures are handwritten tables, logs and JUnit files: they assert that a wrong exit code,
a wrong result category, a missing case, an outdated identity, a timeout or a crash is refused
instead of being reported as a pass.
"""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ElementTree

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow_p0 as p0

CHANGE = "bootstrap-engine-p0"


def make_command(**overrides):
    command = {
        "id": "echo",
        "argv": [sys.executable, "-c", "print('hi')"],
        "cwd": "${REPO}",
        "timeout_seconds": 60,
        "wrapper_exit": None,
        "child_exit": None,
        "parser_id": "msbuild-log",
        "required": True,
        "output_dir": "${OUT}/logs/p0-checks",
        "expected_capability_source": None,
        "depends_on": [],
    }
    command.update(overrides)
    return command


def make_table(commands):
    return {"schema_version": 1, "change": CHANGE, "output_root": "${OUT}",
            "snapshot": "${REPO}", "commands": commands}


def parse(parser_id, command, argv, log, root):
    return p0.PARSER_FUNCTIONS[parser_id](command, argv, log, Path(root))


class TableSchemaTests(unittest.TestCase):
    def test_minimal_table_is_accepted(self):
        table = make_table([make_command()])
        p0.validate_table(table)
        self.assertEqual(table["change"], CHANGE)

    def test_rejects_unknown_table_field(self):
        table = make_table([make_command()])
        table["extra"] = 1
        with self.assertRaises(p0.TableError):
            p0.validate_table(table)

    def test_rejects_wrong_schema_version(self):
        table = make_table([make_command()])
        table["schema_version"] = 2
        with self.assertRaises(p0.TableError):
            p0.validate_table(table)

    def test_rejects_empty_command_list(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([]))

    def test_rejects_shell_string_argv(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(argv="python -c print(1)")]))

    def test_rejects_declared_exit_codes(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(wrapper_exit=0)]))
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(child_exit=0)]))

    def test_rejects_unknown_placeholder(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(argv=["python", "${HOME}/x.py"])]))

    def test_rejects_unknown_parser(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(parser_id="whatever")]))

    def test_rejects_bad_timeout(self):
        for timeout in (0, -1, 60 * 60 * 24):
            with self.assertRaises(p0.TableError):
                p0.validate_table(make_table([make_command(timeout_seconds=timeout)]))

    def test_rejects_duplicate_ids(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(), make_command()]))

    def test_rejects_self_dependency(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(depends_on=["echo"])]))

    def test_rejects_forward_dependency(self):
        first = make_command(id="a", depends_on=["b"])
        second = make_command(id="b")
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([first, second]))

    def test_rejects_unknown_dependency(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(depends_on=["missing"])]))

    def test_ctest_requires_expected_cases(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(id="t", parser_id="ctest-junit")]))
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(id="t", parser_id="ctest-junit",
                                                      expected_cases=["a", "a"])]))

    def test_expected_cases_only_for_ctest(self):
        with self.assertRaises(p0.TableError):
            p0.validate_table(make_table([make_command(expected_cases=["a"])]))

    def test_missing_table_file_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(p0.TableError):
                p0.load_table(Path(directory) / "absent.json")

    def test_repository_table_is_valid(self):
        table = p0.load_table(Path(__file__).parents[2] / "workflow/p0-checks.json")
        self.assertEqual(table["change"], CHANGE)
        ctest = [command for command in table["commands"] if command["parser_id"] == "ctest-junit"]
        self.assertEqual(len(ctest), 2)
        for command in ctest:
            self.assertIn("build-identity", command["expected_cases"])
            self.assertIn("public-headers", command["expected_cases"])


class ClParserTests(unittest.TestCase):
    DEBUG_LOG = ("cl.exe /c /MDd /std:c++23preview /EHsc /permissive- x.cpp\n"
                 "cl.exe /c /MDd /std:c++23preview /EHsc /permissive- y.cpp\n")

    def test_accepts_a_debug_command_line(self):
        detail = parse("msbuild-cl", make_command(), ["cmake", "--build", "b", "--config", "Debug"],
                       self.DEBUG_LOG, ".")
        self.assertEqual(detail["crt"], "/MDd")
        self.assertEqual(detail["counts"]["preview"], 2)
        self.assertEqual(detail["counts"]["latest"], 0)

    def test_rejects_latest(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"],
                  self.DEBUG_LOG + "cl.exe /c /std:c++latest /EHsc\n", ".")

    def test_rejects_missing_preview(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"], "cl.exe /c /EHsc\n", ".")

    def test_rejects_missing_ehsc(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"],
                  "cl.exe /c /MDd /std:c++23preview\n", ".")

    def test_rejects_c4530(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"],
                  self.DEBUG_LOG + "x.cpp: warning C4530: no unwind semantics\n", ".")

    def test_rejects_static_runtime(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"],
                  "cl.exe /c /MT /std:c++23preview /EHsc\n", ".")

    def test_rejects_wrong_crt_for_configuration(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "RelWithDebInfo"], self.DEBUG_LOG, ".")

    def test_rejects_compiler_warning(self):
        with self.assertRaises(ValueError):
            parse("msbuild-cl", make_command(), ["x", "--config", "Debug"],
                  self.DEBUG_LOG + "z.cpp(3): warning C4996: deprecated\n", ".")


class CtestParserTests(unittest.TestCase):
    def _junit(self, directory, cases):
        path = Path(directory) / "ctest.xml"
        suite = ElementTree.Element("testsuite")
        for name, outcome in cases:
            case = ElementTree.SubElement(suite, "testcase", {"name": name})
            if outcome != "passed":
                ElementTree.SubElement(case, outcome)
        ElementTree.ElementTree(suite).write(path, encoding="utf-8")
        return path

    def test_accepts_all_expected_passing(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._junit(directory, [("a", "passed"), ("b", "passed")])
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a", "b"])
            detail = parse("ctest-junit", command,
                           ["ctest", "--output-junit", str(path)], "", directory)
            self.assertEqual(detail["cases"], {"a": "passed", "b": "passed"})

    def test_rejects_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._junit(directory, [("a", "failure")])
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a"])
            with self.assertRaises(ValueError):
                parse("ctest-junit", command, ["ctest", "--output-junit", str(path)], "", directory)

    def test_rejects_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._junit(directory, [("a", "skipped")])
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a"])
            with self.assertRaises(ValueError):
                parse("ctest-junit", command, ["ctest", "--output-junit", str(path)], "", directory)

    def test_rejects_missing_case(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._junit(directory, [("a", "passed")])
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a", "b"])
            with self.assertRaises(ValueError):
                parse("ctest-junit", command, ["ctest", "--output-junit", str(path)], "", directory)

    def test_rejects_undeclared_case(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._junit(directory, [("a", "passed"), ("surprise", "passed")])
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a"])
            with self.assertRaises(ValueError):
                parse("ctest-junit", command, ["ctest", "--output-junit", str(path)], "", directory)

    def test_rejects_missing_junit(self):
        with tempfile.TemporaryDirectory() as directory:
            command = make_command(id="t", parser_id="ctest-junit", expected_cases=["a"])
            with self.assertRaises(ValueError):
                parse("ctest-junit", command,
                      ["ctest", "--output-junit", str(Path(directory) / "absent.xml")], "", directory)


class LogParserTests(unittest.TestCase):
    def test_configure_accepts_a_real_log(self):
        log = ("-- ar: C++23 language mode verified (19.51.36260.0)\n"
               "-- ar: compiled build identity abc (Debug) from x\n-- Configuring done (3.2s)\n")
        detail = parse("cmake-configure", make_command(), ["cmake", "-DAR_EXPECTED_CONFIGURATION=Debug"],
                       log, ".")
        self.assertEqual(detail["configuration"], "Debug")

    def test_configure_rejects_cmake_error(self):
        log = "-- Configuring done\nCMake Error at CMakeLists.txt:3 (message):\n  boom\n"
        with self.assertRaises(ValueError):
            parse("cmake-configure", make_command(), ["cmake", "-DAR_EXPECTED_CONFIGURATION=Debug"], log, ".")

    def test_configure_requires_the_c23_marker(self):
        with self.assertRaises(ValueError):
            parse("cmake-configure", make_command(), ["cmake", "-DAR_EXPECTED_CONFIGURATION=Debug"],
                  "-- Configuring done\n", ".")

    def test_msbuild_log_rejects_compiler_error(self):
        with self.assertRaises(ValueError):
            parse("msbuild-log", make_command(), ["cmake"], "x.cpp(2): error C2039: nope\n", ".")

    def test_msbuild_log_accepts_a_clean_log(self):
        self.assertEqual(parse("msbuild-log", make_command(), ["cmake"], "Build succeeded.\n", "."), {})


class RecordParserTests(unittest.TestCase):
    def _toolchain(self, directory, **overrides):
        record = {
            "schema_version": 1, "vs_instance": "VS 18", "toolset": "14.51.36231",
            "cl_version": "19.51.36260", "cmake_version": "4.2.1",
            "generator": "Visual Studio 18 2026", "windows_sdk": "10.0.26100.0",
            "host_arch": "x64", "target_arch": "x64", "triplet": "x64-windows-static-md",
            "triplet_file_sha256": "a" * 64, "crt": {"debug": "/MDd", "optimized": "/MD"},
            "vcpkg_tool_commit": "b" * 40,
        }
        record.update(overrides)
        path = Path(directory) / "toolchain.json"
        path.write_text(json.dumps(record), encoding="utf-8")
        return path

    def test_toolchain_accepts_the_record(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._toolchain(directory)
            detail = parse("json-toolchain", make_command(),
                           ["python", "x.py", "--output", str(path), "--triplet", "x64-windows-static-md"], "", directory)
            self.assertEqual(detail["triplet"], "x64-windows-static-md")

    def test_toolchain_rejects_wrong_triplet(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._toolchain(directory)
            with self.assertRaises(ValueError):
                parse("json-toolchain", make_command(),
                      ["python", "x.py", "--output", str(path), "--triplet", "x64-windows"], "", directory)

    def test_toolchain_rejects_static_crt_record(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._toolchain(directory, crt={"debug": "/MTd", "optimized": "/MT"})
            with self.assertRaises(ValueError):
                parse("json-toolchain", make_command(), ["python", "x.py", "--output", str(path)], "", directory)

    def test_dependencies_reject_a_changed_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "vcpkg.json").write_text('{"name":"x"}\n', encoding="utf-8")
            record = {
                "schema_version": 1, "manifest_sha256": "c" * 64, "triplet": "x64-windows-static-md",
                "vcpkg_tool_commit": "b" * 40, "registry_baseline": "d" * 40,
                "resolved": [{"name": "spdlog", "version": "1.17.0", "port_version": 0,
                              "triplet": "x64-windows-static-md", "features": []}],
            }
            path = root / "dependencies.json"
            path.write_text(json.dumps(record), encoding="utf-8")
            with self.assertRaises(ValueError):
                parse("json-dependencies", make_command(),
                      ["python", "x.py", "--snapshot", str(root), "--output", str(path)], "", root)

    def test_dependencies_accept_a_matching_manifest(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "vcpkg.json"
            manifest.write_text('{"name":"x"}\n', encoding="utf-8")
            record = {
                "schema_version": 1,
                "manifest_sha256": hashlib.sha256(manifest.read_bytes()).hexdigest(),
                "triplet": "x64-windows-static-md", "vcpkg_tool_commit": "b" * 40,
                "registry_baseline": "d" * 40,
                "resolved": [{"name": "spdlog", "version": "1.17.0", "port_version": 0,
                              "triplet": "x64-windows-static-md", "features": []}],
            }
            path = root / "dependencies.json"
            path.write_text(json.dumps(record), encoding="utf-8")
            detail = parse("json-dependencies", make_command(),
                           ["python", "x.py", "--snapshot", str(root), "--output", str(path)], "", root)
            self.assertEqual(detail["ports"], ["spdlog"])


class IdentityParserTests(unittest.TestCase):
    def _sidecar(self, root, configuration="Debug", dependencies=("REPO", "a" * 40, True)):
        identity = root / "identity" / configuration
        identity.mkdir(parents=True, exist_ok=True)
        sidecar = {
            "schema_version": 1, "build_id": "1" * 64, "input_hash": "2" * 64,
            "configuration": configuration, "toolchain": {"triplet": "x64-windows-static-md"},
            "dependency_manifest_hash": "3" * 64,
            "source": {"revision": dependencies[0], "dirty": dependencies[2]},
        }
        path = identity / "build-identity.json"
        path.write_text(json.dumps(sidecar), encoding="utf-8")
        return path

    def test_prepare_rejects_a_dependency_record_it_did_not_produce(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._sidecar(root)
            dependencies = root / "dependencies.json"
            dependencies.write_text(json.dumps({"resolved": []}), encoding="utf-8")
            with self.assertRaises(ValueError):
                parse("json-identity", make_command(),
                      ["python", "x.py", "prepare", "--output", str(root),
                       "--configuration", "Debug", "--dependency-record", str(dependencies)], "", root)

    def test_prepare_accepts_the_matching_dependency_record(self):
        import hashlib
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dependencies = root / "dependencies.json"
            dependencies.write_text(json.dumps({"resolved": []}), encoding="utf-8")
            expected = hashlib.sha256(p0.canonical_bytes({"resolved": []})).hexdigest()
            sidecar = self._sidecar(root)
            data = json.loads(sidecar.read_text(encoding="utf-8"))
            data["dependency_manifest_hash"] = expected
            sidecar.write_text(json.dumps(data), encoding="utf-8")
            detail = parse("json-identity", make_command(),
                           ["python", "x.py", "prepare", "--output", str(root),
                            "--configuration", "Debug", "--dependency-record", str(dependencies)], "", root)
            self.assertEqual(detail["mode"], "prepare")

    def test_check_requires_the_ok_line(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._sidecar(root)
            with self.assertRaises(ValueError):
                parse("json-identity", make_command(),
                      ["python", "x.py", "check", "--output", str(root), "--configuration", "Debug"],
                      "identity check: mismatch\n", root)

    def test_check_accepts_matching_build_id(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._sidecar(root)
            detail = parse("json-identity", make_command(),
                           ["python", "x.py", "check", "--output", str(root), "--configuration", "Debug"],
                           f"identity check: ok ({'1' * 64})\n", root)
            self.assertEqual(detail["build_id"], "1" * 64)

    def test_check_accepts_crlf_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self._sidecar(root)
            detail = parse("json-identity", make_command(),
                           ["python", "x.py", "check", "--output", str(root), "--configuration", "Debug"],
                           f"identity check: ok ({'1' * 64})\r\n", root)
            self.assertEqual(detail["mode"], "check")


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name) / "repo"
        self.root.mkdir()
        self.output = Path(self._temporary.name) / "out"
        self.environment = {"VCPKG_ROOT": "C:/vcpkg", "AR_P0_DEPS": "C:/deps"}

    def tearDown(self):
        self._temporary.cleanup()

    def test_runs_a_table_and_records_real_exits(self):
        table = make_table([make_command()])
        results = p0.run_table(table, self.root, self.output, self.environment)
        self.assertEqual(results["status"], "passed")
        entry = results["commands"][0]
        self.assertEqual(entry["child_exit"], 0)
        self.assertTrue(entry["passed"])
        self.assertTrue(Path(results["results_path"]).is_file())
        self.assertIn("hi", Path(entry["log"]).read_text(encoding="utf-8"))

    def test_a_required_failure_stops_the_run(self):
        failing = make_command(id="boom", argv=[sys.executable, "-c", "raise SystemExit(3)"])
        table = make_table([failing])
        with self.assertRaises(ValueError):
            p0.run_table(table, self.root, self.output, self.environment)
        results = json.loads((self.output / "logs/p0-checks/results.json").read_text(encoding="utf-8"))
        self.assertEqual(results["status"], "failed")
        self.assertEqual(results["commands"][0]["child_exit"], 3)

    def test_optional_failure_is_recorded_and_skipped(self):
        optional = make_command(id="soft", required=False,
                                argv=[sys.executable, "-c", "raise SystemExit(1)"])
        table = make_table([optional])
        results = p0.run_table(table, self.root, self.output, self.environment)
        self.assertEqual(results["status"], "passed")
        self.assertFalse(results["commands"][0]["passed"])

    def test_timeout_is_refused(self):
        slow = make_command(id="slow", timeout_seconds=1,
                            argv=[sys.executable, "-c", "import time; time.sleep(30)"])
        with self.assertRaises(ValueError):
            p0.run_table(make_table([slow]), self.root, self.output, self.environment)

    def test_output_does_not_pollute_the_snapshot(self):
        table = make_table([make_command()])
        before = sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))
        p0.run_table(table, self.root, self.output, self.environment)
        after = sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))
        self.assertEqual(before, after)

    def test_missing_environment_blocks(self):
        with self.assertRaises(p0.TableError):
            p0.run_table(make_table([make_command()]), self.root, self.output, {})

    def test_only_selects_named_commands(self):
        first = make_command(id="a")
        second = make_command(id="b", depends_on=["a"])
        results = p0.run_table(make_table([first, second]), self.root, self.output,
                               self.environment, only=["b"])
        self.assertEqual([entry["id"] for entry in results["commands"]], ["b"])


if __name__ == "__main__":
    unittest.main()
