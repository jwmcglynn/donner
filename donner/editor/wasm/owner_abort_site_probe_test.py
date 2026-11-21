"""Checks that browser GPU owner abort sites name themselves from a pthread.

The probe module is built like the editor's: threaded, with `main()` on an application pthread and
the editor's abort hook linked in. It runs under Node, where the runtime's error console is stderr.
Each owner case aborts at one site and expects the site's message, then the hook's line for the
same abort, and the hook's record of it forwarded to and kept by the main thread. A last case checks
that a thread's role does not outlive it on the worker the runtime reuses.
"""

import json
import os
import shlex
import subprocess
import sys
import unittest

from python.runfiles import runfiles


def _node_runfile(node_path):
    """Maps the toolchain's Node path, which is in an external repository, to its runfiles path."""
    prefix = "external/"
    if not node_path.startswith(prefix):
        raise ValueError("unexpected Node toolchain path: " + node_path)
    return node_path[len(prefix):]


class OwnerAbortSiteProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.r = runfiles.Create()
        # Arguments: the driver's runfiles path, then the probe module's files.
        cls.driver = cls.r.Rlocation(sys.argv[1])
        cls.probe_paths = sys.argv[2:]
        node_path = os.environ.get("DONNER_NODE_PATH")
        if not node_path:
            raise AssertionError("DONNER_NODE_PATH is unset; run this test through Bazel")
        cls.node = cls.r.Rlocation(_node_runfile(node_path))

    def _run(self, mode):
        glue = [path for path in self.probe_paths if path.endswith(".js")]
        self.assertEqual(len(glue), 1, self.probe_paths)
        env = os.environ.copy()
        node_options = [
            option
            for option in shlex.split(env.get("NODE_OPTIONS", ""))
            if option != "--jitless"
        ]
        env["NODE_OPTIONS"] = shlex.join(node_options)
        result = subprocess.run(
            [self.node, self.driver, self.r.Rlocation(glue[0]), mode],
            capture_output=True,
            env=env,
            text=True,
            timeout=60,
        )
        transcript = "exit {}\nSTDOUT:\n{}\nSTDERR:\n{}".format(
            result.returncode, result.stdout, result.stderr
        )
        errors = result.stderr.splitlines()
        records = [
            json.loads(line[7:])
            for line in result.stdout.splitlines()
            if line.startswith("RECORD ")
        ]
        return result, errors, records, transcript

    def _expect_site(self, mode, message):
        result, errors, records, transcript = self._run(mode)
        # Node exits with 1 for the uncaught RuntimeError the abort raises.
        self.assertEqual(result.returncode, 1, transcript)
        self.assertIn(message, errors, transcript)
        site_index = errors.index(message)
        hook_lines = [
            line
            for line in errors[site_index + 1:]
            if line.startswith("[donner-fatal] Aborted() thread=gpu-owner stack=")
        ]
        self.assertEqual(len(hook_lines), 1, transcript)
        self.assertEqual(
            [(record["kind"], record["thread"], record["what"]) for record in records],
            [("Aborted", "gpu-owner", "")],
            transcript,
        )

    def test_second_registration_names_the_register_site(self):
        self._expect_site(
            "register-twice",
            "[donner-fatal] Aborted at site=gpu-owner.register thread=gpu-owner stopped=0 "
            "ownerRegistered=1",
        )

    def test_dispatch_after_stop_names_its_site(self):
        self._expect_site(
            "dispatch-after-stop",
            "[donner-fatal] Aborted at site=gpu-owner.dispatch-after-stop thread=gpu-owner kind=0 "
            "requested=0 finished=0",
        )

    def test_reused_worker_does_not_inherit_a_finished_threads_role(self):
        result, errors, _, transcript = self._run("role-after-thread-reuse")
        self.assertEqual(result.returncode, 0, transcript)
        self.assertIn("role-after-thread-reuse=none", errors, transcript)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1])
