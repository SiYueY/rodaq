"""Regression cases for ABI-ST/ABI-IF compatibility rejection rules."""

import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from compare_abi_manifest import compare


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.old = {
            "format_version": 1, "architecture": "amd64", "symbols": ["entry"],
            "abi": {"major": 1, "minor": 0}, "constants": {"OK": 0},
            "types": {
                "value": {"class": "FROZEN_VALUE", "sizeof": 4, "alignof": 4},
                "table": {
                    "class": "EXTENSIBLE_STRUCT", "sizeof": 16, "alignof": 8,
                    "required_prefix_size": 16,
                    "fields": {"slot": {"offset": 8, "sizeof": 8, "required": True}},
                },
            },
        }
        self.new = copy.deepcopy(self.old)

    def test_identical_layout_accepts_other_compiler_metadata(self):
        self.new["compiler"] = "another compiler"
        self.assertEqual(compare(self.old, self.new), [])

    def test_optional_tail_is_compatible(self):
        self.new["types"]["table"]["sizeof"] = 24
        self.new["types"]["table"]["fields"]["tail"] = {
            "offset": 16, "sizeof": 8, "required": False, "introduced_minor": 1,
        }
        self.assertEqual(compare(self.old, self.new), [])

    def test_declared_interface_minor_can_advance_but_cannot_regress_or_hide_major_changes(self):
        self.old["constants"].update(RODAQ_ACQUISITION_SERVICE_MAJOR=1,
                                     RODAQ_ACQUISITION_SERVICE_MINOR=0)
        self.new = copy.deepcopy(self.old)
        self.new["constants"]["RODAQ_ACQUISITION_SERVICE_MINOR"] = 1
        self.assertEqual(compare(self.old, self.new), [])
        self.new["constants"]["RODAQ_ACQUISITION_SERVICE_MAJOR"] = 2
        self.assertTrue(compare(self.old, self.new))
        self.new = copy.deepcopy(self.old)
        self.old["constants"]["RODAQ_ACQUISITION_SERVICE_MINOR"] = 1
        self.assertTrue(compare(self.old, self.new))
        self.new["constants"].pop("RODAQ_ACQUISITION_SERVICE_MINOR")
        self.assertTrue(compare(self.old, self.new))

    def test_unrelated_minor_named_constants_are_still_frozen(self):
        self.old["constants"]["ERROR_MINOR"] = 1
        self.new = copy.deepcopy(self.old)
        self.new["constants"]["ERROR_MINOR"] = 2
        self.assertTrue(compare(self.old, self.new))

    def test_each_forbidden_change_is_rejected(self):
        mutations = [
            lambda item: item["abi"].update(revision=1),
            lambda item: item["constants"].update(OK=1),
            lambda item: item["types"]["value"].update(sizeof=8),
            lambda item: item["types"]["table"].update(required_prefix_size=24),
            lambda item: item["types"]["table"]["fields"]["slot"].update(offset=0),
            lambda item: item["types"]["table"]["fields"]["slot"].update(signature="changed"),
            lambda item: item.update(architecture="arm64"),
            lambda item: item["types"].pop("value"),
            lambda item: item["types"]["table"]["fields"].update(
                tail={"offset": 16, "sizeof": 8, "required": True}),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                candidate = copy.deepcopy(self.old)
                mutation(candidate)
                self.assertTrue(compare(self.old, candidate))


if __name__ == "__main__":
    unittest.main()
