#!/usr/bin/env python3
"""Manually scan a local QR image to save Bilibili credentials outside the package."""
import sys
sys.dont_write_bytecode = True

import argparse
import asyncio
import os
from pathlib import Path
import tempfile

if __package__:
    from .adapter import AdapterError, atomic_json, validate_settings
else:
    from adapter import AdapterError, atomic_json, validate_settings


class LoginError(Exception):
    pass


async def login(state_dir, qr=None, timeout_seconds=180, poll_seconds=2):
    try:
        state = Path(validate_settings({"stateDir": str(Path(state_dir).expanduser().resolve())})["stateDir"])
        state.mkdir(mode=0o700, parents=True, exist_ok=True)
        if os.name != "nt":
            state.chmod(0o700)
        if qr is None:
            from bilibili_api.login_v2 import QrCodeLogin
            qr = QrCodeLogin()
    except ImportError:
        raise LoginError("Run setup first, then login with its printed Python interpreter") from None
    except (AdapterError, OSError):
        raise LoginError("Cannot prepare the private login directory") from None
    image = state / "login-qr.png"

    async def flow():
        await qr.generate_qrcode()
        descriptor, temporary = tempfile.mkstemp(prefix=".qr-", dir=str(state))
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(qr.get_qrcode_picture().content)
            os.replace(temporary, image)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        print("Open this image and scan it with the Bilibili app: " + str(image), flush=True)
        while True:
            status = (await qr.check_state()).name
            if status == "DONE":
                credential = qr.get_credential()
                fields = {"SESSDATA": "sessdata", "bili_jct": "bili_jct", "buvid3": "buvid3",
                          "buvid4": "buvid4", "DedeUserID": "dedeuserid", "ac_time_value": "ac_time_value"}
                values = {key: getattr(credential, attribute, None) for key, attribute in fields.items()}
                cookies = {key: value for key, value in values.items() if isinstance(value, str) and value}
                if not cookies.get("SESSDATA"):
                    raise LoginError("Login returned no session; run login again")
                atomic_json(state / "credential.json", cookies)
                return {"status": "logged_in", "credentialPath": str(state / "credential.json")}
            if status == "TIMEOUT":
                raise LoginError("QR expired; run login again")
            if status not in ("SCAN", "CONF"):
                raise LoginError("Unrecognized login state; run login again")
            await asyncio.sleep(poll_seconds)

    try:
        return await asyncio.wait_for(flow(), timeout=timeout_seconds)
    except LoginError:
        raise
    except asyncio.TimeoutError:
        raise LoginError("Login timed out; run login again") from None
    except Exception:
        # Authentication/network errors can contain the QR key or cookies.
        raise LoginError("Login could not finish; check the network and run login again") from None
    finally:
        image.unlink(missing_ok=True)


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", required=True)
    args = parser.parse_args()
    try:
        asyncio.run(login(args.state_dir))
    except (LoginError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print("Login saved locally. Restart the pet if it is already running.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
