"""Exercise EXE startup after removing its manifest request from a test copy."""

import pathlib
import struct
import subprocess
import sys

source, destination, dll = sys.argv[1:]
image = bytearray(pathlib.Path(source).read_bytes())
pe = struct.unpack_from("<I", image, 0x3C)[0]
assert image[pe : pe + 4] == b"PE\0\0"
optional = pe + 24
assert struct.unpack_from("<H", image, optional)[0] == 0x20B
# Clear only the resource data-directory entry. Removing .rsrc with objcopy
# can also invalidate the image layout, which would test the loader instead.
struct.pack_into("<II", image, optional + 112 + 2 * 8, 0, 0)
pathlib.Path(destination).write_bytes(image)
result = subprocess.run([destination, dll], capture_output=True, timeout=30)
# Windows policy may select segment heap even without a manifest. In that case
# main itself verifies segment heap and the EXE/DLL/UCRT identity. Otherwise the
# production startup gate must diagnose and terminate before main runs.
if result.returncode:
    assert result.returncode == 255, result
    assert b"wincrt: executable requires the Windows segment heap; " in result.stderr
else:
    assert not result.stderr, result
