import tempfile
from unittest.mock import MagicMock
import contextlib
import shutil
import os
from pathlib import Path

from mozperftest.metadata import Metadata
from mozperftest.environment import MachEnvironment
from mozperftest.hooks import Hooks


HERE = Path(__file__).parent
EXAMPLE_TESTS_DIR = os.path.join(HERE, "data", "samples")
EXAMPLE_TEST = os.path.join(EXAMPLE_TESTS_DIR, "perftest_example.js")
EXAMPLE_XPCSHELL_TEST = Path(EXAMPLE_TESTS_DIR, "test_xpcshell.js")
BT_DATA = Path(HERE, "data", "browsertime-results", "browsertime.json")
DMG = Path(HERE, "data", "firefox.dmg")


@contextlib.contextmanager
def temp_file(name="temp", content=None):
    tempdir = tempfile.mkdtemp()
    path = os.path.join(tempdir, name)
    if content is not None:
        with open(path, "w") as f:
            f.write(content)
    try:
        yield path
    finally:
        shutil.rmtree(tempdir)


def get_running_env(**kwargs):
    from mozbuild.base import MozbuildObject

    config = MozbuildObject.from_environment()
    mach_cmd = MagicMock()

    def get_binary_path(*args):
        return ""

    mach_cmd.get_binary_path = get_binary_path
    mach_cmd.topsrcdir = config.topsrcdir
    mach_cmd.topobjdir = config.topobjdir
    mach_cmd.distdir = config.distdir
    mach_cmd._mach_context = MagicMock()
    mach_cmd._mach_context.state_dir = tempfile.mkdtemp()
    mach_cmd.run_process.return_value = 0

    mach_args = {
        "flavor": "desktop-browser",
        "test_objects": None,
        "resolve_tests": True,
        "browsertime-clobber": False,
        "browsertime-install-url": None,
    }
    mach_args.update(kwargs)
    hooks = Hooks(mach_cmd, mach_args.pop("hooks", None))
    env = MachEnvironment(mach_cmd, hooks=hooks, **mach_args)
    metadata = Metadata(mach_cmd, env, "desktop-browser")
    return mach_cmd, metadata, env


def requests_content(chunks=None):
    if chunks is None:
        chunks = [b"some ", b"content"]

    def _content(*args, **kw):
        class Resp:
            def iter_content(self, **kw):
                for chunk in chunks:
                    yield chunk

        return Resp()

    return _content
