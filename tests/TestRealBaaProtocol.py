import json
import pathlib
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
            error = process.stderr.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"Baa-LSP closed before a response: {error}")
        if line == b"\r\n":
            break
        name, value = line.decode("ascii").split(":", 1)
        headers[name.lower()] = value.strip()
    length = int(headers["content-length"])
    return json.loads(process.stdout.read(length).decode("utf-8"))


def read_response(process, request_id):
    for _ in range(10):
        message = read_message(process)
        if message.get("id") == request_id:
            return message
    raise RuntimeError(f"Baa-LSP did not answer request {request_id}")


def comparable_uri(uri):
    return uri.lower() if sys.platform == "win32" else uri


def main():
    server_path = pathlib.Path(sys.argv[1]).resolve()
    compiler_path = pathlib.Path(sys.argv[2]).resolve()
    source = (
        "صحيح اجمع(صحيح أول، صحيح ثان) { إرجع أول + ثان. }\n"
        "صحيح الرئيسية() {\n"
        "    صحيح قيمة_محلية = ١.\n"
        "    إرجع اجمع(قيمة_محلية، ٢).\n"
        "}\n"
    )

    with tempfile.TemporaryDirectory(prefix="baa-lsp-real-مسار-") as directory:
        source_path = pathlib.Path(directory, "مصدر عربي.baa")
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
        initialized = read_response(process, 1)
        assert initialized["result"]["capabilities"]["documentSymbolProvider"] is True
        assert initialized["result"]["capabilities"]["foldingRangeProvider"] is True
        assert initialized["result"]["capabilities"]["selectionRangeProvider"] is True
        assert initialized["result"]["capabilities"]["inlayHintProvider"] is True
        semantic_provider = initialized["result"]["capabilities"]["semanticTokensProvider"]
        assert semantic_provider["full"] is True
        assert semantic_provider["legend"]["tokenTypes"] == [
            "type", "macro", "keyword", "modifier",
            "comment", "string", "number", "operator",
            "function", "variable", "parameter", "property",
            "enumMember",
        ]
        assert initialized["result"]["capabilities"]["hoverProvider"] is True
        assert initialized["result"]["capabilities"]["definitionProvider"] is True
        assert initialized["result"]["capabilities"]["referencesProvider"] is True
        assert "signatureHelpProvider" in initialized["result"]["capabilities"]
        assert "ا" in initialized["result"]["capabilities"]["completionProvider"]["triggerCharacters"]
        send_message(process, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {
                "uri": uri, "languageId": "baa", "version": 1, "text": source,
            }},
        })
        diagnostics = read_message(process)
        assert diagnostics["method"] == "textDocument/publishDiagnostics"
        assert diagnostics["params"]["version"] == 1
        assert diagnostics["params"]["diagnostics"] == []

        send_message(process, {
            "jsonrpc": "2.0", "id": 19,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        })
        token_data = read_response(process, 19)["result"]["data"]
        assert len(token_data) % 5 == 0
        decoded_tokens = []
        token_line = 0
        token_character = 0
        for index in range(0, len(token_data), 5):
            delta_line, delta_character, length, token_type, modifiers = (
                token_data[index:index + 5]
            )
            token_line += delta_line
            token_character = (
                token_character + delta_character
                if delta_line == 0 else delta_character
            )
            decoded_tokens.append(
                (token_line, token_character, length, token_type, modifiers)
            )
        assert (0, 0, 4, 0, 0) in decoded_tokens
        assert any(token[0] == 0 and token[3] == 7 for token in decoded_tokens)
        assert any(token[0] == 2 and token[3] == 6 for token in decoded_tokens)
        assert any(token[3] == 8 for token in decoded_tokens)
        assert any(token[3] == 9 for token in decoded_tokens)
        assert any(token[3] == 10 for token in decoded_tokens)

        send_message(process, {
            "jsonrpc": "2.0", "id": 20,
            "method": "textDocument/foldingRange",
            "params": {"textDocument": {"uri": uri}},
        })
        folding_ranges = read_response(process, 20)["result"]
        assert folding_ranges == [{
            "startLine": 1,
            "startCharacter": 16,
            "endLine": 4,
            "endCharacter": 1,
            "kind": "region",
        }]

        local_declaration_line = source.splitlines()[2]
        local_declaration_start = local_declaration_line.index("قيمة_محلية")
        send_message(process, {
            "jsonrpc": "2.0", "id": 21,
            "method": "textDocument/selectionRange",
            "params": {
                "textDocument": {"uri": uri},
                "positions": [{
                    "line": 2,
                    "character": local_declaration_start + 1,
                }],
            },
        })
        selection = read_response(process, 21)["result"][0]
        assert selection["range"] == {
            "start": {"line": 2, "character": local_declaration_start},
            "end": {
                "line": 2,
                "character": local_declaration_start + len("قيمة_محلية"),
            },
        }
        assert "parent" in selection
        outer = selection
        while "parent" in outer:
            outer = outer["parent"]
        assert outer["range"] == {
            "start": {"line": 0, "character": 0},
            "end": {"line": 5, "character": 0},
        }

        send_message(process, {
            "jsonrpc": "2.0", "id": 2, "method": "textDocument/documentSymbol",
            "params": {"textDocument": {"uri": uri}},
        })
        symbols = read_response(process, 2)["result"]
        assert [symbol["name"] for symbol in symbols] == ["اجمع", "الرئيسية"]
        assert symbols[1]["kind"] == 12
        assert symbols[1]["selectionRange"] == {
            "start": {"line": 1, "character": 5},
            "end": {"line": 1, "character": 13},
        }


        send_message(process, {
            "jsonrpc": "2.0", "id": 4, "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 1, "character": 7},
            },
        })
        completion = read_response(process, 4)["result"]
        assert any(item["label"] == "الرئيسية" for item in completion["items"])
        assert all(item.get("filterText") != "main" for item in completion["items"])

        call_line = source.splitlines()[3]
        call_start = call_line.index("اجمع")
        local_start = call_line.index("قيمة_محلية")
        second_argument_start = call_line.index("٢")
        send_message(process, {
            "jsonrpc": "2.0", "id": 22,
            "method": "textDocument/inlayHint",
            "params": {
                "textDocument": {"uri": uri},
                "range": {
                    "start": {"line": 3, "character": call_start},
                    "end": {"line": 3, "character": len(call_line)},
                },
            },
        })
        hints = read_response(process, 22)["result"]
        assert [(hint["position"], hint["label"]) for hint in hints] == [
            ({"line": 3, "character": local_start}, "أول:"),
            ({"line": 3, "character": second_argument_start}, "ثان:"),
        ]
        assert all(hint["kind"] == 2 for hint in hints)
        assert all(hint["data"]["complete"] is True for hint in hints)
        send_message(process, {
            "jsonrpc": "2.0", "id": 18, "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {
                    "line": 3,
                    "character": local_start + len("قيمة"),
                },
            },
        })
        local_completion = read_response(process, 18)["result"]
        local_items = [
            item for item in local_completion["items"]
            if item["label"] == "قيمة_محلية"
        ]
        assert len(local_items) == 1
        assert local_items[0]["kind"] == 6
        assert local_items[0]["textEdit"]["range"] == {
            "start": {"line": 3, "character": local_start},
            "end": {
                "line": 3,
                "character": local_start + len("قيمة"),
            },
        }
        assert local_items[0]["data"]["source"] == "baa-compiler"
        send_message(process, {
            "jsonrpc": "2.0", "id": 5, "method": "textDocument/hover",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": call_start + 1},
            },
        })
        hover = read_response(process, 5)["result"]
        assert hover["range"] == {
            "start": {"line": 3, "character": call_start},
            "end": {"line": 3, "character": call_start + len("اجمع")},
        }
        assert "صحيح اجمع(صحيح أول، صحيح ثان)" in hover["contents"]["value"]

        send_message(process, {
            "jsonrpc": "2.0", "id": 6, "method": "textDocument/signatureHelp",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": call_line.index("٢")},
            },
        })
        signature = read_response(process, 6)["result"]
        assert signature["activeParameter"] == 1
        assert signature["signatures"][0]["label"] == (
            "صحيح اجمع(صحيح أول، صحيح ثان)"
        )
        assert [
            item["label"] for item in signature["signatures"][0]["parameters"]
        ] == ["صحيح أول", "صحيح ثان"]

        send_message(process, {
            "jsonrpc": "2.0", "id": 7, "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": call_start + 1},
            },
        })
        definition = read_response(process, 7)["result"]
        assert definition["uri"] == uri
        assert definition["range"] == {
            "start": {"line": 0, "character": 5},
            "end": {"line": 0, "character": 9},
        }

        send_message(process, {
            "jsonrpc": "2.0", "id": 8, "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 3, "character": call_start + 1},
                "context": {"includeDeclaration": True},
            },
        })
        references = read_response(process, 8)["result"]
        assert len(references) == 2
        assert {item["range"]["start"]["line"] for item in references} == {0, 3}

        header = pathlib.Path(directory, "واجهة عربية.baahd")
        header.write_text(
            "خارجي صحيح ضاعف(صحيح قيمة).\n",
            encoding="utf-8",
        )
        included_source = (
            '#تضمين "واجهة عربية.baahd"\n'
            "صحيح الرئيسية() {\n"
            "    إرجع ضاعف(٣).\n"
            "}\n"
        )
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 2},
                "contentChanges": [{"text": included_source}],
            },
        })
        included_diagnostics = read_message(process)
        assert included_diagnostics["method"] == "textDocument/publishDiagnostics"
        assert included_diagnostics["params"]["version"] == 2
        assert included_diagnostics["params"]["diagnostics"] == []

        included_call = included_source.splitlines()[2].index("ضاعف")
        send_message(process, {
            "jsonrpc": "2.0", "id": 19, "method": "textDocument/completion",
            "params": {
                "textDocument": {"uri": uri},
                "position": {
                    "line": 2,
                    "character": included_call + len("ضا"),
                },
            },
        })
        included_completion = read_response(process, 19)["result"]
        included_items = [
            item for item in included_completion["items"]
            if item["label"] == "ضاعف"
        ]
        assert len(included_items) == 1
        assert included_items[0]["kind"] == 3
        assert included_items[0]["data"]["source"] == "baa-compiler"
        send_message(process, {
            "jsonrpc": "2.0", "id": 9, "method": "textDocument/definition",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": included_call + 1},
            },
        })
        included_definition = read_response(process, 9)["result"]
        assert comparable_uri(included_definition["uri"]) == comparable_uri(
            header.resolve().as_uri()
        )
        assert included_definition["range"]["start"]["line"] == 0

        send_message(process, {
            "jsonrpc": "2.0", "id": 10, "method": "textDocument/references",
            "params": {
                "textDocument": {"uri": uri},
                "position": {"line": 2, "character": included_call + 1},
                "context": {"includeDeclaration": True},
            },
        })
        included_references = read_response(process, 10)["result"]
        assert {comparable_uri(item["uri"]) for item in included_references} == {
            comparable_uri(uri), comparable_uri(header.resolve().as_uri())
        }

        missing_dot_source = (
            "صحيح الرئيسية() {\n"
            "    إرجع ٠\n"
            "}\n"
        )
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 3},
                "contentChanges": [{"text": missing_dot_source}],
            },
        })
        fix_diagnostics = read_message(process)
        assert fix_diagnostics["method"] == "textDocument/publishDiagnostics"
        assert fix_diagnostics["params"]["version"] == 3
        diagnostic = fix_diagnostics["params"]["diagnostics"][0]
        assert diagnostic["code"] == "B0001"
        assert diagnostic["data"]["fixes"][0]["id"] == "B0001.insert-dot"

        send_message(process, {
            "jsonrpc": "2.0", "id": 11,
            "method": "textDocument/codeAction",
            "params": {
                "textDocument": {"uri": uri},
                "range": diagnostic["range"],
                "context": {
                    "diagnostics": [diagnostic],
                    "only": ["quickfix"],
                },
            },
        })
        actions = read_response(process, 11)["result"]
        assert len(actions) == 1
        assert actions[0]["title"] == "أضف '.'"
        assert actions[0]["isPreferred"] is True
        document_change = actions[0]["edit"]["documentChanges"][0]
        assert document_change["textDocument"] == {"uri": uri, "version": 3}
        assert document_change["edits"] == [{
            "range": {
                "start": {"line": 2, "character": 0},
                "end": {"line": 2, "character": 0},
            },
            "newText": ".",
        }]

        messy_source = "صحيح الرئيسية(){إرجع ٠.}\n"
        send_message(process, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {
                "textDocument": {"uri": uri, "version": 4},
                "contentChanges": [{"text": messy_source}],
            },
        })
        formatted_diagnostics = read_message(process)
        assert formatted_diagnostics["method"] == "textDocument/publishDiagnostics"
        assert formatted_diagnostics["params"]["version"] == 4
        assert formatted_diagnostics["params"]["diagnostics"] == []

        send_message(process, {
            "jsonrpc": "2.0", "id": 12,
            "method": "textDocument/formatting",
            "params": {
                "textDocument": {"uri": uri},
                "options": {"tabSize": 2, "insertSpaces": False},
            },
        })
        formatting = read_response(process, 12)["result"]
        assert formatting == [{
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 1, "character": 0},
            },
            "newText": "صحيح الرئيسية() {\n    إرجع ٠.\n}\n",
        }]

        send_message(process, {
            "jsonrpc": "2.0", "id": 3, "method": "shutdown", "params": None,
        })
        assert read_response(process, 3)["result"] is None
        send_message(process, {"jsonrpc": "2.0", "method": "exit", "params": None})
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
