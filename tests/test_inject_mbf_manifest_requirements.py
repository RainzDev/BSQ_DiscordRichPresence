import hashlib
import json
import sys
import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "scripts"))

import inject_mbf_manifest_requirements as injector  # noqa: E402


class InjectorTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary_directory.name)
        self.template = self.directory / "mod.template.json"
        self.template.write_text(
            json.dumps(
                {
                    "_QPVersion": "0.1.2",
                    injector.FIELD_NAME: {"queryPackages": ["com.discord"]},
                }
            ),
            encoding="utf-8",
        )
        self.qmod = self.directory / "test.qmod"

    def tearDown(self):
        self.temporary_directory.cleanup()

    def create_qmod(self, duplicate_manifest=False):
        with zipfile.ZipFile(self.qmod, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("cover.jpg", b"cover payload")
            archive.writestr(
                "mod.json",
                json.dumps(
                    {
                        "_QPVersion": "0.1.2",
                        "name": "Test mod",
                        "id": "test-mod",
                        "author": "Test",
                        "version": "1.0.0",
                        "modFiles": ["libtest.so"],
                    }
                ),
            )
            archive.writestr("libtest.so", b"native payload")
            if duplicate_manifest:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", UserWarning)
                    archive.writestr("mod.json", b"{}")

    def test_injects_field_without_changing_version_payloads_or_order(self):
        self.create_qmod()
        with zipfile.ZipFile(self.qmod) as archive:
            original_names = archive.namelist()
            original_payloads = {
                name: hashlib.sha256(archive.read(name)).hexdigest()
                for name in original_names
                if name != "mod.json"
            }

        self.assertTrue(injector.inject(self.qmod, self.template))

        with zipfile.ZipFile(self.qmod) as archive:
            self.assertEqual(archive.namelist(), original_names)
            manifest = json.loads(archive.read("mod.json"))
            self.assertEqual(manifest["_QPVersion"], "0.1.2")
            self.assertEqual(
                manifest[injector.FIELD_NAME], {"queryPackages": ["com.discord"]}
            )
            self.assertEqual(
                {
                    name: hashlib.sha256(archive.read(name)).hexdigest()
                    for name in original_names
                    if name != "mod.json"
                },
                original_payloads,
            )

    def test_second_run_is_idempotent(self):
        self.create_qmod()
        self.assertTrue(injector.inject(self.qmod, self.template))
        first_hash = hashlib.sha256(self.qmod.read_bytes()).hexdigest()

        self.assertFalse(injector.inject(self.qmod, self.template))
        self.assertEqual(hashlib.sha256(self.qmod.read_bytes()).hexdigest(), first_hash)

    def test_rejects_unsafe_package_name_before_rewriting_archive(self):
        self.create_qmod()
        original_hash = hashlib.sha256(self.qmod.read_bytes()).hexdigest()
        self.template.write_text(
            json.dumps(
                {injector.FIELD_NAME: {"queryPackages": ['com.discord" />']}}
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "Invalid Android package name"):
            injector.inject(self.qmod, self.template)
        self.assertEqual(hashlib.sha256(self.qmod.read_bytes()).hexdigest(), original_hash)

    def test_rejects_duplicate_root_manifest(self):
        self.create_qmod(duplicate_manifest=True)

        with self.assertRaisesRegex(ValueError, "exactly one root mod.json"):
            injector.inject(self.qmod, self.template)


if __name__ == "__main__":
    unittest.main()
