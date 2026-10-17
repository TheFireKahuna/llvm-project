"""Runs a program and prints its standard output, then its exit code.

Usage: exit_status.py <program> [arguments...]
"""

import subprocess
import sys

result = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, text=True)
sys.stdout.write(result.stdout)
print("exit code", result.returncode)
