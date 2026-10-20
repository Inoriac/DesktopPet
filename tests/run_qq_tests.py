"""Run a QQ test module or report a missing optional dependency to CTest."""
import sys
sys.dont_write_bytecode = True

from pathlib import Path
import runpy

module = sys.argv[1]
if module in ("adapter", "delivery"):
    try:
        import websockets.sync.client
        import websockets.sync.server
    except ImportError:
        print("QQ_TEST_DEPENDENCY_MISSING: choose QQ_TEST_PYTHON with plugins/qq/requirements.txt installed")
        sys.exit(77)
sys.argv = [str(Path(__file__).with_name("test_qq_" + module + ".py"))]
runpy.run_path(sys.argv[0], run_name="__main__")
