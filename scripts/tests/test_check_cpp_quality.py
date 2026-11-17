"""Regression tests for the incremental C++ quality checks."""

import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location(
    "check_cpp_quality", Path(__file__).parents[1] / "check_cpp_quality.py")
quality = importlib.util.module_from_spec(spec)
spec.loader.exec_module(quality)


class SourceChecks(unittest.TestCase):
    def test_inline_block_comment_cannot_trigger_rules(self):
        source = [
            "int x; /* open",
            "using namespace std;",
            "#include <vulkan/vulkan.h>",
            '#include "../../rhi/src/Private.h"',
            "vkQueueWaitIdle(queue);",
            "*/ using namespace std;",
        ]
        findings = list(quality.check_source("engine/core/include/core/API.h", source, {2, 3, 4, 5, 6}))
        self.assertEqual(findings, [(6, "error", "avoid using namespace in a header")])

    def test_literals_cannot_trigger_or_hide_call(self):
        source = [
            'const char* raw = R"example(',
            "#include <vulkan/vulkan.h>",
            "vkDeviceWaitIdle(device);",
            ')example";',
            'log("https://example.com/vkDeviceWaitIdle("); vkQueueWaitIdle(queue);',
            'log("/* this is a string */"); // vkDeviceWaitIdle(device);',
            "#include /* comment */ <vulkan/vulkan.h>",
        ]
        findings = list(quality.check_source("engine/core/src/Example.cpp", source, {2, 3, 5, 6, 7}))
        self.assertEqual([(line, severity) for line, severity, _ in findings], [(5, "notice"), (7, "error")])
