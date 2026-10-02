#!/usr/bin/env python3
"""Run the real GUI Vim suite with a bounded lifetime and retained diagnostics."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("executable")
parser.add_argument("--output", default="vim-test-results")
parser.add_argument("--timeout", type=int, default=180)
args = parser.parse_args()
output = Path(args.output).resolve()
output.mkdir(parents=True, exist_ok=True)
environment = os.environ.copy()
environment["TEXSTUDIO_TEST_SCREENSHOT_DIR"] = str(output)
command = [str(Path(args.executable).resolve()), "--start-always", "--config",
           str(output / "config"), "--auto-tests", "--vim-tests"]
with (output / "vim-tests.log").open("w", encoding="utf-8") as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                               env=environment, start_new_session=os.name != "nt")
    try:
        result = process.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        # Avoid GUI crash/recovery dialogs on SIGTERM; kill the whole process tree.
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], check=False)
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        sys.exit("Vim tests timed out; see vim-tests.log")
text = (output / "vim-tests.log").read_text(encoding="utf-8", errors="replace")
totals = re.findall(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped", text)
for line in text.splitlines():
    if any(word in line for word in ("Totals:", "FAIL!", "QFATAL", "desktop platform:")):
        print(line)
if result or len(totals) != 3 or any(int(failed) or int(skipped) for _, failed, skipped in totals):
    sys.exit(f"Vim tests failed or coverage was incomplete (exit {result}); see vim-tests.log")
if not (output / "vim-desktop.png").is_file():
    sys.exit("Desktop test screenshot was not generated")
print(f"Passed {sum(int(passed) for passed, _, _ in totals)} checks; logs and screenshot: {output}")
