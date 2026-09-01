import copy
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import patch


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import config_loader  # noqa: E402
from app_state import AppState, ModelEndpointState, ModelRoleState  # noqa: E402


class LauncherConfigTests(unittest.TestCase):
    def test_effective_config_restores_missing_schema_sections(self):
        template = config_loader.load_template()
        partial = {"legacy": True, "renderSettings": {"antialiasing": False}}

        merged = config_loader.merge_with_template(template, partial)

        self.assertFalse(merged["renderSettings"]["antialiasing"])
        self.assertIn("interactionSettings", merged)
        self.assertTrue(merged["legacy"])

    def test_template_defaults_all_model_roles_to_single_default_endpoint(self):
        config = config_loader.load_template()
        ai = config["aiSettings"]
        profile = ai["profiles"][ai["activeProfile"]]

        self.assertEqual(set(profile["modelEndpoints"]), {"DEFAULT"})
        for role in config_loader.MODEL_ROLES:
            route = profile["modelRoles"][role]["routes"][0]
            self.assertEqual(route["endpointRef"], "DEFAULT")

    def test_explicit_partial_model_state_still_exposes_required_defaults(self):
        state = AppState(
            model_endpoints={},
            model_roles={
                "dialogue": ModelRoleState("DEFAULT", "dialogue-model")
            },
        )

        self.assertIn("DEFAULT", state.model_endpoints)
        self.assertEqual(set(state.model_roles), {
            "dialogue", "vision", "fastExtract", "consolidation", "diary",
            "daydream",
        })
        self.assertEqual(state.model_roles["dialogue"].model, "dialogue-model")

    def test_app_state_exports_emotion_setting(self):
        state = AppState(emotion_enabled=False)
        settings = state.to_settings_dict()
        self.assertIn("emotionEnabled", settings)
        self.assertFalse(settings["emotionEnabled"])

    def test_apply_settings_updates_active_profile_only(self):
        template = {
            "aiSettings": {
                "activeProfile": "selected",
                "profiles": {
                    "default": {"emotion": {"enabled": True}},
                    "selected": {"emotion": {"enabled": True}},
                },
            }
        }
        output = config_loader.apply_settings(template, {"emotionEnabled": False})
        profiles = output["aiSettings"]["profiles"]
        self.assertTrue(profiles["default"]["emotion"]["enabled"])
        self.assertFalse(profiles["selected"]["emotion"]["enabled"])
        self.assertTrue(template["aiSettings"]["profiles"]["selected"]["emotion"]["enabled"])

    def test_template_round_trip_keeps_emotion_block(self):
        template = config_loader.load_template()
        output = config_loader.apply_settings(
            copy.deepcopy(template), {"emotionEnabled": False})
        profile = output["aiSettings"]["profiles"][output["aiSettings"]["activeProfile"]]
        self.assertFalse(profile["emotion"]["enabled"])
        self.assertEqual(profile["emotion"]["decay"]["valenceHalfLifeSec"], 3600)

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "config.json")
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(output, handle, ensure_ascii=False)
            with open(path, "r", encoding="utf-8") as handle:
                loaded = json.load(handle)
        self.assertFalse(
            loaded["aiSettings"]["profiles"]["default"]["emotion"]["enabled"])

    def test_saved_config_restores_api_and_launcher_fields(self):
        state = AppState(
            ai_enabled=True,
            auto_screen_chat=True,
            model_endpoints={"DEFAULT": ModelEndpointState(
                provider="anthropic-messages", base_url="https://text.example",
                api_key="saved-text-secret")},
            model_roles={
                "dialogue": ModelRoleState("DEFAULT", "text-model"),
                "vision": ModelRoleState("DEFAULT", "vision-model"),
            },
            scale_percent=135,
            snap_enabled=False,
        )
        config = config_loader.apply_settings(
            config_loader.load_template(), state.to_settings_dict())
        overrides = state.to_advanced_overrides()
        config["renderSettings"].update(overrides["renderSettings"])
        config["interactionSettings"].update({
            "dragThreshold": overrides["interactionSettings"]["dragThreshold"],
            "clickTimeout": overrides["interactionSettings"]["clickTimeout"],
        })
        config["interactionSettings"]["windowSnapping"].update(
            overrides["interactionSettings"]["windowSnapping"])

        restored = AppState.from_config(config)

        self.assertTrue(restored.ai_enabled)
        self.assertTrue(restored.auto_screen_chat)
        self.assertEqual(
            restored.model_endpoints["DEFAULT"].base_url,
            "https://text.example",
        )
        self.assertEqual(
            restored.model_endpoints["DEFAULT"].api_key,
            "saved-text-secret",
        )
        self.assertEqual(restored.model_roles["dialogue"].model, "text-model")
        self.assertEqual(restored.model_roles["vision"].model, "vision-model")
        self.assertEqual(restored.scale_percent, 135)
        self.assertFalse(restored.snap_enabled)

    def test_from_config_whenRolesReferenceEndpointRegistry_shouldRestoreReferencesModelsAndKeysByEndpoint(self):
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {
                    "default": {
                        "modelEndpoints": {
                            "DEFAULT": {
                                "provider": "anthropic-messages",
                                "baseUrl": "https://text.example",
                                "apiKey": "text-key",
                                "anthropicVersion": "2024-01-01",
                            },
                            "VISION_VENDOR": {
                                "provider": "openai-compatible",
                                "baseUrl": "https://vision.example/v1",
                                "apiKey": "vision-key",
                            },
                        },
                        "modelRoles": {
                            "dialogue": {"routes": [{
                                "routeId": "dialogue-primary",
                                "endpointRef": "DEFAULT",
                                "model": "claude-text",
                            }]},
                            "vision": {"routes": [{
                                "routeId": "vision-primary",
                                "endpointRef": "VISION_VENDOR",
                                "model": "vision-model",
                            }]},
                        },
                    }
                },
            }
        }

        restored = AppState.from_config(config)

        self.assertEqual(restored.model_roles["dialogue"].endpoint_ref, "DEFAULT")
        self.assertEqual(restored.model_roles["dialogue"].model, "claude-text")
        self.assertEqual(restored.model_roles["vision"].endpoint_ref, "VISION_VENDOR")
        self.assertEqual(restored.model_roles["vision"].model, "vision-model")
        self.assertEqual(restored.model_endpoints["DEFAULT"].api_key, "text-key")
        self.assertEqual(
            restored.model_endpoints["VISION_VENDOR"].api_key,
            "vision-key",
        )
        self.assertFalse(hasattr(restored.model_endpoints["DEFAULT"], "model"))

    def test_from_config_whenLegacyRoutesUseDifferentKeys_shouldCreateDefaultAndIndependentMigratedEndpoints(self):
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "provider": "openai-compatible",
                    "baseUrl": "https://profile.example/v1",
                    "apiKey": "profile-key",
                    "model": "profile-text",
                    "visual_model": "legacy-vision",
                    "modelRoles": {
                        "dialogue": {"routes": [{
                            "routeId": "dialogue-primary",
                            "provider": "anthropic-messages",
                            "baseUrl": "https://dialogue.example",
                            "apiKey": "dialogue-key",
                            "model": "dialogue-model",
                        }]},
                        "vision": {"routes": [{
                            "routeId": "vision-primary",
                            "provider": "openai-compatible",
                            "baseUrl": "https://vision.example/v1",
                            "apiKey": "vision-key",
                            "model": "vision-model",
                        }]},
                    },
                }},
            }
        }

        restored = AppState.from_config(config)

        self.assertEqual(restored.model_endpoints["DEFAULT"].api_key, "dialogue-key")
        self.assertEqual(
            restored.model_endpoints["MIGRATED_VISION_0"].api_key,
            "vision-key",
        )
        self.assertEqual(restored.model_roles["dialogue"].endpoint_ref, "DEFAULT")
        self.assertEqual(
            restored.model_roles["vision"].endpoint_ref,
            "MIGRATED_VISION_0",
        )
        self.assertNotEqual(
            restored.model_endpoints["DEFAULT"].api_key,
            restored.model_endpoints["MIGRATED_VISION_0"].api_key,
        )

    def test_from_config_whenLegacyRoleConnectionExactlyMatchesDefault_shouldReuseDefaultEndpoint(self):
        shared_connection = {
            "provider": "anthropic-messages",
            "baseUrl": "https://gateway.example",
            "apiKey": "shared-key",
            "anthropicVersion": "2024-01-01",
            "extraHeaders": {"x-gateway-tenant": "desktop-pet"},
        }
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "modelRoles": {
                        "dialogue": {"routes": [{
                            **shared_connection,
                            "routeId": "dialogue-primary",
                            "model": "dialogue-model",
                        }]},
                        "vision": {"routes": [{
                            **shared_connection,
                            "routeId": "vision-primary",
                            "model": "vision-model",
                            "supportsVision": True,
                        }]},
                    },
                }},
            }
        }

        restored = AppState.from_config(config)

        self.assertEqual(restored.model_roles["vision"].endpoint_ref, "DEFAULT")
        self.assertNotIn("MIGRATED_VISION_0", restored.model_endpoints)
        self.assertEqual(restored.model_endpoints["DEFAULT"].extra_headers,
                         {"x-gateway-tenant": "desktop-pet"})

    def test_from_config_whenLegacyRoutesPartiallyOverrideConnection_shouldMaterializeCompleteMigratedEndpoints(self):
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "provider": "anthropic-messages",
                    "baseUrl": "https://default.example",
                    "apiKey": "default-key",
                    "anthropicVersion": "2024-01-01",
                    "extraHeaders": {"x-gateway-tenant": "desktop-pet"},
                    "model": "default-model",
                    "modelRoles": {
                        "dialogue": {"routes": [
                            {
                                "routeId": "dialogue-primary",
                                "model": "dialogue-model",
                            },
                            {
                                "routeId": "dialogue-fallback",
                                "baseUrl": "https://fallback.example",
                                "apiKey": "fallback-key",
                                "model": "fallback-model",
                            },
                        ]},
                        "vision": {"routes": [{
                            "routeId": "vision-primary",
                            "baseUrl": "https://vision.example",
                            "apiKey": "vision-key",
                            "model": "vision-model",
                        }]},
                    },
                }},
            }
        }

        restored = AppState.from_config(config)

        self.assertEqual(restored.model_roles["vision"].endpoint_ref,
                         "MIGRATED_VISION_0")
        for endpoint_id, expected_url, expected_key in (
            ("MIGRATED_DIALOGUE_1", "https://fallback.example", "fallback-key"),
            ("MIGRATED_VISION_0", "https://vision.example", "vision-key"),
        ):
            migrated = restored.model_endpoints[endpoint_id]
            self.assertEqual(migrated.provider, "anthropic-messages")
            self.assertEqual(migrated.base_url, expected_url)
            self.assertEqual(migrated.api_key, expected_key)
            self.assertEqual(migrated.anthropic_version, "2024-01-01")
            self.assertEqual(
                migrated.extra_headers,
                {"x-gateway-tenant": "desktop-pet"},
            )

        exported = config_loader.apply_settings(
            copy.deepcopy(config), restored.to_settings_dict())
        profile = exported["aiSettings"]["profiles"]["default"]
        fallback = profile["modelRoles"]["dialogue"]["routes"][1]
        self.assertEqual(fallback["endpointRef"], "MIGRATED_DIALOGUE_1")
        for field in config_loader.INLINE_CONNECTION_FIELDS:
            self.assertNotIn(field, fallback)

    def test_from_config_whenLegacyProfileHasProtocolFields_shouldPreserveThemOnDefaultEndpoint(self):
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "provider": "anthropic-messages",
                    "baseUrl": "https://gateway.example",
                    "apiKey": "legacy-key",
                    "anthropicVersion": "2024-01-01",
                    "extraHeaders": {"x-gateway-tenant": "desktop-pet"},
                    "model": "legacy-model",
                }},
            }
        }

        restored = AppState.from_config(config)

        endpoint = restored.model_endpoints["DEFAULT"]
        self.assertEqual(endpoint.anthropic_version, "2024-01-01")
        self.assertEqual(
            endpoint.extra_headers,
            {"x-gateway-tenant": "desktop-pet"},
        )

    def test_from_config_whenAnthropicVersionIsBlank_shouldRestoreDefault(self):
        config = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "modelEndpoints": {
                        "DEFAULT": {
                            "provider": "anthropic-messages",
                            "baseUrl": "https://gateway.example",
                            "apiKey": "key",
                            "anthropicVersion": "",
                        },
                    },
                }},
            },
        }

        restored = AppState.from_config(config)

        self.assertEqual(
            restored.model_endpoints["DEFAULT"].anthropic_version,
            "2023-06-01",
        )

    def test_export_then_load_saved_config_round_trips_json(self):
        config = {"aiSettings": {"activeProfile": "default"}}
        with tempfile.TemporaryDirectory() as directory, patch.object(
                config_loader, "_app_data_dir", return_value=directory):
            path = config_loader.export_config(config)
            restored = config_loader.load_saved_config()

        self.assertEqual(path, os.path.join(directory, "launch_config.json"))
        self.assertEqual(restored, config)


if __name__ == "__main__":
    unittest.main()
