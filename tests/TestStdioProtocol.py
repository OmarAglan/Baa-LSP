import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import time


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
        source_path = pathlib.Path(directory, "رئيسي.باء")
        include_directory = pathlib.Path(directory, "واجهات")
        include_directory.mkdir()
        pathlib.Path(include_directory, "حساب.رأسباء").write_text(
            "خارجي صحيح اجمع(صحيح أ، صحيح ب).\n", encoding="utf-8")
        pathlib.Path(include_directory, "قديم.baahd").write_text(
            "خارجي صحيح قديم().\n", encoding="utf-8")
        pathlib.Path(include_directory, "ليس_مصدر.txt").write_text(
            "ignored\n", encoding="utf-8")
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
        assert response["result"]["capabilities"]["foldingRangeProvider"] is True
        assert response["result"]["capabilities"]["selectionRangeProvider"] is True
        assert response["result"]["capabilities"]["inlayHintProvider"] is True
        assert response["result"]["capabilities"]["semanticTokensProvider"] == {
            "legend": {
                "tokenTypes": [
                    "type", "macro", "keyword", "modifier",
                    "comment", "string", "number", "operator",
                    "function", "variable", "parameter", "property",
                    "enumMember",
                ],
                "tokenModifiers": [],
            },
            "full": True,
        }
        assert response["result"]["capabilities"]["workspaceSymbolProvider"] is True
        assert response["result"]["capabilities"]["hoverProvider"] is True
        assert response["result"]["capabilities"]["definitionProvider"] is True
        assert response["result"]["capabilities"]["referencesProvider"] is True
        assert response["result"]["capabilities"]["documentFormattingProvider"] is True
        assert response["result"]["capabilities"]["codeActionProvider"] == {
            "codeActionKinds": ["quickfix"],
            "resolveProvider": False,
        }
        assert response["result"]["capabilities"]["renameProvider"] == {
            "prepareProvider": True
        }
        assert "signatureHelpProvider" in response["result"]["capabilities"]
        completion_provider = response["result"]["capabilities"]["completionProvider"]
        assert completion_provider["resolveProvider"] is True
        triggers = completion_provider["triggerCharacters"]
        assert "ا" in triggers
        assert "#" in triggers
        assert "/" in triggers

        send_message(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {
                "uri": uri, "languageId": "baa", "version": 1,
                "text": "صحيح الرئيسية(){\n    مفقود = ١.\n}\n",
            }},
        })
        diagnostics = read_message(process)
        assert diagnostics["method"] == "textDocument/publishDiagnostics", diagnostics
        assert diagnostics["params"]["uri"] == uri
        assert diagnostics["params"]["version"] == 1
        assert len(diagnostics["params"]["diagnostics"]) == 1
        item = diagnostics["params"]["diagnostics"][0]
        assert item["source"] == "باء"
        assert item["range"]["start"] == {"line": 1, "character": 4}
        assert item["data"]["fixes"][0]["title"] == "عرّف المتغير بإضافة نوعه"

        send_message(process, {
            "jsonrpc": "2.0", "id": 21,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        })
        semantic_tokens = read_response(process, 21)["result"]["data"]
        assert semantic_tokens == [
            0, 0, 4, 0, 0,
            1, 12, 1, 6, 0,
        ]

        send_message(process, {
            "jsonrpc": "2.0", "id": 22,
            "method": "textDocument/foldingRange",
            "params": {"textDocument": {"uri": uri}},
        })
        folding_ranges = read_response(process, 22)["result"]
        assert folding_ranges == [{
            "startLine": 0,
            "startCharacter": 15,
            "endLine": 2,
            "endCharacter": 1,
            "kind": "region",
        }]

        send_message(process, {
            "jsonrpc": "2.0", "id": 23,
            "method": "textDocument/selectionRange",
            "params": {
                "textDocument": {"uri": uri},
                "positions": [{"line": 1, "character": 4}],
            },
        })
        selection = read_response(process, 23)["result"][0]
        assert selection["range"]["start"] == {"line": 0, "character": 16}
        assert selection["range"]["end"] == {"line": 2, "character": 0}
        assert selection["parent"]["range"]["start"] == {
            "line": 0, "character": 15,
        }
        outer = selection
        while "parent" in outer:
            outer = outer["parent"]
        assert outer["range"] == {
            "start": {"line": 0, "character": 0},
            "end": {"line": 3, "character": 0},
        }

        send_message(process, {
            "jsonrpc": "2.0", "id": 24,
            "method": "textDocument/selectionRange",
            "params": {
                "textDocument": {"uri": uri},
                "positions": [{"line": 0, "character": 2147483648}],
            },
        })
        oversized_position = read_response(process, 24)
        assert oversized_position["error"]["code"] == -32602

        send_message(process, {
            "jsonrpc": "2.0", "id": 14, "method": "textDocument/codeAction",
            "params": {
                "textDocument": {"uri": uri},
                "range": item["range"],
                "context": {
                    "diagnostics": [item],
                    "only": ["quickfix"],
                },
            },
        })
        actions = read_response(process, 14)["result"]
        assert len(actions) == 1
        action = actions[0]
        assert action["title"] == "عرّف المتغير بإضافة نوعه"
        assert action["kind"] == "quickfix"
        assert action["isPreferred"] is True
        assert action["data"]["fixId"] == "E100.insert-int-type"
        change = action["edit"]["documentChanges"][0]
        assert change["textDocument"] == {"uri": uri, "version": 1}
        assert change["edits"] == [{
            "range": {
                "start": {"line": 1, "character": 4},
                "end": {"line": 1, "character": 4},
            },
            "newText": "صحيح ",
        }]

        send_message(process, {
            "jsonrpc": "2.0", "id": 15, "method": "textDocument/codeAction",
            "params": {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 0, "character": 0},
                },
                "context": {
                    "diagnostics": [],
                    "only": ["quickfix"],
                },
            },
        })
        assert read_response(process, 15)["result"] == []

        send_message(process, {
            "jsonrpc": "2.0", "id": 16,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 8, "insertSpaces": False},
            },
        })
        formatting = read_response(process, 16)["result"]
        assert formatting == [{
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 3, "character": 0},
            },
            "newText": (
                "صحيح الرئيسية() {\n"
                "    مفقود = ١.\n"
                "}\n"
            ),
        }]

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
            "jsonrpc": "2.0", "id": 19, "method": "workspace/symbol",
            "params": {"query": "الرئ"},
        })
        workspace_symbols = read_response(process, 19)["result"]
        assert len(workspace_symbols) == 1
        assert workspace_symbols[0]["name"] == "الرئيسية"
        assert workspace_symbols[0]["kind"] == 12
        assert workspace_symbols[0]["location"] == {
            "uri": uri,
            "range": {
                "start": {"line": 0, "character": 5},
                "end": {"line": 0, "character": 13},
            },
        }
        assert workspace_symbols[0]["data"]["baaKind"] == "function"
        send_message(process, {
            "jsonrpc": "2.0", "id": 20, "method": "workspace/symbol",
            "params": {"query": "غير_موجود"},
        })
        assert read_response(process, 20)["result"] == []

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
        assert main_items[0]["detail"] == "صحيح الرئيسية()"
        assert main_items[0]["data"]["source"] == "baa-compiler"
        assert main_items[0]["textEdit"] == {
            "range": {
                "start": {"line": 0, "character": 5},
                "end": {"line": 0, "character": 7},
            },
            "newText": "الرئيسية",
        }
        send_message(process, {
            "jsonrpc": "2.0", "id": 17, "method": "completionItem/resolve",
            "params": main_items[0],
        })
        resolved = read_response(process, 17)["result"]
        assert resolved["documentation"] == {
            "kind": "markdown",
            "value": "دالة باء ونقطة بدء البرنامج.",
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
            "jsonrpc": "2.0", "id": 25,
            "method": "textDocument/inlayHint",
            "params": {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 2, "character": 0},
                    "end": {"line": 2, "character": 19},
                },
            },
        })
        hints = read_response(process, 25)["result"]
        assert hints == [
            {
                "position": {"line": 2, "character": 14},
                "label": "أول:",
                "kind": 2,
                "paddingRight": True,
                "data": {
                    "schema_version": "inlay-hints-json-v1",
                    "parameter": "أول",
                    "complete": True,
                },
            },
            {
                "position": {"line": 2, "character": 17},
                "label": "ثان:",
                "kind": 2,
                "paddingRight": True,
                "data": {
                    "schema_version": "inlay-hints-json-v1",
                    "parameter": "ثان",
                    "complete": True,
                },
            },
        ]

        send_message(process, {
            "jsonrpc": "2.0", "id": 26,
            "method": "textDocument/inlayHint",
            "params": {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 2, "character": 17},
                    "end": {"line": 2, "character": 18},
                },
            },
        })
        filtered_hints = read_response(process, 26)["result"]
        assert [hint["label"] for hint in filtered_hints] == ["ثان:"]

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

        # A completion query intentionally skips the project index. A
        # navigation request at the identical cursor must upgrade that cached
        # result before references and rename reuse it.
        send_message(process, {
            "jsonrpc": "2.0", "id": 18,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": 12},
            },
        })
        cached_completion = read_response(process, 18)["result"]
        assert cached_completion["isIncomplete"] is False

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
        # Wait until the fake compiler proves that it entered its five-second
        # symbol-analysis delay. Cancellation must terminate that active
        # process, not only discard a queued reply.
        active_marker = pathlib.Path(directory, ".baa-lsp-active")
        marker_deadline = time.monotonic() + 3.0
        while not active_marker.exists() and time.monotonic() < marker_deadline:
            time.sleep(0.02)
        assert active_marker.exists()
        send_message(process, {
            "jsonrpc": "2.0", "method": "$/cancelRequest", "params": {"id": 3},
        })
        cancelled = read_response(process, 3)
        assert cancelled["error"]["code"] == -32800

        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 4},
                "contentChanges": [{
                    "text": "صحيح الرئيسية() {\n    إرجع ٠.\n}\n",
                }],
            },
        })
        recovery_started = time.monotonic()
        send_message(process, {
            "jsonrpc": "2.0", "id": 30,
            "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": uri}},
        })
        recovered = read_response(process, 30)
        assert recovered["result"][0]["name"] == "الرئيسية"
        assert time.monotonic() - recovery_started < 3.0

        delayed_inlay_source = (
            "صحيح اجمع(صحيح أول، صحيح ثان) { إرجع أول + ثان. }\n"
            "صحيح الرئيسية() { انتظر. إرجع اجمع(١، ٢). }\n"
        )
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 5},
                "contentChanges": [{"text": delayed_inlay_source}],
            },
        })
        send_message(process, {
            "jsonrpc": "2.0", "id": 31,
            "method": "textDocument/inlayHint",
            "params": {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 0, "character": 0},
                    "end": {"line": 2, "character": 0},
                },
            },
        })
        inlay_marker = pathlib.Path(directory, ".baa-lsp-inlay-active")
        marker_deadline = time.monotonic() + 3.0
        while not inlay_marker.exists() and time.monotonic() < marker_deadline:
            time.sleep(0.02)
        assert inlay_marker.exists()
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 6},
                "contentChanges": [{
                    "text": "صحيح الرئيسية() { إرجع ٠. }\n",
                }],
            },
        })
        stale_inlay = read_response(process, 31)
        assert stale_inlay["error"]["code"] == -32801

        # Qalam inserts both quotes and leaves the caret between them. The
        # closing quote after the caret must not turn this into normal language
        # completion.
        paired_include_source = '#تضمين ""'
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 7},
                "contentChanges": [{"text": paired_include_source}],
            },
        })
        send_message(process, {
            "jsonrpc": "2.0", "id": 69,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": len('#تضمين "')},
            },
        })
        paired_items = read_response(process, 69)["result"]["items"]
        assert any(item["label"] == "واجهات/" and item["kind"] == 19
                   for item in paired_items)
        assert all(item["kind"] in {17, 19} for item in paired_items)
        assert all(item["data"]["source"] == "baa-lsp-include"
                   for item in paired_items)

        include_source = '#تضمين "واج'
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 8},
                "contentChanges": [{"text": include_source}],
            },
        })
        send_message(process, {
            "jsonrpc": "2.0", "id": 70,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": len(include_source)},
            },
        })
        directory_completion = read_response(process, 70)["result"]
        assert directory_completion["isIncomplete"] is False
        assert directory_completion["items"] == [{
            "label": "واجهات/",
            "kind": 19,
            "detail": "افتح المجلد لعرض ملفات التضمين",
            "filterText": "واجهات",
            "insertTextFormat": 1,
            "sortText": "0واجهات",
            "textEdit": {
                "range": {
                    "start": {"line": 0, "character": len('#تضمين "')},
                    "end": {"line": 0, "character": len(include_source)},
                },
                "newText": "واجهات/",
            },
            "data": {"source": "baa-lsp-include"},
        }]

        nested_source = '#تضمين "واجهات/'
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 9},
                "contentChanges": [{"text": nested_source}],
            },
        })
        send_message(process, {
            "jsonrpc": "2.0", "id": 71,
            "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 0, "character": len(nested_source)},
            },
        })
        file_items = read_response(process, 71)["result"]["items"]
        assert [item["label"] for item in file_items] == [
            "حساب.رأسباء", "قديم.baahd",
        ]
        assert all(item["kind"] == 17 for item in file_items)
        assert all(item["data"]["source"] == "baa-lsp-include" for item in file_items)

        send_message(process, {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None})
        assert read_response(process, 4)["id"] == 4
        send_message(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
