from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path
from unittest import mock


RELEASE_TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RELEASE_TOOLS))

import firmware_artifact  # noqa: E402


def make_esp32s3_image(segment_data: bytes = b"litch-test-image") -> bytes:
    padding = (-len(segment_data)) % 4
    segment_data += b"\0" * padding

    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[12:14] = struct.pack("<H", 9)
    header[23] = 1

    segment = struct.pack("<II", 0x3C000020, len(segment_data)) + segment_data
    checksum = 0xEF
    for value in segment_data:
        checksum ^= value

    image_without_checksum = bytes(header) + segment
    checksum_padding = b"\0" * ((15 - len(image_without_checksum)) % 16)
    image_prefix = image_without_checksum + checksum_padding + bytes([checksum])
    return image_prefix + hashlib.sha256(image_prefix).digest()


class FirmwareArtifactTests(unittest.TestCase):
    def test_verified_boot_policy_matches_shared_full_image_fixtures(self) -> None:
        fixture = Path(__file__).resolve().parents[3] / "test/fixtures/firmware_boot_policy.json"
        cases = json.loads(fixture.read_text())["cases"]
        for case in cases:
            with self.subTest(case=case["name"]):
                data = bytes.fromhex(case["image_hex"])
                if not case["valid_integrity"]:
                    with self.assertRaises(firmware_artifact.ArtifactValidationError):
                        firmware_artifact.inspect_firmware_bytes_with_policy(data)
                    continue
                inspected = firmware_artifact.inspect_firmware_bytes_with_policy(data)
                self.assertEqual(asdict(inspected.boot_policy), case["expected"])
                self.assertEqual(inspected.metadata.sha256, case["artifact_sha256"])
                self.assertEqual(inspected.metadata.image_sha256, case["image_sha256"])
                self.assertEqual(
                    inspected.first_segment_load_address,
                    struct.unpack_from("<I", data, 24)[0],
                )
                self.assertEqual(
                    inspected.first_segment_size_bytes,
                    struct.unpack_from("<I", data, 28)[0],
                )
                self.assertEqual(
                    asdict(inspected.metadata),
                    asdict(firmware_artifact.inspect_firmware_bytes(data)),
                )
                self.assertEqual(set(asdict(inspected.metadata)), {
                    "size_bytes", "sha256", "image_sha256", "chip_id", "segment_count",
                })

    def test_unverified_app_descriptor_never_implies_legacy_policy(self) -> None:
        policy = b"FXBT" + struct.pack("<HHII", 1, 16, 2, 0)
        for segment in (b"tiny", bytes(252), bytes(256) + policy):
            with self.subTest(segment_bytes=len(segment)):
                data = make_esp32s3_image(segment)
                inspected = firmware_artifact.inspect_firmware_bytes_with_policy(data)
                self.assertEqual(inspected.boot_policy.status.value, "unavailable")
                self.assertEqual(inspected.boot_policy.epoch, 0)
                self.assertEqual(inspected.metadata, firmware_artifact.inspect_firmware_bytes(data))

    def test_integrity_failure_precedes_capability_decoding(self) -> None:
        data = bytearray(make_esp32s3_image())
        data[-1] ^= 1
        with mock.patch.object(firmware_artifact, "_inspect_boot_policy") as decode:
            with self.assertRaisesRegex(firmware_artifact.ArtifactValidationError, "SHA-256"):
                firmware_artifact.inspect_firmware_bytes_with_policy(data)
        decode.assert_not_called()

    def test_combined_file_inspection_reads_metadata_and_policy_from_same_bytes(self) -> None:
        fixture = Path(__file__).resolve().parents[3] / "test/fixtures/firmware_boot_policy.json"
        cases = {case["name"]: case for case in json.loads(fixture.read_text())["cases"]}
        data = bytes.fromhex(cases["sdk_policy_2"]["image_hex"])
        replacement = bytes.fromhex(cases["record_policy_1"]["image_hex"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.bin"
            path.write_bytes(data)
            with mock.patch.object(Path, "read_bytes", side_effect=(data, replacement)) as read:
                inspected = firmware_artifact.inspect_firmware_with_policy(path)
            read.assert_called_once_with()
        self.assertEqual(inspected.boot_policy.epoch, 2)
        self.assertEqual(inspected.metadata.sha256, cases["sdk_policy_2"]["artifact_sha256"])

    def test_combined_inspection_retains_exact_publication_size_boundary(self) -> None:
        data = make_esp32s3_image(
            b"x" * (firmware_artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE - 80),
        )
        self.assertEqual(len(data), firmware_artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE)
        inspected = firmware_artifact.inspect_firmware_bytes_with_policy(data)
        self.assertEqual(inspected.metadata.size_bytes, len(data))
        with self.assertRaisesRegex(firmware_artifact.ArtifactValidationError, "2 MiB"):
            firmware_artifact.inspect_firmware_bytes_with_policy(data + b"x")

    def test_accepts_a_complete_esp32s3_image(self) -> None:
        image = make_esp32s3_image()

        artifact = firmware_artifact.inspect_firmware_bytes(image)

        self.assertEqual(artifact.size_bytes, len(image))
        self.assertEqual(artifact.chip_id, 9)
        self.assertEqual(artifact.segment_count, 1)
        self.assertEqual(artifact.sha256, hashlib.sha256(image).hexdigest())
        self.assertEqual(
            artifact.image_sha256,
            hashlib.sha256(image[:-32]).hexdigest(),
        )

    def test_rejects_images_that_are_not_complete_hashed_s3_artifacts(self) -> None:
        valid = make_esp32s3_image()
        cases: dict[str, bytes] = {}

        wrong_magic = bytearray(valid)
        wrong_magic[0] = 0
        cases["magic"] = bytes(wrong_magic)

        no_segments = bytearray(valid)
        no_segments[1] = 0
        cases["segment count"] = bytes(no_segments)

        wrong_chip = bytearray(valid)
        wrong_chip[12:14] = struct.pack("<H", 5)
        cases["chip ID"] = bytes(wrong_chip)

        no_image_hash = bytearray(valid)
        no_image_hash[23] = 0
        cases["appended image SHA"] = bytes(no_image_hash)

        corrupt_checksum = bytearray(valid)
        corrupt_checksum[-33] ^= 1
        cases["checksum"] = bytes(corrupt_checksum)

        corrupt_digest = bytearray(valid)
        corrupt_digest[-1] ^= 1
        cases["image SHA"] = bytes(corrupt_digest)

        cases["exact size"] = valid + b"unexpected"
        cases["2 MiB"] = (
            b"\xE9"
            + b"\0" * firmware_artifact.MAX_OFFICIAL_ESP32S3_FIRMWARE_SIZE
        )

        for expected_message, image in cases.items():
            with self.subTest(expected_message=expected_message):
                with self.assertRaisesRegex(
                    firmware_artifact.ArtifactValidationError,
                    expected_message,
                ):
                    firmware_artifact.inspect_firmware_bytes(image)

    def test_renders_a_deterministic_schema_one_manifest(self) -> None:
        artifact = firmware_artifact.inspect_firmware_bytes(
            make_esp32s3_image(),
        )
        firmware_url = (
            "https://foxielitch.com/fw/firmware_0.6.0_"
            f"{artifact.sha256}.bin"
        )
        notes = firmware_artifact.ReleaseNotesArtifact(
            size_bytes=24,
            sha256=artifact.sha256,
        )

        manifest = firmware_artifact.build_manifest(
            artifact=artifact,
            channel="stable",
            version="0.6.0",
            manifest_url="https://foxielitch.com/fw/manifest.json",
            firmware_url=firmware_url,
            notes_url=(
                "https://foxielitch.com/fw/version_0.6.0_"
                f"{artifact.sha256}.md"
            ),
            notes=notes,
        )
        rendered = firmware_artifact.render_manifest_json(manifest)

        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["target"], "esp32s3")
        self.assertEqual(manifest["chip_id"], 9)
        self.assertEqual(manifest["artifact"]["url"], firmware_url)
        self.assertEqual(manifest["artifact"]["size_bytes"], len(make_esp32s3_image()))
        self.assertEqual(manifest["artifact"]["sha256"], artifact.sha256)
        self.assertEqual(
            manifest["artifact"]["image_sha256"],
            artifact.image_sha256,
        )
        self.assertEqual(rendered, firmware_artifact.render_manifest_json(manifest))
        self.assertTrue(rendered.endswith("\n"))
        self.assertNotIn(str(Path.cwd()), rendered)

    def test_rejects_untrusted_or_mutable_manifest_urls(self) -> None:
        artifact = firmware_artifact.inspect_firmware_bytes(
            make_esp32s3_image(),
        )
        immutable_name = f"firmware_0.6.0_{artifact.sha256}.bin"
        cases = [
            ("plain HTTPS", f"http://foxielitch.com/fw/{immutable_name}"),
            ("manifest origin", f"https://updates.example/fw/{immutable_name}"),
            ("immutable", "https://foxielitch.com/fw/firmware.bin"),
            ("immutable", (
                "https://foxielitch.com/fw/firmware_0.6.0_"
                f"{artifact.sha256}%2fextra.bin"
            )),
        ]

        for expected_message, firmware_url in cases:
            with self.subTest(firmware_url=firmware_url):
                with self.assertRaisesRegex(
                    firmware_artifact.ArtifactValidationError,
                    expected_message,
                ):
                    firmware_artifact.build_manifest(
                        artifact=artifact,
                        channel="stable",
                        version="0.6.0",
                        manifest_url="https://foxielitch.com/fw/manifest.json",
                        firmware_url=firmware_url,
                        notes_url=(
                            "https://foxielitch.com/fw/version_0.6.0_"
                            f"{artifact.sha256}.md"
                        ),
                        notes=firmware_artifact.ReleaseNotesArtifact(
                            size_bytes=24,
                            sha256=artifact.sha256,
                        ),
                    )

    def test_release_names_keep_aliases_and_add_immutable_assets(self) -> None:
        digest = "a" * 64

        stable = firmware_artifact.release_names(
            "stable",
            "0.6.0",
            digest,
            digest,
        )
        beta = firmware_artifact.release_names(
            "beta",
            "0.6.0-beta.1",
            digest,
            digest,
        )

        self.assertEqual(stable["firmware_alias"], "firmware.bin")
        self.assertEqual(stable["notes_alias"], "version.md")
        self.assertEqual(stable["manifest"], "manifest.json")
        self.assertEqual(beta["firmware_alias"], "firmware_beta.bin")
        self.assertEqual(beta["notes_alias"], "version_beta.md")
        self.assertEqual(beta["manifest"], "manifest_beta.json")
        self.assertEqual(
            stable["firmware_immutable"],
            f"firmware_0.6.0_{digest}.bin",
        )
        self.assertEqual(
            beta["notes_immutable"],
            f"version_beta_0.6.0-beta.1_{digest}.md",
        )

    def test_notes_name_is_addressed_by_its_own_digest(self) -> None:
        firmware_digest = "a" * 64
        notes_digest = "b" * 64

        names = firmware_artifact.release_names(
            "stable",
            "0.6.0",
            firmware_digest,
            notes_digest,
        )

        self.assertEqual(
            names["notes_immutable"],
            f"version_0.6.0_{notes_digest}.md",
        )
        self.assertEqual(
            names["firmware_immutable"],
            f"firmware_0.6.0_{firmware_digest}.bin",
        )

    def test_manifest_binds_exact_content_addressed_notes(self) -> None:
        image = make_esp32s3_image()
        notes = b"# Stable\n\nVersion: 0.6.0\n"
        notes_digest = hashlib.sha256(notes).hexdigest()
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            firmware_path = root / "firmware.bin"
            notes_path = root / "version.md"
            manifest_path = root / "manifest.json"
            firmware_path.write_bytes(image)
            notes_path.write_bytes(notes)

            manifest = firmware_artifact.create_manifest(
                firmware_path=firmware_path,
                notes_path=notes_path,
                channel="stable",
                version="0.6.0",
                public_base_url="https://foxielitch.com/fw",
                output_path=manifest_path,
            )

        self.assertEqual(manifest["notes"]["size_bytes"], len(notes))
        self.assertEqual(manifest["notes"]["sha256"], notes_digest)
        self.assertEqual(
            manifest["notes"]["url"],
            "https://foxielitch.com/fw/"
            f"version_0.6.0_{notes_digest}.md",
        )

    def test_manifest_rejects_invalid_notes_metadata(self) -> None:
        artifact = firmware_artifact.inspect_firmware_bytes(
            make_esp32s3_image(),
        )
        firmware_url = (
            "https://foxielitch.com/fw/firmware_0.6.0_"
            f"{artifact.sha256}.bin"
        )
        for notes in (
            firmware_artifact.ReleaseNotesArtifact(
                size_bytes=0,
                sha256=artifact.sha256,
            ),
            firmware_artifact.ReleaseNotesArtifact(
                size_bytes=firmware_artifact.MAX_NOTES_SIZE + 1,
                sha256=artifact.sha256,
            ),
            firmware_artifact.ReleaseNotesArtifact(
                size_bytes=24,
                sha256="not-a-sha",
            ),
        ):
            with self.subTest(notes=notes):
                with self.assertRaisesRegex(
                    firmware_artifact.ArtifactValidationError,
                    "notes",
                ):
                    firmware_artifact.build_manifest(
                        artifact=artifact,
                        channel="stable",
                        version="0.6.0",
                        manifest_url="https://foxielitch.com/fw/manifest.json",
                        firmware_url=firmware_url,
                        notes_url=(
                            "https://foxielitch.com/fw/version_0.6.0_"
                            f"{notes.sha256}.md"
                        ),
                        notes=notes,
                    )

    def test_release_verification_rejects_changed_notes(self) -> None:
        image = make_esp32s3_image()
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            firmware_path = root / "firmware.bin"
            notes_path = root / "version.md"
            manifest_path = root / "manifest.json"
            firmware_path.write_bytes(image)
            notes_path.write_text("Version: 0.6.0\n", encoding="utf-8")
            firmware_artifact.create_manifest(
                firmware_path=firmware_path,
                notes_path=notes_path,
                channel="stable",
                version="0.6.0",
                public_base_url="https://foxielitch.com/fw",
                output_path=manifest_path,
            )
            notes_path.write_text("Version: changed\n", encoding="utf-8")

            with self.assertRaisesRegex(
                firmware_artifact.ArtifactValidationError,
                "notes",
            ):
                firmware_artifact.verify_release(
                    firmware_path=firmware_path,
                    notes_path=notes_path,
                    manifest_path=manifest_path,
                    manifest_url="https://foxielitch.com/fw/manifest.json",
                )

    def test_cli_creates_and_verifies_a_release_manifest(self) -> None:
        image = make_esp32s3_image()
        digest = hashlib.sha256(image).hexdigest()
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            firmware_path = root / "firmware.bin"
            notes_path = root / "version.md"
            manifest_path = root / "manifest.json"
            firmware_path.write_bytes(image)
            notes_path.write_text("Version: 0.6.0\n", encoding="utf-8")

            result = firmware_artifact.main(
                [
                    "create-manifest",
                    "--firmware",
                    str(firmware_path),
                    "--notes",
                    str(notes_path),
                    "--channel",
                    "stable",
                    "--version",
                    "0.6.0",
                    "--public-base-url",
                    "https://foxielitch.com/fw",
                    "--output",
                    str(manifest_path),
                ]
            )

            self.assertEqual(result, 0)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                manifest["artifact"]["url"],
                "https://foxielitch.com/fw/"
                f"firmware_0.6.0_{digest}.bin",
            )
            self.assertNotIn(str(root), manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                firmware_artifact.main(
                    [
                        "verify",
                        "--firmware",
                        str(firmware_path),
                        "--notes",
                        str(notes_path),
                        "--manifest",
                        str(manifest_path),
                        "--manifest-url",
                        "https://foxielitch.com/fw/manifest.json",
                    ]
                ),
                0,
            )

            manifest["schema_version"] = True
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                firmware_artifact.ArtifactValidationError,
                "exactly describe",
            ):
                firmware_artifact.verify_release(
                    firmware_path=firmware_path,
                    notes_path=notes_path,
                    manifest_path=manifest_path,
                    manifest_url="https://foxielitch.com/fw/manifest.json",
                )


if __name__ == "__main__":
    unittest.main()
