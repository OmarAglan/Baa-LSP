import json
import pathlib
import struct
import subprocess
import sys
import tempfile


def send_message(process, message):
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
    process.stdin.flush()


def read_message(process):
    headers = {}
    while True:
        line = process.stdout.readline()
        if not line:
            raise RuntimeError("Baa-LSP closed stdout before sending a response")
        if line == b"\r\n":
            break
        name, value = line.decode("ascii").split(":", 1)
        headers[name.lower()] = value.strip()
    length = int(headers["content-length"])
    body = process.stdout.read(length)
    if len(body) != length:
        raise RuntimeError("Baa-LSP returned a truncated message")
    return json.loads(body.decode("utf-8"))


def main():
    server_path = pathlib.Path(sys.argv[1]).resolve()
    compiler_path = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="baa-lsp-مسار-") as directory:
        source_path = pathlib.Path(directory, "رئيسي.baa")
        uri = source_path.as_uri()
        process = subprocess.Popen(
            [str(server_path), "--baa-path", str(compiler_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        send_message(process, {
            "jsonrpc": "2.0", "id": 1, "method": "initialize",
            "params": {"rootUri": pathlib.Path(directory).as_uri()},
        })
        response = read_message(process)
        assert response["id"] == 1
        assert response["result"]["capabilities"]["positionEncoding"] == "utf-16"

        send_message(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {
                "uri": uri, "languageId": "baa", "version": 1,
                "text": "صحيح الرئيسية() {\n    مفقود = ١.\n}\n",
            }},
        })
        diagnostics = read_message(process)
        assert diagnostics["method"] == "textDocument/publishDiagnostics"
        assert diagnostics["params"]["uri"] == uri
        assert diagnostics["params"]["version"] == 1
        assert len(diagnostics["params"]["diagnostics"]) == 1
        item = diagnostics["params"]["diagnostics"][0]
        assert item["source"] == "باء"
        assert item["range"]["start"] == {"line": 1, "character": 4}

        send_message(process, {"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
        assert read_message(process)["id"] == 2
        send_message(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
