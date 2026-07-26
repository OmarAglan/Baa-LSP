import os
import pathlib
import subprocess
import sys


def main():
    server = pathlib.Path(sys.argv[1]).resolve()
    windows_directory = pathlib.Path(
        os.environ.get("SystemRoot", r"C:\Windows")
    )
    environment = os.environ.copy()
    environment["PATH"] = os.pathsep.join(
        [str(windows_directory / "System32"), str(windows_directory)]
    )

    result = subprocess.run(
        [str(server), "--version"],
        capture_output=True,
        check=False,
        env=environment,
        timeout=10,
    )
    stderr = result.stderr.decode("utf-8", errors="replace")
    assert result.returncode == 0, (
        f"Baa-LSP did not start without external runtime directories: "
        f"{result.returncode}: {stderr}"
    )
    assert "Baa-LSP" in stderr, stderr


if __name__ == "__main__":
    main()
