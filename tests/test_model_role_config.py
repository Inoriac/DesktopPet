import copy
import os
import sys
import unittest


REPOSITORY_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPOSITORY_ROOT, "launcher"))

import config_loader  # noqa: E402


class LauncherModelRoleConfigTests(unittest.TestCase):
    def test_export_model_roles_whenMultipleProfilesConfigured_shouldPreserveEachRoleAndFallback(self):
        template = {
            "aiSettings": {
                "activeProfile": "selected",
                "profiles": {
                    "default": {
                        "modelRoles": {
                            "dialogue": {
                                "routes": [{"routeId": "default-dialogue", "model": "unchanged"}]
                            }
                        }
                    },
                    "selected": {
                        "enabled": True,
                        "provider": "legacy-provider",
                        "baseUrl": "https://legacy.example/v1",
                        "apiKey": "legacy-key",
                        "model": "legacy-model",
                        "visual_model": "legacy-vision",
                        "modelRoles": {
                            "dialogue": {
                                "routes": [
                                    {"routeId": "dialogue-primary", "model": "old-dialogue"},
                                    {"routeId": "dialogue-fallback", "model": "fallback-model"},
                                ]
                            },
                            "consolidation": {
                                "routes": [{"routeId": "consolidation", "model": "keep-me"}]
                            },
                            "vision": {
                                "routes": [{"routeId": "vision-primary", "model": "old-vision"}]
                            },
                        },
                    },
                },
            }
        }

        output = config_loader.apply_settings(
            template,
            {
                "provider": "openai-compatible",
                "baseUrl": "https://new.example/v1",
                "model": "new-dialogue",
                "visualModel": "new-vision",
            },
        )

        profiles = output["aiSettings"]["profiles"]
        selected_roles = profiles["selected"]["modelRoles"]
        self.assertEqual(
            profiles["default"]["modelRoles"]["dialogue"]["routes"][0]["model"],
            "unchanged",
        )
        self.assertEqual(selected_roles["dialogue"]["routes"][0]["model"], "new-dialogue")
        self.assertEqual(
            selected_roles["dialogue"]["routes"][1],
            {"routeId": "dialogue-fallback", "model": "fallback-model"},
        )
        self.assertEqual(
            selected_roles["consolidation"]["routes"][0]["model"], "keep-me"
        )
        self.assertEqual(selected_roles["vision"]["routes"][0]["model"], "new-vision")
        self.assertEqual(template["aiSettings"]["profiles"]["selected"]
                         ["modelRoles"]["dialogue"]["routes"][0]["model"],
                         "old-dialogue")

    def test_export_model_roles_whenApiKeyPresent_shouldKeepExistingSecretHandlingBehavior(self):
        template = {
            "aiSettings": {
                "activeProfile": "default",
                "profiles": {
                    "default": {
                        "apiKey": "old-key",
                        "modelRoles": {
                            "dialogue": {
                                "routes": [{"routeId": "dialogue-primary", "apiKey": "old-key"}]
                            },
                            "vision": {
                                "routes": [{"routeId": "vision-primary", "apiKey": "old-key"}]
                            },
                        },
                    }
                },
            }
        }

        output = config_loader.apply_settings(
            copy.deepcopy(template), {"apiKey": "new-secret"})
        profile = output["aiSettings"]["profiles"]["default"]

        self.assertEqual(profile["apiKey"], "new-secret")
        self.assertEqual(
            profile["modelRoles"]["dialogue"]["routes"][0]["apiKey"],
            "new-secret",
        )
        self.assertEqual(
            profile["modelRoles"]["vision"]["routes"][0]["apiKey"],
            "new-secret",
        )


if __name__ == "__main__":
    unittest.main()
