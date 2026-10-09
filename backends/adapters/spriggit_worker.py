"""Convert staged plugin data outside this worker's inherited MO2 hooks.

Only this disposable worker disconnects. The editor and parent adapter retain
MO2's virtual filesystem; the parent materializes inputs and publishes outputs.
"""
import ctypes
import os
import subprocess
import sys


def main():
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
        kernel.GetModuleHandleW.restype = ctypes.c_void_p
        if kernel.GetModuleHandleW('usvfs_x64.dll'):
            vfs = ctypes.WinDLL('usvfs_x64.dll')
            disconnect = vfs.usvfsDisconnectVFS
            disconnect.argtypes = []
            disconnect.restype = None
            disconnect()
    try:
        result = subprocess.run(sys.argv[1:], timeout=170,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    except subprocess.TimeoutExpired:
        print('Spriggit timed out after 170 seconds.', file=sys.stderr)
        return 1
    except OSError as error:
        print(f'Could not start Spriggit: {error}', file=sys.stderr)
        return 1
    if result.returncode:
        print(f'Spriggit exited with code 0x{result.returncode & 0xffffffff:08X}.', file=sys.stderr)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
