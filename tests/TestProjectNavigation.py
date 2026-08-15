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


def read_response(process, request_id, observed=None):
    for _ in range(30):
        message = read_message(process)
        if observed is not None:
            observed.append(message)
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
        refreshed_source = source_dir / "الإضافي.baa"

        second_root = root / "مساحة ثانية"
        second_source_dir = second_root / "مصدر"
        second_source_dir.mkdir(parents=True)
        second_manifest = second_root / "مشروع.تكوين"
        second_source = second_source_dir / "الثاني.baa"

        initial_manifest = """
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
""".lstrip()
        manifest.write_text(initial_manifest, encoding="utf-8")
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
        refreshed_source.write_text(
            "صحيح بعد_التحديث() { إرجع ٧. }\n", encoding="utf-8"
        )
        second_manifest.write_text(
            """
[المشروع]
الاسم = "ثانوي"
الإصدار = "1.0.0"

[الأهداف.ثانوي]
النوع = "تنفيذي"
المدخل = "مصدر/الثاني.baa"

[البناء]
المخرج = "بناء"
""".lstrip(),
            encoding="utf-8",
        )
        second_source.write_text(
            "صحيح من_المساحة_الثانية() { إرجع ٢. }\n",
            encoding="utf-8",
        )

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
                "params": {
                    "rootUri": root.resolve().as_uri(),
                    "initializationOptions": {
                        "baaStructuredLogs": {
                            "schemaVersion": "baa-lsp-log-v1"
                        }
                    },
                },
            },
        )
        initialized = read_response(process, 1)
        assert initialized["result"]["capabilities"]["workspaceSymbolProvider"] is True
        log_capability = initialized["result"]["capabilities"]["experimental"][
            "baaLogEvent"
        ]
        assert log_capability == {
            "schemaVersion": "baa-lsp-log-v1",
            "transport": "local-stdio",
            "telemetry": False,
        }
        workspace_capability = initialized["result"]["capabilities"]["workspace"]
        assert workspace_capability["workspaceFolders"]["supported"] is True
        assert workspace_capability["workspaceFolders"]["changeNotifications"] is True
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

        refreshed_manifest = initial_manifest.replace(
            'يعتمد_على = ["حساب"]',
            'يعتمد_على = ["حساب", "إضافي"]',
        ).replace(
            '[البناء]\nالمخرج = "بناء"',
            '[الأهداف.إضافي]\n'
            'النوع = "مكتبة"\n'
            'المدخل = "مصدر عربي/الإضافي.baa"\n\n'
            '[البناء]\nالمخرج = "بناء"',
        )
        manifest.write_text(refreshed_manifest, encoding="utf-8")
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWatchedFiles",
                "params": {
                    "changes": [{"uri": manifest.resolve().as_uri(), "type": 2}]
                },
            },
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 22,
                "method": "workspace/symbol",
                "params": {"query": "بعد_التحديث"},
            },
        )
        refreshed_symbols = read_response(process, 22)["result"]
        assert len(refreshed_symbols) == 1, refreshed_symbols
        assert comparable_uri(
            refreshed_symbols[0]["location"]["uri"]
        ) == comparable_uri(refreshed_source.resolve().as_uri())

        second_folder = {
            "uri": second_root.resolve().as_uri(),
            "name": second_root.name,
        }
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWorkspaceFolders",
                "params": {
                    "event": {"added": [second_folder], "removed": []}
                },
            },
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 23,
                "method": "workspace/symbol",
                "params": {"query": "من_المساحة_الثانية"},
            },
        )
        second_symbols = read_response(process, 23)["result"]
        assert len(second_symbols) == 1, second_symbols
        assert comparable_uri(
            second_symbols[0]["location"]["uri"]
        ) == comparable_uri(second_source.resolve().as_uri())

        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWorkspaceFolders",
                "params": {
                    "event": {"added": [], "removed": [second_folder]}
                },
            },
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 24,
                "method": "workspace/symbol",
                "params": {"query": "من_المساحة_الثانية"},
            },
        )
        assert read_response(process, 24)["result"] == []

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

        manifest.write_text("[بيان غير صالح\n", encoding="utf-8")
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "method": "workspace/didChangeWatchedFiles",
                "params": {
                    "changes": [{"uri": manifest.resolve().as_uri(), "type": 2}]
                },
            },
        )
        send_message(
            process,
            {
                "jsonrpc": "2.0",
                "id": 25,
                "method": "workspace/symbol",
                "params": {"query": "بعد_التحديث"},
            },
        )
        reload_messages = []
        assert read_response(process, 25, reload_messages)["result"] == []
        reload_events = [
            message["params"]
            for message in reload_messages
            if message.get("method") == "baa/logEvent"
        ]
        assert reload_events, reload_messages
        assert all(
            message.get("method") != "telemetry/event"
            for message in reload_messages
        )
        failure = next(
            event
            for event in reload_events
            if event.get("event") == "workspace.plan.failed"
        )
        assert failure["schema_version"] == "baa-lsp-log-v1"
        assert failure["sequence"] > 0
        assert failure["severity"] == "warning"
        assert failure["component"] == "workspace"
        assert failure["message"] == (
            "Takween failed while loading the project plan."
        )
        assert isinstance(failure["data"].get("exit_code"), int)
        assert len(failure) == 7

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
