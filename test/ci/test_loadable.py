"""CI checks of the loadable extension as it ships, in a stock DuckDB (the Python package, duckdb==2.0.1).

The SQL tests link the extension statically into DuckDB's test runner, so they never load the stripped
sketchbook.duckdb_extension file the pipeline builds. This does, the way a user's DuckDB would (the Python
package loads extensions with RTLD_LOCAL, as most clients do), runs the notebook functions, and then checks
the editor server's HTTP API and its gates: the token, the Host header and the Origin header.

    python test/ci/test_loadable.py path/to/sketchbook.duckdb_extension [--allow-metadata-mismatch]

--allow-metadata-mismatch loads a local build, which is stamped with DuckDB's commit hash rather than
v2.0.1 (CLAUDE.md). CI does not pass it: a CI build that is not stamped for v2.0.1 must fail here.
Needs only the standard library and duckdb.
"""

import argparse
import http.client
import json
import os
import shutil
import sys
import tempfile
import urllib.parse

import duckdb

NOTEBOOK = """-- %% numbers
SELECT range AS n FROM range(5);

-- %% total
SELECT count(*) AS total FROM numbers;
"""

failures = []


def check(condition, message):
    print(("ok    " if condition else "FAIL  ") + message)
    if not condition:
        failures.append(message)


def quote(text):
    return "'" + text.replace("'", "''") + "'"


def connect(extension, allow_mismatch):
    config = {"allow_unsigned_extensions": "true"}
    if allow_mismatch:
        config["allow_extensions_metadata_mismatch"] = "true"
    con = duckdb.connect(config=config)
    con.execute(f"LOAD {quote(extension)}")
    return con


def test_functions(con, notebook):
    version = con.execute("SELECT version()").fetchone()[0]
    check(version == "v2.0.1", f"DuckDB is v2.0.1 (got {version})")
    loaded = con.execute("SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'sketchbook'").fetchall()
    check(loaded == [(True,)], "duckdb_extensions() lists sketchbook as loaded")
    cells = con.execute(f"SELECT name, kind FROM notebook_cells({quote(notebook)}) ORDER BY idx").fetchall()
    check(cells == [("numbers", "sql"), ("total", "sql")], f"notebook_cells reads the notebook ({cells})")
    ran = con.execute(f"SELECT name, status, row_count FROM notebook_run({quote(notebook)}) ORDER BY idx").fetchall()
    check(ran == [("numbers", "ok", 5), ("total", "ok", 1)], f"notebook_run runs every cell ({ran})")
    script = con.execute(f"SELECT * FROM notebook_sql({quote(notebook)})").fetchone()[0]
    check("FROM numbers" in script, "notebook_sql writes the notebook as a script")


class Server:
    def __init__(self, port, token):
        self.port = port
        self.token = token

    def request(self, method, path, body=None, headers=None, token=True):
        headers = dict(headers or {})
        if token:
            headers.setdefault("X-Notebook-Token", self.token)
        if body is not None:
            body = json.dumps(body)
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        try:
            # A Host header given here replaces the one http.client would send.
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, response.read().decode("utf-8", "replace")
        finally:
            connection.close()


def test_server(con, notebook):
    url, port, token = con.execute(
        f"SELECT url, port, token FROM notebook_start({quote(notebook)}, open := false)"
    ).fetchone()
    check(url.startswith(f"http://127.0.0.1:{port}/?token={token}"), f"notebook_start returns the editor's URL ({url})")
    check(len(token) >= 32, "the token is long")
    server = Server(port, token)
    nb = urllib.parse.quote(notebook, safe="")
    path = f"/api/notebook?nb={nb}"
    try:
        status, _ = server.request("GET", path, token=False)
        check(status == 403, f"no token: 403 (got {status})")
        status, _ = server.request("GET", path, headers={"X-Notebook-Token": "0" * len(token)})
        check(status == 403, f"a wrong token: 403 (got {status})")
        status, _ = server.request("GET", f"{path}&token={token}", token=False)
        check(status == 200, f"the token as a parameter: 200 (got {status})")
        status, text = server.request("GET", path, headers={"Host": f"attacker.example:{port}"})
        check(status == 403 and "host" in text.lower(), f"another Host (DNS rebinding): 403 (got {status})")
        status, text = server.request("GET", path, headers={"Origin": "http://attacker.example"})
        check(status == 403 and "origin" in text.lower(), f"another Origin: 403 (got {status})")
        status, _ = server.request(
            "POST", "/api/run", body={"nb": notebook}, headers={"Origin": f"http://127.0.0.1:{port}.attacker.example"}
        )
        check(status == 403, f"a run from another Origin: 403 (got {status})")
        status, _ = server.request("POST", "/api/run", body={"nb": notebook}, token=False)
        check(status == 403, f"a run without the token: 403 (got {status})")

        status, text = server.request(
            "GET", path, headers={"Origin": f"http://localhost:{port}", "Host": f"localhost:{port}"}
        )
        check(status == 200, f"the notebook, from the server's own origin: 200 (got {status})")
        opened = json.loads(text) if status == 200 else {}
        check(len(opened.get("cells", [])) == 2, "the notebook has its two cells")
        check(opened.get("cached") == [], "nothing is cached before the first run")

        status, text = server.request("POST", "/api/run", body={"nb": notebook})
        check(status == 200, f"a run: 200 (got {status})")
        results = json.loads(text).get("results", []) if status == 200 else []
        check(
            [r.get("status") for r in results] == ["ok", "ok"], f"every cell ran ({[r.get('status') for r in results]})"
        )
        rows = results[1].get("data", {}).get("rows") if len(results) == 2 else None
        check(rows is not None and str(rows[0][0]) == "5", f"the second cell read the first ({rows})")

        status, text = server.request("GET", path)
        cached = json.loads(text).get("cached", []) if status == 200 else []
        check(
            [(c.get("idx"), c.get("stale")) for c in cached] == [(0, False), (1, False)],
            f"the run's results are kept, fresh ({[(c.get('idx'), c.get('stale')) for c in cached]})",
        )
    finally:
        con.execute("FROM notebook_stop()").fetchall()
    try:
        server.request("GET", path)
        check(False, "the server stops")
    except OSError:
        check(True, "the server stops")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("extension")
    parser.add_argument("--allow-metadata-mismatch", action="store_true")
    args = parser.parse_args()
    work = tempfile.mkdtemp(prefix="sketchbook-ci-")
    try:
        notebook = os.path.join(work, "smoke.sql")
        with open(notebook, "w", encoding="utf-8", newline="\n") as f:
            f.write(NOTEBOOK)
        con = connect(os.path.abspath(args.extension), args.allow_metadata_mismatch)
        test_functions(con, notebook)
        test_server(con, notebook)
        con.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"{len(failures)} check(s) failed", file=sys.stderr)
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
