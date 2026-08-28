import copy
import os
import sys
import unittest


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import config_loader  # noqa: E402
from app_state import ModelEndpointState, ModelRoleState  # noqa: E402


def endpoint(provider, base_url, api_key, version="2023-06-01"):
    return ModelEndpointState(
        provider=provider,
        base_url=base_url,
        api_key=api_key,
        anthropic_version=version,
    )


class LauncherModelRoleConfigTests(unittest.TestCase):
    def test_apply_model_endpoint_settings_whenDefaultChanges_shouldKeepRoleModelsAndReferences(self):
        profile = {
            "modelEndpoints": {
                "DEFAULT": {
                    "provider": "openai-compatible",
                    "baseUrl": "https://old.example/v1",
                    "apiKey": "old-key",
                }
            },
            "modelRoles": {
                "dialogue": {"routes": [{
                    "routeId": "dialogue-primary",
                    "endpointRef": "DEFAULT",
                    "model": "dialogue-model",
                }]},
                "vision": {"routes": [{
                    "routeId": "vision-primary",
                    "endpointRef": "DEFAULT",
                    "model": "vision-model",
                    "supportsVision": True,
                }]},
            },
        }
        endpoints = {"DEFAULT": endpoint(
            "anthropic-messages", "https://new.example", "new-key",
            "2024-01-01")}
        roles = {
            "dialogue": ModelRoleState("DEFAULT", "dialogue-model"),
            "vision": ModelRoleState("DEFAULT", "vision-model"),
        }

        config_loader.apply_model_endpoint_settings(profile, endpoints, roles)

        self.assertEqual(
            profile["modelEndpoints"]["DEFAULT"]["baseUrl"],
            "https://new.example",
        )
        self.assertEqual(
            profile["modelRoles"]["dialogue"]["routes"][0]["model"],
            "dialogue-model",
        )
        self.assertEqual(
            profile["modelRoles"]["vision"]["routes"][0]["model"],
            "vision-model",
        )
        self.assertEqual(
            profile["modelRoles"]["vision"]["routes"][0]["endpointRef"],
            "DEFAULT",
        )

    def test_apply_model_endpoint_settings_whenSpecialEndpointIsUsed_shouldKeepApiKeyOnlyInRegistry(self):
        profile = {
            "provider": "legacy",
            "baseUrl": "https://legacy.example/v1",
            "apiKey": "legacy-key",
            "modelRoles": {"diary": {"routes": [{
                "routeId": "diary-primary",
                "provider": "openai-compatible",
                "baseUrl": "https://old-diary.example/v1",
                "apiKey": "old-diary-key",
                "model": "old-diary-model",
            }]}},
        }
        endpoints = {
            "DEFAULT": endpoint(
                "anthropic-messages", "https://default.example", "default-key"),
            "DIARY_VENDOR": endpoint(
                "openai-compatible", "https://diary.example/v1", "diary-key"),
        }
        roles = {"diary": ModelRoleState("DIARY_VENDOR", "diary-model")}

        config_loader.apply_model_endpoint_settings(profile, endpoints, roles)

        route = profile["modelRoles"]["diary"]["routes"][0]
        self.assertEqual(route["endpointRef"], "DIARY_VENDOR")
        self.assertEqual(route["model"], "diary-model")
        for key in ("provider", "baseUrl", "apiKey", "anthropicVersion", "extraHeaders"):
            self.assertNotIn(key, route)
        self.assertNotIn("apiKey", profile)
        self.assertEqual(
            profile["modelEndpoints"]["DIARY_VENDOR"]["apiKey"],
            "diary-key",
        )

    def test_apply_model_endpoint_settings_whenFallbacksAndUnknownFieldsExist_shouldMigrateConnectionsAndPreserveBehavior(self):
        profile = {
            "modelRoles": {
                "dialogue": {
                    "limits": {"maxLatencyMs": 9000},
                    "unknownRoleField": {"keep": True},
                    "routes": [
                        {
                            "routeId": "dialogue-primary",
                            "provider": "anthropic-messages",
                            "baseUrl": "https://default.example",
                            "apiKey": "default-key",
                            "model": "old-model",
                            "unknownRouteField": "keep-primary",
                        },
                        {
                            "routeId": "dialogue-fallback",
                            "provider": "openai-compatible",
                            "baseUrl": "https://fallback.example/v1",
                            "apiKey": "fallback-key",
                            "model": "fallback-model",
                            "timeoutMs": 54321,
                            "unknownRouteField": "keep-fallback",
                        },
                    ],
                }
            }
        }
        endpoints = {
            "DEFAULT": endpoint(
                "anthropic-messages", "https://default.example", "default-key"),
            "MIGRATED_DIALOGUE_1": endpoint(
                "openai-compatible", "https://fallback.example/v1", "fallback-key"),
        }
        roles = {"dialogue": ModelRoleState("DEFAULT", "dialogue-model")}

        config_loader.apply_model_endpoint_settings(profile, endpoints, roles)

        role = profile["modelRoles"]["dialogue"]
        self.assertEqual(role["limits"], {"maxLatencyMs": 9000})
        self.assertEqual(role["unknownRoleField"], {"keep": True})
        self.assertEqual(role["routes"][0]["unknownRouteField"], "keep-primary")
        self.assertEqual(role["routes"][1]["routeId"], "dialogue-fallback")
        self.assertEqual(role["routes"][1]["endpointRef"], "MIGRATED_DIALOGUE_1")
        self.assertEqual(role["routes"][1]["model"], "fallback-model")
        self.assertEqual(role["routes"][1]["timeoutMs"], 54321)
        self.assertEqual(role["routes"][1]["unknownRouteField"], "keep-fallback")
        self.assertNotIn("apiKey", role["routes"][1])

    def test_apply_model_endpoint_settings_whenFallbackReferenceIsUnknown_shouldRejectBeforeWrite(self):
        profile = {
            "modelRoles": {
                "dialogue": {"routes": [
                    {
                        "routeId": "dialogue-primary",
                        "endpointRef": "DEFAULT",
                        "model": "dialogue-model",
                    },
                    {
                        "routeId": "dialogue-fallback",
                        "endpointRef": "REMOVED_VENDOR",
                        "model": "fallback-model",
                    },
                ]}
            }
        }
        endpoints = {"DEFAULT": endpoint(
            "anthropic-messages", "https://default.example", "default-key")}
        roles = {"dialogue": ModelRoleState("DEFAULT", "dialogue-model")}

        with self.assertRaisesRegex(ValueError, "REMOVED_VENDOR"):
            config_loader.apply_model_endpoint_settings(
                profile, endpoints, roles)

    def test_apply_model_endpoint_settings_whenLegacyDaydreamModelSettingsExist_shouldMoveThemToDaydreamRoute(self):
        profile = {
            "daydream": {
                "enabled": True,
                "model": "legacy-light-model",
                "maxTokens": 222,
                "temperature": 0.25,
                "batchLimit": 5,
            }
        }
        endpoints = {"DEFAULT": endpoint(
            "openai-compatible", "https://default.example/v1", "default-key")}
        roles = {
            "daydream": ModelRoleState("DEFAULT", "legacy-light-model")
        }

        config_loader.apply_model_endpoint_settings(profile, endpoints, roles)

        route = profile["modelRoles"]["daydream"]["routes"][0]
        self.assertEqual(route["endpointRef"], "DEFAULT")
        self.assertEqual(route["model"], "legacy-light-model")
        self.assertEqual(route["maxTokens"], 222)
        self.assertEqual(route["temperature"], 0.25)
        self.assertEqual(profile["daydream"], {"enabled": True, "batchLimit": 5})

    def test_apply_settings_preserves_saved_fallbacks_whenUsingSavedConfigAsBase(self):
        saved = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {"default": {
                    "modelEndpoints": {"DEFAULT": {
                        "provider": "openai-compatible",
                        "baseUrl": "https://old.example/v1",
                        "apiKey": "old-key",
                    }},
                    "modelRoles": {"dialogue": {
                        "limits": {"maxLatencyMs": 8000},
                        "routes": [
                            {"routeId": "primary", "endpointRef": "DEFAULT", "model": "main"},
                            {"routeId": "fallback", "endpointRef": "DEFAULT", "model": "backup"},
                        ],
                    }},
                }},
            }
        }
        state_endpoints = {"DEFAULT": endpoint(
            "openai-compatible", "https://new.example/v1", "new-key")}
        state_roles = {"dialogue": ModelRoleState("DEFAULT", "main")}

        output = config_loader.apply_settings(
            copy.deepcopy(saved),
            {"modelEndpoints": state_endpoints, "modelRoles": state_roles},
        )

        role = output["aiSettings"]["profiles"]["default"]["modelRoles"]["dialogue"]
        self.assertEqual(role["limits"], {"maxLatencyMs": 8000})
        self.assertEqual(role["routes"][1]["routeId"], "fallback")


if __name__ == "__main__":
    unittest.main()
