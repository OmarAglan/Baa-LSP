import json
import os
import pathlib
import subprocess
import sys
import tempfile


def send_message(process, message):
    body = json.dumps(
        message, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    process.stdin.write(
        f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
    )
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
    for _ in range(30):
        message = read_message(process)
        if message.get("id") == request_id:
            return message
    raise RuntimeError(f"Baa-LSP did not answer request {request_id}")


def read_diagnostics(process, uri, version):
    for _ in range(30):
        message = read_message(process)
        if (
            message.get("method") == "textDocument/publishDiagnostics"
            and message.get("params", {}).get("uri") == uri
            and message.get("params", {}).get("version") == version
        ):
            return message
    raise RuntimeError("Baa-LSP did not publish expected diagnostics")


def comparable_uri(uri):
    return uri.lower() if sys.platform == "win32" else uri


def main():
    server = pathlib.Path(sys.argv[1]).resolve()
    compiler = pathlib.Path(sys.argv[2]).resolve()
    takween = pathlib.Path(sys.argv[3]).resolve()

    with tempfile.TemporaryDirectory(
        prefix="baa-lsp-project-مسار عربي-"
    ) as temporary:
        root = pathlib.Path(temporary)
        source_dir = root / "مصدر عربي"
        source_dir.mkdir()
        manifest = root / "مشروع.تكوين"
        header = source_dir / "واجهة.baahd"
        caller = source_dir / "الرئيسية.baa"
        implementation = source_dir / "الحساب.baa"

        manifest.write_text(
            """
[المشروع]
الاسم = "تطبيق"
الإصدار = "1.0.0"

[الأهداف.تطبيق]
النوع = "تنفيذي"
المدخل = "مصدر عربي/الرئيسية.baa"
يعتمد_على = ["حساب"]

[الأهداف.حساب]
النوع = "مكتبة"
المدخل = "مصدر عربي/الحساب.baa"

[البناء]
المخرج = "بناء"
""".lstrip(),
            encoding="utf-8",
        )
        header.write_text(
            "خارجي صحيح ضاعف(صحيح قيمة).\n", encoding="utf-8"
        )
        caller_text = (
            '#تضمين "واجهة.baahd"\n'
            "صحيح الرئيسية() {\n"
            "    إرجع ضاعف(٣).\n"
            "}\n"
        )
        implementation_text = (
            '#تضمين "واجهة.baahd"\n'
            "صحيح ضاعف(صحيح قيمة) {\n"
            "    إرجع قيمة * ٢.\n"
            "}\n"
            "صحيح محجوز() {\n"
            "    إرجع ٠.\n"
            "}\n"
        )
        caller.write_text(caller_text, encoding="utf-8")
        implementation.write_text(implementation_text, encoding="utf-8")

        environment = os.environ.copy()
        environment["PATH"] = (
            str(compiler.parent)
            + os.pathsep
            + environment.get("PATH", "")
        )
        process = subprocess.Popen(
            [
                str(server),
                "--baa-path",
                str(compiler),
                "--takween-path",
                str(takween),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
        )
        caller_uri = caller.resolve().as_uri()
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"rootUri": root.resolve().as_uri()},
            },
        )
        initialized = read_response(process, 1)
        assert initialized["result"]["capabilities"]["workspaceSymbolProvider"] is True
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "initialized",
                "params": {},
            },
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": caller_uri,
                        "languageId": "baa",
                        "version": 1,
                        "text": caller_text,
                    }
                },
            },
        )
        diagnostics = read_diagnostics(process, caller_uri, 1)
        assert diagnostics["params"]["diagnostics"] == []

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 20,
                "method": "workspace/symbol",
                "params": {"query": ""},
            },
        )
        project_symbols = read_response(process, 20)["result"]
        symbol_names = {item["name"] for item in project_symbols}
        assert {
            "الرئيسية",
            "ضاعف",
            "محجوز",
        }.issubset(symbol_names), project_symbols
        assert "قيمة" not in symbol_names, project_symbols
        assert {
            comparable_uri(item["location"]["uri"])
            for item in project_symbols
        } == {
            comparable_uri(caller.resolve().as_uri()),
            comparable_uri(implementation.resolve().as_uri()),
        }, project_symbols

        unsaved_caller_text = (
            caller_text
            + "صحيح غير_محفوظ() { إرجع ١. }\n"
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": caller_uri, "version": 2},
                    "contentChanges": [{"text": unsaved_caller_text}],
                },
            },
        )
        changed_diagnostics = read_diagnostics(process, caller_uri, 2)
        assert changed_diagnostics["params"]["diagnostics"] == []
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 21,
                "method": "workspace/symbol",
                "params": {"query": "غير_محفوظ"},
            },
        )
        unsaved_symbols = read_response(process, 21)["result"]
        assert len(unsaved_symbols) == 1, unsaved_symbols
        assert comparable_uri(
            unsaved_symbols[0]["location"]["uri"]
        ) == comparable_uri(caller_uri)

        call_line = unsaved_caller_text.splitlines()[2]
        call_start = call_line.index("ضاعف")
        position = {"line": 2, "character": call_start + 1}
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/definition",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                },
            },
        )
        definition = read_response(process, 2)["result"]
        assert comparable_uri(definition["uri"]) == comparable_uri(
            implementation.resolve().as_uri()
        ), definition
        assert definition["range"]["start"]["line"] == 1

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                    "context": {"includeDeclaration": True},
                },
            },
        )
        references = read_response(process, 3)["result"]
        assert {
            comparable_uri(item["uri"]) for item in references
        } == {
            comparable_uri(caller.resolve().as_uri()),
            comparable_uri(header.resolve().as_uri()),
            comparable_uri(implementation.resolve().as_uri()),
        }, references
        assert len(references) == 3, references

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/references",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                    "context": {"includeDeclaration": False},
                },
            },
        )
        uses_only = read_response(process, 4)["result"]
        assert len(uses_only) == 1
        assert comparable_uri(uses_only[0]["uri"]) == comparable_uri(caller_uri)

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/prepareRename",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                },
            },
        )
        prepared = read_response(process, 5)["result"]
        assert prepared["placeholder"] == "ضاعف"

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                    "newName": "احسب",
                },
            },
        )
        workspace_edit = read_response(process, 6)["result"]
        changes = workspace_edit["documentChanges"]
        assert {
            comparable_uri(change["textDocument"]["uri"])
            for change in changes
        } == {
            comparable_uri(caller.resolve().as_uri()),
            comparable_uri(header.resolve().as_uri()),
            comparable_uri(implementation.resolve().as_uri()),
        }, changes
        assert sum(len(change["edits"]) for change in changes) == 3, changes
        caller_change = next(
            change for change in changes
            if comparable_uri(change["textDocument"]["uri"])
            == comparable_uri(caller_uri)
        )
        assert caller_change["textDocument"]["version"] == 2
        assert all(
            edit["newText"] == "احسب"
            for change in changes
            for edit in change["edits"]
        )

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/rename",
                "params": {
                    "textDocument": {"uri": caller_uri},
                    "position": position,
                    "newName": "محجوز",
                },
            },
        )
        collision = read_response(process, 7)
        assert collision["error"]["code"] == -32602, collision

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "shutdown",
                "params": None,
            },
        )
        assert read_response(process, 8)["result"] is None
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "exit",
                "params": None,
            },
        )
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
