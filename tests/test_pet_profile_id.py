import json
import os
import sys
import tempfile
import unittest
from unittest import mock


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import pet_registry  # noqa: E402


class PetProfileIdTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.registry_path = os.path.join(self.temp_dir.name, "pets.json")
        self.path_patch = mock.patch.object(
            pet_registry, "pets_json_path", return_value=self.registry_path)
        self.path_patch.start()

    def tearDown(self):
        self.path_patch.stop()
        self.temp_dir.cleanup()

    def _write_registry(self, entries):
        with open(self.registry_path, "w", encoding="utf-8") as handle:
            json.dump(entries, handle)

    def test_load_pets_whenLegacyEntriesExist_shouldAssignAndPersistStableProfileIds(self):
        self._write_registry([
            {"name": "Milltina", "modelPath": "models/milltina.gltf"},
            {"name": "Nora", "modelPath": "models/nora.gltf"},
        ])

        first = pet_registry.load_pets()
        second = pet_registry.load_pets()

        self.assertEqual(len(first), 2)
        self.assertEqual(
            [entry.profile_id for entry in first],
            [entry.profile_id for entry in second],
        )
        self.assertEqual(len({entry.profile_id for entry in first}), 2)
        for entry in first:
            self.assertRegex(
                entry.profile_id,
                r"^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$",
            )
        with open(self.registry_path, "r", encoding="utf-8") as handle:
            persisted = json.load(handle)
        self.assertEqual(
            [item["profileId"] for item in persisted],
            [entry.profile_id for entry in first],
        )

    def test_rename_pet_whenProfileExists_shouldPreserveProfileId(self):
        entry = pet_registry.PetEntry(
            name="Milltina",
            model_path="models/milltina.gltf",
            profile_id="5bb00e6d-937a-4f46-9c87-e3933c078f5a",
        )
        self._write_registry([entry.to_dict()])

        renamed = pet_registry.rename_pet(entry.profile_id, "Milly")

        self.assertEqual(renamed.name, "Milly")
        self.assertEqual(renamed.profile_id, entry.profile_id)
        self.assertEqual(pet_registry.load_pets(), [renamed])

    def test_load_pets_whenExistingProfileIdsAreDuplicated_shouldRejectWithoutRewrite(self):
        duplicate_id = "f8685597-fc48-4df7-a15a-8ccfde643c52"
        original = [
            {"name": "Milltina", "modelPath": "one.gltf", "profileId": duplicate_id},
            {"name": "Nora", "modelPath": "two.gltf", "profileId": duplicate_id},
        ]
        self._write_registry(original)

        with self.assertRaises(pet_registry.PetRegistryError):
            pet_registry.load_pets()

        with open(self.registry_path, "r", encoding="utf-8") as handle:
            self.assertEqual(json.load(handle), original)


if __name__ == "__main__":
    unittest.main()
