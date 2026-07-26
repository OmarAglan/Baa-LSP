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


def read_response(process, request_id):
    for _ in range(10):
        message = read_message(process)
        if message.get("id") == request_id:
            return message
    raise RuntimeError(f"Baa-LSP did not answer request {request_id}")


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
        assert response["result"]["capabilities"]["documentSymbolProvider"] is True
        assert response["result"]["capabilities"]["hoverProvider"] is True
        assert response["result"]["capabilities"]["definitionProvider"] is True
        assert response["result"]["capabilities"]["referencesProvider"] is True
        assert response["result"]["capabilities"]["renameProvider"] == {
            "prepareProvider": True
        }
        assert "signatureHelpProvider" in response["result"]["capabilities"]
        triggers = response["result"]["capabilities"]["completionProvider"]["triggerCharacters"]
        assert "ا" in triggers
        assert "#" in triggers

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

        send_message(process, {
            "jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": uri}},
        })
        symbols_response = read_message(process)
        assert symbols_response["id"] == 2
        assert len(symbols_response["result"]) == 1
        symbol = symbols_response["result"][0]
        assert symbol["name"] == "الرئيسية"
        assert symbol["kind"] == 12
        assert symbol["detail"] == "-> صحيح"
        assert symbol["selectionRange"] == {
            "start": {"line": 0, "character": 5},
            "end": {"line": 0, "character": 13},
        }

        send_message(process, {
            "jsonrpc": "2.0", "id": 5, "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": 7},
            },
        })
        completion = read_response(process, 5)["result"]
        assert completion["isIncomplete"] is False
        main_items = [item for item in completion["items"] if item["label"] == "الرئيسية"]
        assert len(main_items) == 1
        assert main_items[0]["kind"] == 3
        assert main_items[0]["detail"] == "دالة ← صحيح"
        assert main_items[0]["textEdit"] == {
            "range": {
                "start": {"line": 0, "character": 5},
                "end": {"line": 0, "character": 7},
            },
            "newText": "الرئيسية",
        }

        semantic_source = (
            "صحيح اجمع(صحيح أول، صحيح ثان) { إرجع أول + ثان. }\n"
            "صحيح الرئيسية() {\n"
            "    إرجع اجمع(١، ٢).\n"
            "}\n"
        )
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": semantic_source}],
            },
        })
        semantic_diagnostics = read_message(process)
        assert semantic_diagnostics["method"] == "textDocument/publishDiagnostics"
        assert semantic_diagnostics["params"]["version"] == 2

        send_message(process, {
            "jsonrpc": "2.0", "id": 6, "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": 7},
            },
        })
        hover_response = read_response(process, 6)
        assert "result" in hover_response, hover_response
        hover = hover_response["result"]
        assert hover["contents"]["kind"] == "markdown"
        assert "صحيح اجمع(صحيح أول، صحيح ثان)" in hover["contents"]["value"]
        assert hover["range"] == {
            "start": {"line": 0, "character": 5},
            "end": {"line": 0, "character": 9},
        }

        second_argument = semantic_source.splitlines()[2].index("٢")
        send_message(process, {
            "jsonrpc": "2.0", "id": 7, "method": "textDocument/signatureHelp",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": second_argument},
            },
        })
        signature = read_response(process, 7)["result"]
        assert signature["activeSignature"] == 0
        assert signature["activeParameter"] == 1
        assert len(signature["signatures"][0]["parameters"]) == 2

        send_message(process, {
            "jsonrpc": "2.0", "id": 8, "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
            },
        })
        definition = read_response(process, 8)["result"]
        assert definition["uri"] == uri
        assert definition["range"] == {
            "start": {"line": 0, "character": 5},
            "end": {"line": 0, "character": 9},
        }

        send_message(process, {
            "jsonrpc": "2.0", "id": 9, "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
                "context": {"includeDeclaration": False},
            },
        })
        references = read_response(process, 9)["result"]
        assert len(references) == 1
        assert references[0]["uri"] == uri
        assert references[0]["range"]["start"]["line"] == 2

        send_message(process, {
            "jsonrpc": "2.0", "id": 10,
            "method": "textDocument/prepareRename",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
            },
        })
        prepared = read_response(process, 10)["result"]
        assert prepared["placeholder"] == "اجمع"
        assert prepared["range"]["start"] == {"line": 2, "character": 9}

        send_message(process, {
            "jsonrpc": "2.0", "id": 11,
            "method": "textDocument/rename",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
                "newName": "احسب",
            },
        })
        rename = read_response(process, 11)["result"]
        assert len(rename["documentChanges"]) == 1
        assert rename["documentChanges"][0]["textDocument"] == {
            "uri": uri, "version": 2
        }
        edits = rename["documentChanges"][0]["edits"]
        assert len(edits) == 2
        assert all(edit["newText"] == "احسب" for edit in edits)

        send_message(process, {
            "jsonrpc": "2.0", "id": 12,
            "method": "textDocument/rename",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
                "newName": "latin",
            },
        })
        assert read_response(process, 12)["error"]["code"] == -32602

        send_message(process, {
            "jsonrpc": "2.0", "id": 13,
            "method": "textDocument/rename",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
                "newName": "صحيح",
            },
        })
        assert read_response(process, 13)["error"]["code"] == -32602

        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 3},
                "contentChanges": [{
                    "text": "صحيح الرئيسية() {\n    انتظر = ١.\n}\n",
                }],
            },
        })
        send_message(process, {
            "jsonrpc": "2.0", "id": 3, "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": uri}},
        })
        send_message(process, {
            "jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 3},
        })
        cancelled = read_response(process, 3)
        assert cancelled["error"]["code"] == -32800

        send_message(process, {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None})
        assert read_response(process, 4)["id"] == 4
        send_message(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
