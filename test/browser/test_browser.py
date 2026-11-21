"""End-to-end tests in a real browser: the notebook server and UI, and every export format.

Needs the release build, Python with Playwright, and Chromium (`playwright install chromium`).
Run with `make browser_test`. Set NOTEBOOK_WASM_RUNTIME to a built runtime (sh wasm/build.sh)
to include the html-wasm export.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DUCKDB = os.path.join(ROOT, "build", "release", "duckdb")
failures = []


def check(condition, message):
    print(("ok   " if condition else "FAIL ") + message)
    if not condition:
        failures.append(message)


def sql(db, *statements):
    """Runs statements in a fresh DuckDB shell and returns its output."""
    args = [DUCKDB, "-init", "/dev/null", "-noheader", "-list"]
    for statement in statements:
        args += ["-c", statement]
    result = subprocess.run(args + [db], capture_output=True, text=True, cwd=ROOT, env=os.environ)
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout)
    return result.stdout


# Shells running a server that have not been stopped yet; main() stops them if a test fails part-way.
running = []


def serve(work, notebook, db=None, setup=None):
    """Starts the notebook server in a DuckDB shell (in-memory, or on `db`) and returns (shell, url).
    `setup` is SQL the shell runs first. Stop it with stop(shell)."""
    args = [DUCKDB, "-init", "/dev/null", "-noheader", "-list"] + ([db] if db else [])
    shell = subprocess.Popen(
        args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=work, bufsize=1
    )
    running.append(shell)
    if setup:
        shell.stdin.write(setup + "\n")
    shell.stdin.write(f"SELECT url FROM notebook_start('{notebook}', open := false);\n")
    shell.stdin.flush()
    return shell, shell.stdout.readline().strip()


def stop(shell):
    """Stops the server and quits the shell; returns what the shell printed. Kills a shell that does not quit."""
    running.remove(shell)
    try:
        shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
        shell.stdin.flush()
        return shell.communicate(timeout=30)[0]
    except Exception:
        shell.kill()
        shell.communicate()
        raise


def still(url):
    """The page with the handwriting animation off (render-ink.js): the checks here are about the finished
    page. test_ink checks the animation itself."""
    return url + ("&" if "?" in url else "?") + "ink=off"


def watch(page):
    errors = []
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.on("pageerror", lambda e: errors.append(str(e)))
    return errors


def test_server(browser, work):
    notebook = os.path.join(work, "revenue.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    db = os.path.join(work, "server.duckdb")
    shell, url = serve(work, notebook, db)
    check(url.startswith("http://127.0.0.1:"), f"notebook_start returns a URL ({url[:40]}...)")

    page = browser.new_page(viewport={"width": 1400, "height": 1000})
    errors = watch(page)
    page.goto(still(url))
    page.wait_for_selector(".nb-cell")
    check(page.locator("#title").text_content() == "Revenue review", "the editor shows the notebook's title")
    check(page.locator(".nb-cell").count() == 11, "one editor cell per notebook cell")
    check(page.locator(".nb-badge.fresh").count() == 0, "opening a notebook does not run it")

    check(not page.locator("#menu").is_visible(), "options live behind the gear, closed by default")

    # SQL highlighting: a coloured copy behind each SQL text area, with DuckDB's own keywords.
    filtered_code = page.locator(".nb-cell[data-idx='6']")
    hl, ta = filtered_code.locator(".nb-code-hl"), filtered_code.locator("textarea")
    check(
        hl.locator(".tk-kw").first.text_content() == "SELECT"
        and hl.locator(".tk-var").first.text_content() == "$region"
        and hl.locator(".tk-name").first.text_content() == "orders",
        "SQL is highlighted: keywords, $variables and cell names",
    )
    check(
        hl.evaluate("e => e.textContent") == ta.input_value() + "\n"
        and abs(hl.bounding_box()["height"] - ta.bounding_box()["height"]) < 1
        and ta.evaluate("e => getComputedStyle(e).color") == "rgba(0, 0, 0, 0)",
        "the highlighted copy matches the text area exactly, behind transparent text",
    )
    tokens = page.evaluate(
        """() => NB.tokenizeSql("SELECT $$a -- b$$ /* x /* y */ z */ 'it''s' $v collate day sum(n) t.select AS orders FROM orders",
      {names: {orders: true}}).filter(t => t.type !== 'ws').map(t => t.type)"""
    )
    check(
        tokens
        == [
            "kw",
            "str",
            "comment",
            "str",
            "var",
            "kw",
            "ident",
            "fn",
            "op",
            "ident",
            "op",
            "ident",
            "op",
            "ident",
            "kw",
            "ident",
            "kw",
            "name",
        ],
        f"the tokenizer follows DuckDB's lexer and keyword list ({tokens})",
    )
    classes = page.evaluate(
        "() => ['INTERVAL', 'INTEGER[]', 'POINT', 'INT', 'DECIMAL(9,2)', 'TIMESTAMP WITH TIME ZONE', 'VARCHAR'].map(NB.typeClass)"
    )
    check(
        classes == ["nominal", "nominal", "nominal", "quantitative", "quantitative", "temporal", "nominal"],
        f"column types are classed by their whole name, as the chart compiler does ({classes})",
    )
    page.click("#gear")
    check(
        page.locator("#menu").is_visible() and page.locator("#view-graph").is_visible(),
        "the gear opens the options menu",
    )
    page.keyboard.press("Escape")
    check(not page.locator("#menu").is_visible(), "Escape closes the options menu")

    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-badge.failed").count() == 0, "Run all: no cell fails")
    check(page.locator("svg.nb-chart-svg").count() >= 2, "Run all draws the charts")
    check("EU" in page.locator(".nb-md").first.text_content(), "markdown shows {{ $region }}")
    foot = page.locator(".nb-cell[data-idx='6'] .nb-foot")
    check(
        foot.count() == 1
        and "rows" in foot.text_content()
        and "defines filtered" in foot.text_content()
        and foot.locator(".nb-time").count() == 1
        and "rows" not in page.locator(".nb-cell[data-idx='6'] .nb-meta").text_content(),
        "rows, what a cell defines and its run time sit together in the bottom right of its box",
    )
    kinds = page.locator(".nb-kind").all()
    check(
        [k.text_content() for k in kinds[:8]] == ["☰", "¶", "✎", "▦", "◉", "◉", "▦", "▦"]
        and "Query" in kinds[3].get_attribute("title"),
        "cell kinds are symbols, named in their tooltips",
    )
    # Margins: kind and name to the left of a cell, state and buttons to the right, frame as a bracket.
    cell6 = page.locator(".nb-cell[data-idx='6']")
    cb, mb, hb = (
        cell6.bounding_box(),
        cell6.locator(".nb-margin").bounding_box(),
        cell6.locator(".nb-cell-head").bounding_box(),
    )
    check(
        mb["x"] + mb["width"] <= cb["x"]
        and hb["x"] >= cb["x"] + cb["width"]
        and cell6.locator(".nb-name").text_content() == "filtered",
        "a cell's kind and name sit in its left margin, its state and buttons in its right",
    )
    check(
        cell6.locator("svg.sk-outline").evaluate("e => getComputedStyle(e).clipPath") != "none",
        "a cell's frame is drawn as a bracket on its left",
    )
    # Buttons: a horizontal strip on the top edge, shown on hover, reachable however short the cell.
    tools = page.locator(".nb-cell[data-idx='3'] .nb-tools")
    check(
        tools.evaluate("e => getComputedStyle(e).opacity") == "0",
        "a cell's buttons are hidden until the pointer is on it",
    )
    page.locator(".nb-cell[data-idx='3']").hover()
    page.wait_for_timeout(300)
    rb, tb3 = page.locator(".nb-cell[data-idx='3']").bounding_box(), tools.bounding_box()
    dot = page.locator(".nb-cell[data-idx='3'] .nb-badge i").bounding_box()
    check(
        tools.evaluate("e => getComputedStyle(e).opacity") == "1"
        and tb3["height"] < 40
        and tb3["width"] > 150
        and tb3["x"] + tb3["width"] <= rb["x"] + rb["width"]
        and abs((tb3["y"] + tb3["height"] / 2) - (dot["y"] + dot["height"] / 2)) < 2,
        "hovering shows them as one row at the top right, in line with the state dot",
    )
    cell6.locator(".nb-name").click()
    head_input = cell6.locator("input.nb-head")
    check(
        head_input.is_visible() and head_input.evaluate("e => e === document.activeElement"),
        "clicking a cell's name opens its -- %% line",
    )
    page.keyboard.press("Escape")
    page.wait_for_function(
        "!document.querySelector(\".nb-cell[data-idx='6'] input.nb-head\").offsetParent", timeout=5000
    )
    check(head_input.input_value() == "filtered", "Escape closes it unchanged")
    trend = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    trend.scroll_into_view_if_needed()
    tb = trend.bounding_box()
    page.mouse.move(tb["x"] + tb["width"] * 0.5, tb["y"] + tb["height"] * 0.5)
    page.mouse.move(tb["x"] + tb["width"] * 0.5 + 3, tb["y"] + tb["height"] * 0.5 + 3)
    page.wait_for_selector(".nb-tip", state="visible", timeout=5000)
    check(
        "revenue" in page.locator(".nb-tip").text_content()
        and page.locator(".nb-cell[data-idx='7'] .nb-hover-dot[visibility='visible']").count() == 1,
        "hovering a line chart shows the nearest point's values, through the brush overlay",
    )
    monthly_cell = page.locator(".nb-cell[data-idx='7']")
    monthly_cell.locator(".nb-tab", has_text="Category mix").click()
    bar = monthly_cell.locator("svg.nb-chart-svg [data-tip]").first
    bb, plot = bar.bounding_box(), monthly_cell.locator("svg.nb-chart-svg").bounding_box()
    # Past the end of the bar, in its row: the bar still answers.
    page.mouse.move(plot["x"] + plot["width"] - 30, bb["y"] + bb["height"] / 2)
    page.mouse.move(plot["x"] + plot["width"] - 28, bb["y"] + bb["height"] / 2 + 1)
    page.wait_for_function(
        "(document.querySelector('.nb-tip') || {}).style && document.querySelector('.nb-tip').style.display === 'block' && document.querySelector('.nb-tip').textContent.includes('category')",
        timeout=5000,
    )
    check(
        monthly_cell.locator(".nb-hover-band[visibility='visible']").count() == 1,
        "a bar answers hover anywhere in its row, highlighted",
    )
    monthly_cell.locator(".nb-tab").first.click()
    heat = page.locator(".nb-cell[data-idx='9'] [data-tip]").nth(5)
    heat.scroll_into_view_if_needed()
    heat.hover(force=True)
    page.wait_for_function(
        "(document.querySelector('.nb-tip') || {}).textContent && document.querySelector('.nb-tip').textContent.includes('amount')",
        timeout=5000,
    )
    check(True, "hovering a mark shows its values")
    page.mouse.move(5, 5)
    check(not page.locator(".nb-tip").is_visible(), "the values go away when the pointer leaves")
    add_bar = page.locator(".nb-add").nth(3)
    check(add_bar.evaluate("e => getComputedStyle(e).opacity") == "0", "the add-cell buttons between cells are hidden")
    add_bar.hover()
    page.wait_for_timeout(300)
    check(
        add_bar.evaluate("e => getComputedStyle(e).opacity") == "1", "hovering between cells shows the add-cell buttons"
    )
    regions = page.locator(".nb-cell[data-idx='3']")
    check(
        regions.locator(".nb-switcher").count() == 0 and regions.locator("button.nb-pick-chart").count() == 1,
        "a table's chart picker is hidden behind '+ chart'",
    )
    regions.locator("button.nb-pick-chart").click()
    check(regions.locator(".nb-switcher").count() == 1, "'+ chart' opens the chart picker")
    regions.locator("button.nb-switcher-close").click()
    check(regions.locator(".nb-switcher").count() == 0, "the chart picker closes again")

    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    check(True, "changing an input re-runs the cells that read it")

    before = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr").first.text_content()
    overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    scrolled = page.evaluate("document.getElementById('main').scrollTop")
    page.mouse.move(box["x"] + box["width"] * 0.2, box["y"] + 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.3, box["y"] + 40, steps=5)
    page.mouse.up()
    page.wait_for_function(
        "(b) => { const r = document.querySelector(\".nb-cell[data-idx='10'] table.nb-grid tbody tr\"); "
        "return r && r.textContent !== b; }",
        arg=before,
        timeout=30000,
    )
    months = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr td:nth-child(3)").all_text_contents()
    check(
        len(months) > 0 and all(m[5:7] in ("03", "04") for m in months),
        f"brushing the trend chart filters top_orders ({sorted(set(m[:7] for m in months))})",
    )
    check(
        abs(page.evaluate("document.getElementById('main').scrollTop") - scrolled) < 5 and scrolled > 100,
        f"brushing keeps the page where it was (scrollTop {scrolled})",
    )

    # A query's charts are tabs over its result, with the table last.
    monthly = page.locator(".nb-cell[data-idx='7']")
    tabs = monthly.locator(".nb-tab")
    check(
        tabs.count() == 4
        and "on" in tabs.first.get_attribute("class")
        and "Monthly revenue" in tabs.first.text_content(),
        "a query's charts are tabs, the first one open",
    )
    check(monthly.locator(".nb-chart-title").count() == 0, "a chart in a tab is titled by its tab")
    monthly.locator(".nb-tab", has_text="table").click()
    check(monthly.locator("table.nb-grid").count() == 1, "the table tab shows the query's rows")
    monthly.locator(".nb-tab", has_text="Category mix").click()
    monthly.locator(".nb-stub", has_text="chart spec").click()
    spec = monthly.locator("textarea.nb-spec")
    spec.fill(spec.input_value().replace("Category mix", "Revenue by category"))
    spec.press("Shift+Enter")
    page.wait_for_function(
        "[...document.querySelectorAll(\".nb-cell[data-idx='7'] .nb-tab\")].some(t => t.textContent === 'Revenue by category')",
        timeout=15000,
    )
    check(
        page.locator("#status").text_content().startswith("Ran 1 cell "), "editing a chart spec draws that chart again"
    )

    # Page size: the picker under a table writes rows=N to the cell, so it is saved with the notebook.
    filtered = page.locator(".nb-cell[data-idx='6']")
    fit = page.evaluate("NB.tableRows()")
    check(
        10 < fit < 25 and filtered.locator("table.nb-grid tbody tr").count() == fit,
        f"tables show 25 rows per page by default, or as many as fit without scrolling ({fit})",
    )
    check(
        filtered.locator("table.nb-grid thead tr").count() == 1
        and filtered.locator(".nb-coltype, svg.nb-spark").count() == 0,
        "table headers show column names only, no types or profile",
    )
    filtered.locator("select.nb-page-size").select_option("10")
    page.wait_for_function(
        "document.querySelectorAll(\".nb-cell[data-idx='6'] table.nb-grid tbody tr\").length === 10", timeout=15000
    )
    check(
        "rows=10" in filtered.locator("input.nb-head").input_value(), "choosing 10 per page writes rows=10 to the cell"
    )
    filtered.locator("button.nb-page", has_text="›").click()
    page.wait_for_function(
        "(document.querySelector(\".nb-cell[data-idx='6'] .nb-page-range\") || {}).textContent === '11–20'",
        timeout=15000,
    )
    check(True, "paging moves through the result 10 rows at a time")

    # Hide code / output: toggles in the cell header write hide= to the cell.
    regions = page.locator(".nb-cell[data-idx='3']")
    regions.hover()
    regions.locator("button.nb-hide-code").click()
    check("hide=code" in regions.locator("input.nb-head").input_value(), "the code toggle writes hide=code")
    page.wait_for_timeout(700)
    check(
        regions.locator(".nb-tools").evaluate("e => getComputedStyle(e).opacity") == "1",
        "after a click the buttons stay, though the cell redrew and got shorter",
    )
    check(
        not regions.locator("textarea.nb-code").is_visible() and regions.locator(".nb-stub").first.is_visible(),
        "hidden code collapses to a line that can show it",
    )
    regions.hover()
    regions.locator("button.nb-hide-output").click()
    check("hide=all" in regions.locator("input.nb-head").input_value(), "hiding code and output writes hide=all")
    regions.hover()
    regions.locator("button.nb-hide-output").click()
    regions.hover()
    regions.locator("button.nb-hide-code").click()
    check("hide=" not in regions.locator("input.nb-head").input_value(), "showing both again removes hide=")

    page.locator(".nb-add").last.locator("button", has_text="+ SQL").click()
    editor = page.locator(".nb-cell").last.locator("textarea")
    editor.fill("SELECT 41 + 1 AS answer;")
    check(
        page.locator(".nb-cell").last.locator(".nb-code-hl .tk-num").first.text_content() == "41",
        "highlighting follows typing",
    )
    editor.press("Shift+Enter")
    page.wait_for_function(
        "[...document.querySelectorAll('.nb-cell')].pop().querySelector('table.nb-grid')", timeout=30000
    )
    check(
        "42" in page.locator(".nb-cell").last.locator("table.nb-grid").text_content(),
        "a new cell runs with Shift+Enter",
    )
    answer = page.locator(".nb-cell").last
    answer.locator("button.nb-pick-chart").click()
    answer.locator("button.nb-add-chart").click()
    page.wait_for_function(
        "[...document.querySelectorAll('.nb-cell')].pop().querySelectorAll('.nb-tab').length === 3", timeout=15000
    )
    page.wait_for_function(
        "[...document.querySelectorAll('.nb-cell')].pop().querySelector('.nb-tab-body').children.length > 0",
        timeout=15000,
    )
    check(
        answer.locator(".nb-tab-body .nb-error").count() == 0, "the chart picker adds a chart to the cell and draws it"
    )
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    saved = open(notebook).read()
    check("SELECT 41 + 1 AS answer;" in saved, "Ctrl+S writes the cell to the file")
    check("-- %% filtered rows=10" in saved, "the chosen page size is saved in the file")
    check(
        "SELECT 41 + 1 AS answer;\n\n/* chart\ntype: kpi" in saved,
        "an added chart is saved as a /* chart */ comment in its cell",
    )
    check(
        "title: Revenue by category" in saved and "title: Category mix" not in saved,
        "an edited chart spec is saved in its cell",
    )

    with open(notebook, "a") as f:
        f.write("\n-- %% from_disk\nSELECT 'edited elsewhere' AS note;\n")
    page.wait_for_function("document.querySelectorAll('.nb-cell').length === 13", timeout=15000)
    check(True, "an edit made to the file by another program appears in the editor")

    # Timing is a notebook setting: the gear's switch writes timing: false into the [notebook] cell.
    page.click("#run-all")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
    check(page.locator(".nb-time").count() > 5, "run times show after Run all")
    page.click("#gear")
    page.uncheck("#show-timing")
    check(page.locator(".nb-time").count() == 0, "switching timing off hides run times")
    page.keyboard.press("Escape")
    page.keyboard.press("Control+s")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Saved')", timeout=10000)
    check("timing: false\n*/" in open(notebook).read(), "the timing switch is saved in the [notebook] cell")
    page.click("#gear")
    page.check("#show-timing")
    check(page.locator(".nb-time").count() > 0, "switching timing on shows run times again")
    page.keyboard.press("Escape")

    page.click("#gear")
    page.click("#view-graph")
    check(page.locator(".nb-graph path.edge").count() >= 8, "the graph view draws the dependencies")
    left = page.evaluate("Math.min(...[...document.querySelectorAll('.nb-graph path.edge')].map(p => p.getBBox().x))")
    check(left >= 0, f"no dependency curve bends out of the graph's view box (leftmost x {left:.1f})")
    page.click("#view-reading")
    check(page.locator("textarea.nb-code:visible").count() == 0, "reading view hides the code")
    page.click("#view-reading")
    page.keyboard.press("Escape")

    token = re.search(r"token=([0-9a-f]+)", url).group(1)
    base = url.split("/?")[0]
    response = page.request.get(f"{base}/api/export?nb={notebook}&format=html&token={token}")
    check(response.ok and b"nb-data" in response.body(), "the editor's Export produces an HTML file")
    response = page.request.get(f"{base}/api/export?nb={notebook}&format=md&hide=code&token={token}")
    check(response.ok and b"```sql" not in response.body(), "the editor's Export can leave the code out")
    other = browser.new_page()
    check(
        other.request.get(f"{base}/api/catalog?nb={notebook}").status == 403,
        "the API refuses requests without the token",
    )
    check(not errors, f"no browser errors in the editor {errors[:3]}")

    output = stop(shell)
    check("true" in output, "notebook_stop stops the server")
    check(not os.path.exists(db + ".wal"), "the database closes and checkpoints when the shell exits")
    check(sql(db, "SELECT count(*) FROM orders").strip() == "50000", "another process can open the database afterwards")


def test_metadata_error(browser, work):
    # A notebook whose [notebook] cell needs an extension this DuckDB lacks says so in full.
    notebook = os.path.join(work, "needs_ext.sql")
    with open(notebook, "w") as f:
        f.write(
            "-- %% [notebook]\n/*\ntitle: Needs an extension\nextensions: [no_such_extension_xyz]\n*/\n\n-- %% one\nSELECT 1 AS x;\n\n"
            "-- %% input n slider min=1 max=9\nSET VARIABLE n = 1;\n"
        )
    shell, url = serve(work, notebook)
    page = browser.new_page()
    page.goto(still(url))
    page.wait_for_selector(".nb-cell")
    # B3 follow-up: setting an input never fails because of the [notebook] cell; a run reports that.
    moved = page.evaluate(
        "() => App.api('POST', '/api/variable', {name: 'n', value: 3, cell: 2}).then(r => r.ok, e => 'error: ' + e.message)"
    )
    check(moved is True, f"an input can be set while the [notebook] cell's extension cannot load ({moved})")
    page.click("#run-all")
    page.wait_for_selector(".nb-problem", timeout=30000)
    text = page.locator(".nb-problem .nb-error").text_content()
    check(
        "no_such_extension_xyz" in text
        and "LOAD no_such_extension_xyz works" in text
        and page.locator(".nb-problem").is_visible(),
        "a run that fails before any cell shows the whole message, naming the missing extension",
    )
    stop(shell)


def test_metadata_first(browser, work):
    # B3 and trust on open (DESIGN section 3): the [notebook] cell's attach: applies to every request once the
    # reader has run something, not before. Opening the notebook runs none of it: an input's choices from= a
    # query and the catalog's view of the attached database wait for the first run.
    folder = os.path.join(work, "attached")
    os.makedirs(folder)
    sql(os.path.join(folder, "extra.duckdb"), "CREATE TABLE picks AS SELECT unnest(['b', 'a']) AS c")
    notebook = os.path.join(folder, "attach_first.sql")
    with open(notebook, "w") as f:
        f.write(
            "-- %% [notebook]\n/*\nattach:\n  - {path: extra.duckdb, as: extra, read_only: true}\n*/\n\n"
            "-- %% input pick select from=extra.picks.c\nSET VARIABLE pick = 'a';\n\n"
            "-- %% input size select options=s,m,l\nSET VARIABLE size = 's';\n"
        )
    shell, url = serve(folder, notebook)
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        before = page.evaluate("() => App.api('GET', '/api/options?cell=1').then(r => r, e => 'error: ' + e.message)")
        check(
            before == {"values": [], "reason": "nothing has been run yet"},
            f"opening a notebook runs no input's from= query ({before})",
        )
        listed = page.evaluate(
            "() => App.api('GET', '/api/options?cell=2').then(r => r.values, e => 'error: ' + e.message)"
        )
        check(listed == ["s", "m", "l"], f"an options= list needs no run ({listed})")
        catalog = page.evaluate(
            "() => App.api('GET', '/api/catalog').then(r => [r.trusted, r.tables.map(t => t.qualified)], e => 'error: ' + e.message)"
        )
        check(
            isinstance(catalog, list) and catalog[0] is False and not any("picks" in t for t in catalog[1]),
            f"opening a notebook does not attach what its [notebook] cell names ({catalog})",
        )
        paged = page.evaluate(
            "() => App.api('GET', '/api/page?cell=1&offset=0&limit=5').then(r => 'ok', e => e.message)"
        )
        check("run the notebook first" in paged, f"paging waits for a run ({paged})")
        page.click("#run-all")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        options = page.evaluate(
            "() => App.api('GET', '/api/options?cell=1').then(r => r.values, e => 'error: ' + e.message)"
        )
        check(options == ["a", "b"], f"once run, an input's choices come from the attached database ({options})")
        catalog = page.evaluate(
            "() => App.api('GET', '/api/catalog').then(r => r.tables.map(t => t.qualified), e => 'error: ' + e.message)"
        )
        check(
            isinstance(catalog, list) and any("picks" in t for t in catalog),
            f"once run, the catalog lists the attached database's tables ({catalog})",
        )
        bad = page.evaluate(
            """() => Promise.all([
          App.api('POST', '/api/run', {cells: [-1]}).then(r => 'ok', e => e.message),
          App.api('POST', '/api/run', {cells: [0], page_rows: 'many'}).then(r => 'ok', e => e.message),
          App.api('GET', '/api/page?cell=1&offset=-5&limit=99999999999999999999').then(r => 'ok', e => e.message)])"""
        )
        check(
            "cell indexes" in bad[0] and "page_rows must be a number" in bad[1] and "whole number" in bad[2],
            f"numbers in requests are checked, not cast ({bad})",
        )
        # B12: what the catalog offers to drag can be dragged, and its tooltips name the gestures that work.
        page.wait_for_function("document.getElementById('sidebar').textContent.includes('picks')", timeout=10000)
        page.wait_for_selector("#sidebar details .nb-col", state="attached")
        dragged = page.evaluate(
            """() => [...document.querySelectorAll('#sidebar details')].map(d => {
          const col = d.querySelector('.nb-col'), t = new DataTransfer();
          col.dispatchEvent(new DragEvent('dragstart', {dataTransfer: t, bubbles: true}));
          return [d.querySelector('summary').title, col.title, t.getData('text/plain')];
        })"""
        )
        check(
            any(row[2] == "c" for row in dragged)
            and all("ouble-click" in row[0] and "rag" in row[1] for row in dragged),
            f"a catalog column drags its name, and the tooltips say double-click and drag ({dragged})",
        )
        page.close()
    finally:
        stop(shell)


def test_session_store(browser, work):
    # Round 2 of the review: what the server's session store keeps between requests is the table the cells made,
    # keys, defaults and later ALTERs included; and a db: notebook opened after USE is read, saved and cached in
    # that database.
    folder = os.path.join(work, "session_store")
    os.makedirs(folder)
    notebook = os.path.join(folder, "store.sql")
    with open(notebook, "w") as f:
        f.write(
            "-- %% make\nCREATE TEMP TABLE seen(id INTEGER PRIMARY KEY, v VARCHAR DEFAULT 'x');\n\n"
            "-- %% grow\nALTER TABLE seen ADD COLUMN z INTEGER;\n\n"
            "-- %% add\nINSERT INTO seen (id) VALUES (1) ON CONFLICT DO NOTHING;\n\n"
            "-- %% look\nSELECT count(*) AS n, min(v) AS v, count(z) AS z FROM seen;\n"
        )
    shell, url = serve(folder, notebook)
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        # One request per cell: each is a new connection, which gets the tables back from the store.
        results = page.evaluate(
            """async () => {
          const out = [];
          for (const cell of [0, 1, 2, 2, 3]) {
            const r = await App.api('POST', '/api/run', {text: App.text(), cells: [cell]});
            out.push(r.results.map(x => x.status + (x.error ? ': ' + x.error : '')).join());
          }
          const last = await App.api('POST', '/api/run', {text: App.text(), cells: [3]});
          out.push(JSON.stringify(last.results[0].data.rows));
          return out;
        }"""
        )
        check(
            results[:5] == ["ok"] * 5 and results[5] == '[[1,"x",0]]',
            f"temp tables keep their keys, defaults and ALTERs across requests ({results})",
        )
        page.close()
    finally:
        stop(shell)

    team = os.path.join(folder, "team.duckdb")
    setup = (
        f"ATTACH '{team}' AS team; USE team; "
        f"SELECT count(*) FROM notebook_save('{notebook}', name := 'store');"
    )
    # The first line the shell prints is the count from notebook_save; the URL follows it.
    shell, _ = serve(folder, "db:store", setup=setup)
    url = shell.stdout.readline().strip()
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        saved = page.evaluate(
            """async () => {
          await App.api('POST', '/api/run', {text: App.text(), cells: null});
          const r = await App.api('POST', '/api/notebook', {text: App.text() + '\\n-- saved again\\n', save: true});
          return r.note;
        }"""
        )
        check(saved == "Saved as version 2", f"a db: notebook opened after USE is saved in that database ({saved})")
        page.close()
    finally:
        stop(shell)
    stored = sql(
        team,
        "SELECT count(*) FROM _sketchbook.versions WHERE name = 'store'",
        "SELECT count(*) > 0 FROM _sketchbook.cache WHERE name = 'store'",
    ).split()
    check(stored == ["2", "true"], f"its versions and its cache are in the database USE chose ({stored})")


def test_session_keys(browser, work):
    # Round 3 of the review: a server session is a stored notebook in its database (db:x after USE a and after USE b
    # are two notebooks, and each page keeps to its own) or a file by its canonical path; saving keeps a symlink and
    # refuses an old version; cache entries are readable by their owner only.
    folder = os.path.join(work, "session_keys")
    os.makedirs(folder)
    with open(os.path.join(folder, "real.sql"), "w") as f:
        f.write("-- %% one\nSELECT 1 AS one;\n")
    link = os.path.join(folder, "link.sql")
    os.symlink("real.sql", link)
    for name, text in (("x_a.sql", "in a"), ("x_b1.sql", "in b, first"), ("x_b2.sql", "in b, second")):
        with open(os.path.join(folder, name), "w") as f:
            f.write(f"-- %% which\nSELECT '{text}' AS w;\n")
    setup = (
        "ATTACH 'a.duckdb' AS a; ATTACH 'b.duckdb' AS b; "
        "USE b; SELECT count(*) FROM notebook_save('x_b1.sql', name := 'x'); "
        "SELECT count(*) FROM notebook_save('x_b2.sql', name := 'x'); "
        "USE a; SELECT count(*) FROM notebook_save('x_a.sql', name := 'x');"
    )
    shell, line = serve(folder, "db:x", setup=setup)
    # The counts notebook_save printed come first.
    while not line.startswith("http"):
        line = shell.stdout.readline().strip()
    url_a = line
    shell.stdin.write("USE b; SELECT url FROM notebook_start('db:x', open := false);\n")
    shell.stdin.flush()
    url_b = shell.stdout.readline().strip()
    base = url_a.split("&nb=")[0]
    try:
        check("db=a" in url_a and "db=b" in url_b, f"notebook_start names its current database in the URL ({url_b})")
        texts = []
        for url in (url_a, url_b, url_a):
            page = browser.new_page()
            page.goto(still(url))
            page.wait_for_selector(".nb-cell")
            texts.append(page.evaluate("App.text()"))
            page.close()
        check(
            "in a" in texts[0] and "in b, second" in texts[1] and "in a" in texts[2],
            "db:x after USE a and after USE b are two notebooks, and a page keeps to its database",
        )

        # A file reached through a symlink is saved through it: the link stays a link.
        page = browser.new_page()
        page.goto(still(base + "&nb=link.sql"))
        page.wait_for_selector(".nb-cell")
        note = page.evaluate(
            """async () => {
          await App.api('POST', '/api/run', {text: App.text(), cells: null});
          return (await App.api('POST', '/api/notebook', {text: App.text() + '\\n-- saved through the link\\n',
                                                         save: true})).note;
        }"""
        )
        with open(os.path.join(folder, "real.sql")) as f:
            saved = f.read()
        check(
            os.path.islink(link) and "saved through the link" in saved,
            f"saving a symlinked notebook writes the file it points to and keeps the link ({note})",
        )
        modes = []
        for root, _, names in os.walk(os.path.join(folder, ".duckdb-sketchbook")):
            modes += [os.stat(os.path.join(root, n)).st_mode & 0o777 for n in names if n.endswith(".json")]
        check(modes and all(m & 0o077 == 0 for m in modes), f"cache entries are readable by their owner only {modes}")
        page.close()

        # real.sql, ./real.sql and link.sql are one notebook, so one session.
        for nb in ("real.sql", "./real.sql"):
            page = browser.new_page()
            page.goto(still(base + "&nb=" + nb))
            page.wait_for_selector(".nb-cell")
            page.close()
        page = browser.new_page()
        page.goto(still(url_b.replace("nb=db%3Ax", "nb=db%3Ax%401")))
        page.wait_for_selector(".nb-cell")
        result = page.evaluate(
            """async () => {
          const open = (await App.api('GET', '/api/notebooks')).open;
          let refused = '';
          try {
            await App.api('POST', '/api/notebook', {text: App.text(), save: true});
          } catch (e) {
            refused = e.message;
          }
          return {open, refused, text: App.text()};
        }"""
        )
        files = [s for s in result["open"] if s.endswith("real.sql") or s.endswith("link.sql")]
        check(len(files) == 1, f"a file opened by three paths is one session ({result['open']})")
        check(
            "in b, first" in result["text"] and "not the newest" in result["refused"],
            f"an old version opens but is not saved over the newest ({result['refused']})",
        )
        page.close()
    finally:
        stop(shell)
    versions = sql(os.path.join(folder, "b.duckdb"), "SELECT count(*) FROM _sketchbook.versions WHERE name = 'x'")
    check(versions.split() == ["2"], f"nothing was saved from the old version ({versions.split()})")

    # The store makes a table whose foreign key names another after that one, whatever their names.
    with open(os.path.join(folder, "fk.sql"), "w") as f:
        f.write(
            "-- %% parent\nCREATE TEMP TABLE parent(id INTEGER PRIMARY KEY);\nINSERT INTO parent VALUES (1);\n\n"
            "-- %% child\nCREATE TEMP TABLE child(x INTEGER REFERENCES parent(id));\n\n"
            "-- %% bad\nINSERT INTO child VALUES (2);\n"
        )
    shell, url = serve(folder, "fk.sql")
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        result = page.evaluate(
            """async () => {
          await App.api('POST', '/api/run', {text: App.text(), cells: [0, 1]});
          const r = await App.api('POST', '/api/run', {text: App.text(), cells: [2]});
          return r.results[0].status + ' ' + (r.results[0].error || '');
        }"""
        )
        check(result.startswith("error") and "foreign key" in result.lower(), f"a foreign key survives the store ({result})")
        page.close()
    finally:
        stop(shell)

    # When a table a cell made cannot be copied into the store (SKETCHBOOK_TEST_FAIL_STORE_COPY makes the copy fail),
    # that cell's tables are made again by its SQL and none comes from the store, and no later cell copies them in:
    # either would clash with that replay.
    with open(os.path.join(folder, "copy.sql"), "w") as f:
        f.write(
            "-- %% mk\nCREATE TEMP TABLE b AS SELECT 2 AS y;\nCREATE TEMP TABLE a AS SELECT 1 AS x;\n\n"
            "-- %% ins\nINSERT INTO b VALUES (3);\n\n"
            "-- %% look\nSELECT (SELECT count(*) FROM a) AS na, (SELECT count(*) FROM b) AS nb;\n"
        )
    os.environ["SKETCHBOOK_TEST_FAIL_STORE_COPY"] = "a"
    try:
        shell, url = serve(folder, "copy.sql")
    finally:
        del os.environ["SKETCHBOOK_TEST_FAIL_STORE_COPY"]
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        result = page.evaluate(
            """async () => {
          const out = [];
          for (const cells of [[0], [1], [2]]) {
            const r = await App.api('POST', '/api/run', {text: App.text(), cells});
            out.push(r.results[0].status + (r.results[0].error ? ' ' + r.results[0].error : ''));
          }
          const look = await App.api('POST', '/api/run', {text: App.text(), cells: [2]});
          out.push(JSON.stringify(look.results[0].data.rows));
          return out;
        }"""
        )
        check(
            result[:3] == ["ok", "ok", "ok"] and result[3] == "[[1,1]]",
            f"a failed copy into the store replays the cell's tables, in later requests too ({result})",
        )
        page.close()
    finally:
        stop(shell)

    # Database names are matched without case: USE B and USE b open one session.
    shell, url_upper = serve(folder, "db:x", setup="ATTACH 'b.duckdb' AS b; USE B;")
    try:
        shell.stdin.write("USE b; SELECT url FROM notebook_start('db:x', open := false);\n")
        shell.stdin.flush()
        url_lower = shell.stdout.readline().strip()
        page = browser.new_page()
        for url in (url_upper, url_lower):
            page.goto(still(url))
            page.wait_for_selector(".nb-cell")
        opened = page.evaluate("async () => (await App.api('GET', '/api/notebooks')).open")
        check(opened.count("db:x") == 1, f"a stored notebook is one session whatever the case of its database ({opened})")
        page.close()
    finally:
        stop(shell)


def test_cache_written_later(browser, work):
    # B24: a session reads the store again until it has run, so results a scheduled
    # notebook_run(cache := true) writes after the editor opened appear on the next reload.
    folder = os.path.join(work, "cache_later")
    os.makedirs(folder)
    notebook = os.path.join(folder, "later.sql")
    with open(notebook, "w") as f:
        f.write("-- %% answer\nSELECT 42 AS answer;\n")
    shell, url = serve(folder, notebook, os.path.join(folder, "later.duckdb"))
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        check(page.locator("table.nb-grid").count() == 0, "a notebook never run shows no results")
        shell.stdin.write(f"SELECT count(*) FROM notebook_run('{notebook}', cache := true);\n")
        shell.stdin.flush()
        shell.stdout.readline()
        page.reload()
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.getElementById('status').textContent !== ''")
        grid = page.locator("table.nb-grid")
        check(
            grid.count() == 1 and "42" in grid.text_content(),
            "results kept by notebook_run after the editor opened show on reload",
        )
        page.close()
    finally:
        stop(shell)


def test_cache_variables_reloaded(browser, work):
    # B24 follow-up: reading the store again refreshes the input values it restored, and keeps the ones the
    # reader set in this session.
    folder = os.path.join(work, "cache_vars")
    os.makedirs(folder)
    notebook = os.path.join(folder, "vars.sql")
    with open(notebook, "w") as f:
        f.write("-- %% input n slider min=1 max=9\nSET VARIABLE n = 1;\n\n-- %% q\nSELECT $n AS v\n")
    db = os.path.join(folder, "vars.duckdb")
    sql(db, f"SELECT count(*) FROM notebook_run('{notebook}', cache := true, params := {{n: 2}})")
    shell, url = serve(folder, notebook, db)

    def scheduled(n):
        shell.stdin.write(f"SELECT count(*) FROM notebook_run('{notebook}', cache := true, params := {{n: {n}}});\n")
        shell.stdin.flush()
        shell.stdout.readline()

    def reopened():
        page.reload()
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.getElementById('status').textContent !== ''")
        return page.evaluate(
            """async () => {
          const opened = await App.api('GET', '/api/notebook');
          const r = await App.api('POST', '/api/run', {text: App.text(), cells: [1]});
          return [opened.variables.n, JSON.stringify(r)];
        }"""
        )

    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        scheduled(3)
        shown, ran = reopened()
        check(
            shown == 3 and '"rows":[[3]]' in ran,
            f"a restored input follows the store when it is read again ({shown}, {ran[:300]})",
        )
        page.close()
    finally:
        stop(shell)
    # The reader moves the input without running; a scheduled run then writes another value.
    shell, url = serve(folder, notebook, db)
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.evaluate("() => App.api('POST', '/api/variable', {name: 'n', value: 7, cell: 0})")
        scheduled(4)
        shown, ran = reopened()
        check(
            shown == 7 and '"rows":[[7]]' in ran,
            f"an input the reader set outlives the store's copy ({shown}, {ran[:300]})",
        )
        page.close()
    finally:
        stop(shell)


def test_cache_redraw_stale(browser, work):
    # B26: drawing a stale result's charts again does not make its rows fresh.
    folder = os.path.join(work, "cache_redraw")
    os.makedirs(folder)
    notebook = os.path.join(folder, "redraw.sql")
    with open(notebook, "w") as f:
        f.write(
            "-- %% input n slider min=1 max=5\nSET VARIABLE n = 1;\n\n"
            "-- %% q\nSELECT $n AS v\n\n/* chart\nmark: bar\nx: v\ny: v\n*/\n"
        )
    db = os.path.join(folder, "redraw.duckdb")
    sql(db, f"SELECT count(*) FROM notebook_run('{notebook}', cache := true)")
    shell, url = serve(folder, notebook, db)
    try:
        page = browser.new_page()
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.getElementById('status').textContent !== ''")
        # Everything runs (v = 1); the input changes without running, so q's kept result is stale; then only
        # q's charts are drawn again, from its view, which now gives v = 2.
        page.evaluate("() => App.api('POST', '/api/run', {text: App.text(), cells: null, redraw: []})")
        page.evaluate("() => App.api('POST', '/api/variable', {name: 'n', value: 2, cell: 0})")
        page.evaluate("() => App.api('POST', '/api/run', {text: App.text(), cells: [], redraw: [1]})")
        page.reload()
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.getElementById('status').textContent !== ''")
        state = page.evaluate(
            "() => { const c = document.querySelector(\".nb-cell[data-idx='1']\"); return {outdated: c.classList.contains('outdated'), kept: !!c.querySelector('.nb-output svg, table.nb-grid')} }"
        )
        check(
            state["kept"] and state["outdated"],
            f"a stale result whose charts were drawn again is still stale ({state})",
        )
        page.close()
    finally:
        stop(shell)


def test_brush_listeners(browser, work):
    # B4: drawing a brushable chart again does not add window listeners each time.
    notebook = os.path.join(work, "brush.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    shell, url = serve(work, notebook)
    try:
        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        page.add_init_script(
            """(() => { window.__moves = 0; const add = window.addEventListener;
            window.addEventListener = function (type) { if (type === 'mousemove') window.__moves++; return add.apply(this, arguments); }; })()"""
        )
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.click("#run-all")
        page.wait_for_function(
            "!App.running && document.getElementById('status').textContent.startsWith('Ran')", timeout=60000
        )
        check(page.locator(".nb-brush-overlay").count() > 0, "the notebook has a brushable chart")
        before = page.evaluate("window.__moves")
        page.evaluate("() => { App.render(); App.render(); App.render(); }")
        after = page.evaluate("window.__moves")
        check(after == before, f"drawing brushable charts again adds no window listeners ({before} -> {after})")
        page.close()
    finally:
        stop(shell)


def test_cache(browser, work):
    """The result cache (FORMAT §8): a reopened notebook shows its last results without running anything,
    results whose inputs changed are marked, and the cache can be cleared or switched off."""
    folder = os.path.join(work, "cache")
    os.makedirs(folder)
    notebook = os.path.join(folder, "revenue.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    db = os.path.join(folder, "cache.duckdb")
    sidecar = os.path.join(folder, ".duckdb-sketchbook", "revenue")

    def start(source=notebook):
        return serve(folder, source, db)

    def open_page(url):
        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        page.errors = watch(page)
        # What the page asks the server to do: a reopened notebook must not run or query anything.
        page.calls = []
        page.on(
            "request", lambda r: page.calls.append(r.url.split("/api/")[1].split("?")[0]) if "/api/" in r.url else None
        )
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.getElementById('status').textContent !== ''")
        return page

    def run_all(page):
        page.click("#run-all")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)

    def outdated(page):
        return page.evaluate(
            "() => Array.from(document.querySelectorAll('.nb-cell.outdated')).map(c => +c.dataset.idx)"
        )

    def shown(page):
        """What a reader sees of the results: the prose, a grid's page, the charts and the input."""
        return page.evaluate(
            """() => ({
            prose: document.querySelector(".nb-cell[data-idx='1'] .nb-md").textContent,
            grid: document.querySelector(".nb-cell[data-idx='6'] .nb-output").innerHTML,
            charts: document.querySelector(".nb-cell[data-idx='7'] .nb-output").innerHTML,
            kpi: document.querySelector(".nb-cell[data-idx='8'] .nb-output").innerHTML,
            top: document.querySelector(".nb-cell[data-idx='10'] .nb-output").innerHTML,
            foot: document.querySelector(".nb-cell[data-idx='6'] .nb-foot").textContent,
            region: (s => s.options[s.selectedIndex].text)(document.querySelector(".nb-input select")),
            badges: Array.from(document.querySelectorAll('.nb-cell-head .nb-badge')).map(b => b.className)
        })"""
        )

    # ---- First session: run, pick an input, brush. ----
    shell, url = start()
    page = open_page(url)
    check(
        page.locator(".nb-badge.fresh").count() == 0 and "Opened" in page.locator("#status").text_content(),
        "a notebook that has never run opens with no results",
    )
    check(not os.path.exists(sidecar), "opening a notebook writes no cache")
    run_all(page)
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')", timeout=30000)
    before_top = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr").first.text_content()
    overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    page.mouse.move(box["x"] + box["width"] * 0.2, box["y"] + 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.3, box["y"] + 40, steps=5)
    page.mouse.up()
    page.wait_for_function(
        "(b) => { const r = document.querySelector(\".nb-cell[data-idx='10'] table.nb-grid tbody tr\"); "
        "return r && r.textContent !== b && !App.running; }",
        arg=before_top,
        timeout=30000,
    )
    page.mouse.move(5, 5)
    page.wait_for_timeout(300)
    first = shown(page)
    check(
        "US" in first["prose"] and first["region"] == "US" and all("fresh" in b for b in first["badges"][1:]),
        "the first session ends with every cell fresh, for the region picked",
    )
    names = sorted(os.listdir(sidecar)) if os.path.isdir(sidecar) else []
    check(
        "monthly.json" in names
        and "@state.json" in names
        and "filtered.json" in names
        and len([n for n in names if n.startswith("@") and n != "@state.json"]) == 2,
        f"running keeps each cell's result beside the notebook ({len(names)} files in .duckdb-sketchbook/revenue)",
    )
    check(
        open(os.path.join(folder, ".duckdb-sketchbook", ".gitignore")).read().strip().endswith("*"),
        "the cache directory ignores itself in git",
    )
    check(
        max(os.path.getsize(os.path.join(sidecar, n)) for n in names) < 4 * 1024 * 1024,
        "every entry is under the size limit",
    )
    check(not page.errors, f"no browser errors while running {page.errors[:3]}")
    page.close()
    stop(shell)

    # ---- A cache kept before the extension was renamed (.duckdb-notebook/) is still shown. ----
    current, legacy = os.path.join(folder, ".duckdb-sketchbook"), os.path.join(folder, ".duckdb-notebook")
    os.rename(current, legacy)
    shell, url = start()
    page = open_page(url)
    old = shown(page)
    check(
        old["badges"] == first["badges"] and old["grid"] == first["grid"] and old["region"] == "US",
        "a cache in the old .duckdb-notebook folder is read when the new folder has none",
    )
    check(not os.path.exists(current), "reading an old cache writes nothing in the new folder")
    page.close()
    stop(shell)
    os.rename(legacy, current)

    # ---- A new process: the page shows the same results at once, and nothing runs. ----
    shell, url = start()
    page = open_page(url)
    again = shown(page)
    check(
        again["badges"] == first["badges"] and again["region"] == "US" and again["prose"] == first["prose"],
        "reopening shows the last results: the same states, input value and prose",
    )
    check(
        again["grid"] == first["grid"] and again["top"] == first["top"] and again["foot"] == first["foot"],
        "a remembered grid is the same page of rows, drawn exactly as when it was computed",
    )
    check(
        again["charts"] == first["charts"]
        and again["kpi"] == first["kpi"]
        and page.locator(".nb-cell[data-idx='7'] rect.nb-brush").get_attribute("width") not in (None, "0"),
        "remembered charts are drawn exactly as before, the brush included",
    )
    check(
        set(page.calls) <= {"notebook", "catalog", "notebooks", "events"},
        f"reopening runs nothing: the page only asks for the notebook ({sorted(set(page.calls))})",
    )
    check(
        outdated(page) == []
        and "last results" in page.locator("#status").text_content()
        and "Run all" in page.locator("#status").get_attribute("title"),
        "nothing is marked stale, and the status says these are the last results",
    )
    stopped = sql(
        os.path.join(folder, "other.duckdb"), "SELECT count(*) FROM duckdb_views() WHERE view_name = 'monthly'"
    ).strip()
    check(stopped == "0", "showing results defines nothing")

    # Paging and brushing need a session that has run: they say so instead of running or failing.
    page.locator(".nb-cell[data-idx='6'] button.nb-page").last.click()
    check(
        "first page of the last run" in page.locator("#status").text_content(),
        "paging a remembered grid asks for a run",
    )
    overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
    overlay.scroll_into_view_if_needed()
    box = overlay.bounding_box()
    page.mouse.move(box["x"] + box["width"] * 0.6, box["y"] + 20)
    page.mouse.down()
    page.mouse.move(box["x"] + box["width"] * 0.7, box["y"] + 40, steps=5)
    page.mouse.up()
    page.wait_for_timeout(300)
    check(
        "last run" in page.locator("#status").text_content() and shown(page)["charts"] == first["charts"],
        "brushing a remembered chart asks for a run and leaves the chart as it was",
    )
    check(
        "run" not in page.calls and "page" not in page.calls and "variable" not in page.calls,
        "neither ran or queried anything",
    )

    # An input changes: what reads it is stale, in fainter ink with a note, and still nothing runs.
    page.locator(".nb-input select").first.select_option(label="APAC")
    page.wait_for_function("document.querySelectorAll('.nb-cell.outdated').length > 0")
    check(
        outdated(page) == [1, 6, 7, 8, 9, 10],
        f"changing an input marks the cells that read it, and their readers ({outdated(page)})",
    )
    faint = page.evaluate(
        """() => ['6', '3'].map(i => +getComputedStyle(document.querySelector(`.nb-cell[data-idx='${i}'] .nb-output`)).opacity)"""
    )
    note = page.locator(".nb-cell[data-idx='6'] .nb-foot .nb-outdated")
    check(
        faint[0] < 0.6
        and faint[1] == 1
        and note.text_content() == "from an earlier run"
        and "earlier run" in note.get_attribute("title"),
        f"a stale result is fainter ({faint[0]}) and says it is from an earlier run; a fresh one is not",
    )
    check(shown(page)["grid"] == first["grid"], "the stale result itself is still the last one")
    check("run" not in page.calls, "changing an input before anything has run does not run cells")
    page.click("#gear")
    page.click("#view-reading")
    page.keyboard.press("Escape")
    check(
        page.locator(".nb-cell[data-idx='6'] .nb-outdated").is_visible(),
        "reading view keeps the note on a stale result",
    )
    page.click("#gear")
    page.click("#view-reading")
    page.keyboard.press("Escape")
    for theme in ("dark", "light"):
        page.evaluate("t => document.documentElement.setAttribute('data-theme', t)", theme)
        colour = page.evaluate("() => getComputedStyle(document.querySelector('.nb-outdated')).color")
        check(colour == "rgb(240, 140, 0)", f"the note is drawn in the stale colour in {theme} mode")

    # Running brings everything up to date.
    run_all(page)
    check(outdated(page) == [] and page.locator(".nb-badge.stale").count() == 0, "after a run nothing is stale")
    check(not page.errors, f"no browser errors on a reopened notebook {page.errors[:3]}")
    page.close()
    stop(shell)

    # ---- An upstream cell is edited on disk, and two entries are damaged. ----
    text = open(notebook).read()
    with open(notebook, "w") as f:
        f.write(text.replace("count(*) AS orders\nFROM filtered", "count(*) AS order_count\nFROM filtered"))
    with open(os.path.join(sidecar, "top_orders.json"), "w") as f:
        f.write("this is not a result")
    damaged = open(os.path.join(sidecar, "order_sizes.json")).read()
    with open(os.path.join(sidecar, "order_sizes.json"), "w") as f:
        f.write(damaged[: len(damaged) // 2])
    shell, url = start()
    page = open_page(url)
    check(
        outdated(page) == [1, 7, 8],
        f"an edited cell and the cells that read it (the prose reads monthly) are stale; the rest stay fresh ({outdated(page)})",
    )
    check(
        page.locator(".nb-cell[data-idx='6'] .nb-badge.fresh").count() == 1
        and page.locator(".nb-cell[data-idx='6'] table.nb-grid").count() == 1,
        "a cell above the edit keeps its fresh result",
    )
    check(
        page.locator(".nb-cell[data-idx='9'] svg.nb-chart-svg").count() == 0
        and page.locator(".nb-cell[data-idx='10'] table.nb-grid").count() == 0
        and page.locator(".nb-error").count() == 0
        and not page.errors,
        "a corrupt entry and a truncated one are ignored: those cells are simply empty",
    )
    run_all(page)
    check(
        outdated(page) == [] and page.locator(".nb-cell[data-idx='10'] table.nb-grid").count() == 1,
        "running fills them in again",
    )

    # ---- The gear menu: forget the results, or keep none. ----
    page.click("#gear")
    check(
        page.locator("#keep-results").is_checked() and page.locator("#forget-results").is_visible(),
        "the gear menu has the two cache controls, results kept by default",
    )
    page.click("#forget-results")
    page.wait_for_function("document.getElementById('status').textContent.startsWith('Forgot')")
    check(
        not os.path.exists(sidecar) and page.locator(".nb-cell[data-idx='6'] table.nb-grid").count() == 1,
        "Forget removes the notebook's cache and leaves this session's results on the page",
    )
    page.keyboard.press("Escape")
    page.reload()
    page.wait_for_selector(".nb-cell")
    page.wait_for_function("document.getElementById('status').textContent !== ''")
    check(
        page.locator("table.nb-grid").count() == 0 and "Opened" in page.locator("#status").text_content(),
        "after Forget the notebook opens with no results",
    )
    run_all(page)
    check(os.path.isdir(sidecar), "the next run keeps results again")
    page.click("#gear")
    page.click("#keep-results")
    page.wait_for_function("App.text().includes('cache: false')")
    page.wait_for_timeout(1200)
    check(
        not os.path.exists(sidecar),
        "unticking keep results writes cache: false in the [notebook] cell and removes the cache",
    )
    page.keyboard.press("Escape")
    run_all(page)
    check(
        not os.path.exists(sidecar) and page.locator(".nb-cell[data-idx='6'] table.nb-grid").count() == 1,
        "with cache: false a run keeps nothing",
    )
    page.click("#gear")
    page.click("#keep-results")
    page.wait_for_function("!App.text().includes('cache: false')")
    page.keyboard.press("Escape")
    run_all(page)
    check(os.path.isdir(sidecar), "ticking it again keeps results from the next run")
    check(not page.errors, f"no browser errors using the cache controls {page.errors[:3]}")
    page.close()

    # ---- A notebook stored in the database keeps its cache there. ----
    shell.stdin.write(
        f"SELECT saved FROM notebook_save('{os.path.join(ROOT, 'examples', 'revenue.sql')}', name := 'stored');\n"
    )
    shell.stdin.flush()
    shell.stdout.readline()
    page = open_page(url.split("&nb=")[0] + "&nb=db:stored")
    run_all(page)
    page.close()
    stop(shell)
    rows = sql(
        db, "SELECT count(*), count(*) FILTER (cell = 'monthly') FROM _sketchbook.cache WHERE name = 'stored'"
    ).strip()
    check(
        rows.split("|")[0].isdigit() and int(rows.split("|")[0]) >= 10 and rows.split("|")[1] == "1",
        f"a stored notebook's results are rows of _sketchbook.cache ({rows})",
    )
    shell, url = start("db:stored")
    page = open_page(url)
    check(
        page.locator(".nb-cell[data-idx='6'] table.nb-grid").count() == 1
        and outdated(page) == []
        and "run" not in page.calls
        and page.locator(".nb-badge.fresh").count() == 11,
        "a stored notebook reopens with its results too",
    )
    check(not page.errors, f"no browser errors with a stored notebook {page.errors[:3]}")
    page.close()
    stop(shell)


def test_exports(browser, work, chromium):
    notebook = os.path.join(ROOT, "examples", "revenue.sql")
    db = os.path.join(work, "exports.duckdb")
    html = os.path.join(work, "revenue.html")
    pre = os.path.join(work, "revenue_pre.html")
    pdf = os.path.join(work, "revenue.pdf")
    sql(
        db,
        f"FROM notebook_export('{notebook}', file := '{html}')",
        f"FROM notebook_export('{notebook}', file := '{pre}', interactive := 'precomputed')",
        f"FROM notebook_export('{notebook}', file := '{pdf}', format := 'pdf', chrome := '{chromium}')",
    )

    page = browser.new_page()
    errors = watch(page)
    page.route("**/*", lambda r: r.abort() if not r.request.url.startswith("file:") else r.continue_())
    page.goto(still("file://" + html))
    page.wait_for_function("window.__NB_READY === true")
    check(page.locator("svg.nb-chart-svg").count() >= 2, "HTML export draws its charts offline")
    check(page.locator("pre.nb-source").count() >= 3, "an export shows the SQL by default")
    check(
        page.locator("pre.nb-source .tk-kw").count() > 5 and page.locator("pre.nb-source .tk-var").count() > 0,
        "exported SQL is highlighted",
    )
    monthly = page.locator(".nb-cell[data-idx='7']")
    check(
        monthly.locator(".nb-tab").count() == 3 and "/* chart" not in monthly.locator("pre.nb-source").text_content(),
        "an export shows a query's charts as tabs and its SQL without them",
    )
    monthly.locator(".nb-tab", has_text="table").click()
    check(page.locator(".nb-cell[data-idx='7'] table.nb-grid").count() == 1, "an export's table tab shows the rows")
    check(page.evaluate("document.fonts.check('20px Excalifont')"), "an export carries its handwriting font")
    first = page.locator(".nb-cell[data-kind='sql'] table.nb-grid").nth(1)
    fit = page.evaluate("NB.tableRows()")
    check(
        first.locator("tbody tr").count() == min(25, fit), f"exported tables page as many rows as fit their box ({fit})"
    )

    nocode = os.path.join(work, "revenue_nocode.html")
    sql(db, f"FROM notebook_export('{notebook}', file := '{nocode}', hide := 'code')")
    page.goto(still("file://" + nocode))
    page.wait_for_function("window.__NB_READY === true")
    check(
        page.locator("pre.nb-source").count() == 0 and page.locator("table.nb-grid").count() >= 3,
        "hide := 'code' exports results without code",
    )
    check(page.locator(".nb-input select:disabled").count() == 1, "a static export shows inputs but freezes them")

    page.goto(still("file://" + pre))
    page.wait_for_function("window.__NB_READY === true")
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('US')")
    check(True, "a precomputed export switches results when an input changes")

    gallery = os.path.join(work, "charts.html")
    sql(db, f"FROM notebook_export('{os.path.join(ROOT, 'examples', 'charts.sql')}', file := '{gallery}')")
    page.goto(still("file://" + gallery))
    page.wait_for_function("window.__NB_READY === true")
    check(
        page.locator("svg.nb-chart-svg").count() >= 24 and page.locator(".nb-error").count() == 0,
        "every chart type in examples/charts.sql draws without errors",
    )
    legend = page.locator(".nb-cell[data-idx='6'] .nb-legend-item").evaluate_all(
        "els => els.map(e => e.textContent + '=' + getComputedStyle(e.querySelector('i')).borderColor)"
    )
    check(
        legend
        == ["North=rgb(25, 113, 194)", "South=rgb(47, 158, 68)", "East=rgb(232, 89, 12)", "West=rgb(156, 54, 181)"],
        f"a color scale's domain and range fix each category's order and colour ({legend})",
    )
    kpi_colour = page.locator(".nb-kpi-banded .nb-kpi-value").first.evaluate("e => getComputedStyle(e).color")
    check(kpi_colour == "rgb(47, 158, 68)", f"a KPI's bands colour its value by the band it falls in ({kpi_colour})")
    # tip: on a table chart: the region cells carry the row's category and orders, which are not columns of their own.
    tipped = page.locator(".nb-cell:has(td.nb-has-tip)")
    heads = tipped.locator("thead th .nb-colname").all_text_contents()
    check(heads == ["day", "region", "revenue"], f"a table's tip sources are not shown as columns ({heads})")
    cell = tipped.locator("td.nb-has-tip").first
    cell.scroll_into_view_if_needed()
    cell.hover()
    page.wait_for_selector(".nb-tip", state="visible", timeout=5000)
    tip_text = page.locator(".nb-tip").text_content()
    check(
        "category" in tip_text
        and "orders" in tip_text
        and tipped.locator("td.nb-has-tip").count() == tipped.locator("tbody tr").count(),
        f"hovering a table cell with tip: shows that row's values of the tip columns ({tip_text!r})",
    )
    page.mouse.move(0, 0)
    link = tipped.locator("td a.nb-link").first
    href, region = link.get_attribute("href"), link.text_content()
    colours = link.evaluate(
        "a => [getComputedStyle(a).color, getComputedStyle(a.closest('td')).color, getComputedStyle(a).textDecorationLine]"
    )
    check(
        href == "https://example.com/regions/" + region and link.get_attribute("target") == "_blank",
        f"link: makes a table cell a link to its URL template, filled in from the row ({href})",
    )
    check(
        colours[0] == colours[1] and "underline" in colours[2],
        f"in an export a table link keeps the text's colour and is underlined ({colours})",
    )
    widths = page.locator(".nb-cell[data-idx='15'] svg.nb-chart-svg [data-tip] > .sk-hit").evaluate_all(
        "els => els.map(e => e.getBoundingClientRect().width)"
    )
    check(
        len(widths) == 48 and min(widths) > 0.9 * max(widths),
        f"bars on a date axis fill their slot, the first and last as wide as the rest ({min(widths or [0]):.1f}-{max(widths or [0]):.1f}px)",
    )
    # Nothing scrolls inside a page: tables show fewer rows and page their columns instead.
    scrolling = """() => [...document.querySelectorAll('#nb-root *')].filter(e => { const s = getComputedStyle(e);
      return (/auto|scroll/.test(s.overflowX) && e.scrollWidth > e.clientWidth + 1) || (/auto|scroll/.test(s.overflowY) && e.scrollHeight > e.clientHeight + 1); }).length"""
    clipped = "() => [...document.querySelectorAll('.nb-output, .nb-grid-wrap')].filter(e => e.scrollWidth > e.clientWidth + 1 || e.scrollHeight > e.clientHeight + 1).length"
    for width in (1300, 600):
        page.set_viewport_size({"width": width, "height": 900})
        page.wait_for_timeout(400)
        check(
            page.evaluate(scrolling) == 0 and page.evaluate(clipped) == 0,
            f"at {width}px no table or chart in examples/charts.sql scrolls or is cut off",
        )
    pivot = page.locator(".nb-cell[data-idx='20']")
    pivot.scroll_into_view_if_needed()
    check(
        pivot.locator(".nb-col-range").text_content() == "columns 1–2 of 4"
        and pivot.locator("th:not(.nb-col-off)").count() == 3,
        "a table too wide for its cell shows the columns that fit, and says how many there are",
    )
    check(
        pivot.locator("select.nb-page-size").count() == 0
        and page.locator(".nb-cell[data-idx='3'] select.nb-page-size").count() == 1,
        "a table short enough for any page size has no page-size picker; a longer one does",
    )
    pivot.locator("button.nb-col-page", has_text="›").click()
    page.wait_for_timeout(100)
    check(
        pivot.locator(".nb-col-range").text_content() == "columns 3–4 of 4"
        and pivot.locator("th:not(.nb-col-off)").first.text_content() == ""
        and pivot.locator("th:not(.nb-col-off) .nb-colname").first.text_content() == "Services",
        "› shows the next columns, with the row numbers kept",
    )
    page.set_viewport_size({"width": 1280, "height": 720})
    check(not errors, f"no browser errors in the exports {errors[:3]}")

    check(open(pdf, "rb").read(5) == b"%PDF-", "PDF export prints through headless Chromium")

    runtime = os.environ.get("NOTEBOOK_WASM_RUNTIME")
    if not runtime:
        print("skip html-wasm: set NOTEBOOK_WASM_RUNTIME to a runtime built by wasm/build.sh")
        return
    live = os.path.join(work, "revenue_wasm.html")
    sql(
        db,
        f"FROM notebook_export('{notebook}', file := '{live}', format := 'html-wasm', " f"wasm_runtime := '{runtime}')",
    )
    page = browser.new_page()
    errors = watch(page)
    page.route("**/*", lambda r: r.abort() if not r.request.url.startswith(("file:", "blob:")) else r.continue_())
    page.goto(still("file://" + live))
    page.wait_for_function("window.__NB_WASM === true", timeout=60000)
    page.locator(".nb-input select").first.select_option(label="APAC")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes('APAC')", timeout=30000)
    check(True, "html-wasm: DuckDB-WASM re-runs the notebook's SQL in the page, offline")
    check(not errors, f"no browser errors in the html-wasm export {errors[:3]}")

    # B14: a variable set in the page has the type the editor gives it, and {{ }} shows a value as the
    # editor does (DuckDB's own text for it).
    values = os.path.join(work, "wasm_values.html")
    sql(
        db,
        f"FROM notebook_export('{os.path.join(ROOT, 'test', 'notebooks', 'wasm_values.sql')}', file := '{values}', "
        f"format := 'html-wasm', wasm_runtime := '{runtime}')",
    )
    sql(db, "DROP TABLE IF EXISTS w_days")
    page.goto(still("file://" + values))
    page.wait_for_function("window.__NB_WASM === true", timeout=60000)
    page.locator(".nb-input select").first.select_option(index=1)
    page.wait_for_timeout(500)
    number = page.locator(".nb-input input[type=number]")
    number.fill("7")
    number.dispatch_event("change")
    page.wait_for_function("document.querySelector('.nb-md').textContent.includes(' 7')", timeout=30000)
    page.wait_for_timeout(500)
    text = page.locator(".nb-md").text_content().strip()
    check(
        text == "values DOUBLE 7.0 DATE [1, 2] 1.50",
        f"html-wasm: variables and {{{{ }}}} values match the editor ({text})",
    )
    check(not errors, f"no browser errors in the html-wasm export {errors[:3]}")


# ---- Handwriting (frontend/render-ink.js, docs/HANDWRITING.md) ----------------------------------

INK_STATE = """() => ({
    canvases: document.querySelectorAll('canvas.ink-layer').length,
    hidden: document.querySelectorAll('.ink-hide').length,
    styled: [...document.querySelectorAll('[style]')].filter(e => /opacity|visibility|dash|--ink/.test(e.getAttribute('style'))).length,
    running: Ink.jobs().filter(j => j.state !== 'parked').length,
    jobs: Ink.stats.jobs })"""


FRAME_STATS = """() => {
  const f = Ink.stats.frames.filter(x => x[2] > 0), gaps = [];
  for (let i = 1; i < f.length; i++) { const g = f[i][0] - f[i - 1][0]; if (g < 300) gaps.push(g); }
  gaps.sort((a, b) => a - b);
  const work = f.map(x => x[1]).sort((a, b) => a - b), q = (a, k) => a[Math.min(a.length - 1, Math.floor(a.length * k))];
  return { frames: f.length, gap50: q(gaps, .5), gap95: q(gaps, .95), work50: q(work, .5), work95: q(work, .95), workMax: q(work, 1) };
}"""


def html_of(page, selector):
    """The page's markup, without the clip-path ids Sketch.fill picks at random."""
    return re.sub(r"skc[a-z0-9]+", "skc", page.locator(selector).evaluate("e => e.outerHTML"))


def frames(page, n=2):
    page.evaluate(
        f"() => new Promise(r => {{ let k = {n}; const f = () => --k ? requestAnimationFrame(f) : r(); requestAnimationFrame(f); }})"
    )


def settle(page):
    """Outlines follow their element's size a little later (Sketch.outline); let them."""
    page.wait_for_timeout(400)
    frames(page, 2)


def opened(page, url):
    page.goto(url)
    page.wait_for_function("window.__NB_READY === true && document.fonts.status === 'loaded'")
    frames(page, 3)


def pixel_diff(page, a, b):
    """Compares two PNG screenshots in the browser: pixels differing by more than 48 in a channel."""
    import base64

    return page.evaluate(
        """async ([a, b]) => {
      const load = async s => createImageBitmap(await (await fetch('data:image/png;base64,' + s)).blob());
      const [x, y] = [await load(a), await load(b)];
      const px = img => { const c = new OffscreenCanvas(img.width, img.height), g = c.getContext('2d'); g.drawImage(img, 0, 0); return g.getImageData(0, 0, img.width, img.height).data; };
      const p = px(x), q = px(y);
      let differ = 0, ink = 0;
      for (let i = 0; i < p.length; i += 4) {
        const d = Math.max(Math.abs(p[i] - q[i]), Math.abs(p[i + 1] - q[i + 1]), Math.abs(p[i + 2] - q[i + 2]));
        if (d > 48) differ++;
        if (Math.min(q[i], q[i + 1], q[i + 2]) < 160) ink++;
      }
      return { differ, ink, total: p.length / 4 };
    }""",
        [base64.b64encode(a).decode(), base64.b64encode(b).decode()],
    )


def test_ink(browser, work, chromium):
    revenue = os.path.join(ROOT, "examples", "revenue.sql")
    charts = os.path.join(ROOT, "examples", "charts.sql")
    db = os.path.join(work, "ink.duckdb")
    html, gallery = os.path.join(work, "ink_revenue.html"), os.path.join(work, "ink_charts.html")
    sql(
        db,
        f"FROM notebook_export('{revenue}', file := '{html}')",
        f"FROM notebook_export('{charts}', file := '{gallery}')",
    )
    url = "file://" + html
    viewport = {"width": 1300, "height": 950}

    # The reference: the same page with the animation off.
    off = browser.new_page(viewport=viewport)
    opened(off, still(url))
    settle(off)
    reference_html, reference_text = html_of(off, "#nb-root"), off.locator("#nb-root").inner_text()
    reference_shot = off.screenshot()
    check(off.evaluate(INK_STATE)["jobs"] == 0, "ink=off: the page is drawn with no handwriting")

    # 4. Mid-flight, on a stopped clock: text is all in the DOM, a canvas writes it, shapes wait.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(450)")
    frames(page, 2)
    state = page.evaluate(INK_STATE)
    hidden_paths = page.locator("svg path[style*='visibility: hidden']").count()
    check(
        page.locator("#nb-root").inner_text() == reference_text
        and state["canvases"] > 0
        and state["hidden"] > 0
        and hidden_paths > 0,
        f"part-way through, the text is complete in the DOM while a canvas writes it and paths wait ({state}, {hidden_paths} paths hidden)",
    )

    # 1. No trace: finished, the page is exactly the page drawn without the animation.
    page.evaluate("Ink.advance(60000); Ink.finishAll()")
    settle(page)
    state = page.evaluate(INK_STATE)
    check(
        state["canvases"] == 0
        and state["hidden"] == 0
        and state["styled"] == 0
        and html_of(page, "#nb-root") == reference_html,
        f"no trace: once written, the page's DOM is identical to a page drawn with the animation off ({state})",
    )
    check(not errors, f"no browser errors while writing {errors[:3]}")

    # 3. Glyph alignment: the writer's last frame is the real text, in both fonts and at several sizes.
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(60000)")
    frames(page, 2)
    page.evaluate(
        """() => {
      const box = document.createElement('div');
      box.id = 'ink-samples';
      box.style.cssText = 'position:relative;padding:8px;background:var(--nb-bg)';
      [['var(--nb-hand)', 14], ['var(--nb-hand)', 20], ['var(--nb-hand)', 42], ['var(--nb-mono)', 13.5], ['var(--nb-mono)', 22]].forEach(([f, size]) => {
        const p = document.createElement('div');
        p.style.cssText = `font-family:${f};font-size:${size}px;line-height:1.4;white-space:nowrap`;
        p.textContent = f.includes('mono') ? "SELECT sum(amount) AS revenue, 'EU' FROM t WHERE i >= 3; -- 0123" : 'Revenue review: The quick brown fox, 4,490,867.00 (APAC & EU)?';
        box.appendChild(p);
      });
      document.getElementById('nb-root').prepend(box);
      window.scrollTo(0, 0);
      Ink.present(box, 'test/samples', 'samples');
    }"""
    )
    frames(page, 2)
    job = page.evaluate(
        "(() => { const j = Ink.jobs().find(j => j.key === 'test/samples'); return { start: j.start, end: j.end, now: Ink.now() }; })()"
    )
    page.evaluate(f"Ink.advance({job['start'] + job['end'] - job['now'] - 0.5})")
    frames(page, 2)
    samples = page.locator("#ink-samples")
    last_frame = samples.screenshot()
    writing = page.evaluate("Ink.jobs().some(j => j.key === 'test/samples')")
    page.evaluate("Ink.advance(5)")
    frames(page, 2)
    real = samples.screenshot()
    diff = pixel_diff(page, last_frame, real)
    check(
        writing and diff["ink"] > 3000 and diff["differ"] < diff["ink"] * 0.01,
        f"a glyph's last frame matches the real text it hands over to ({diff['differ']} of {diff['ink']} inked pixels differ)",
    )

    # 2. Same pixels, and 7. the budget, on a running clock.
    page = browser.new_page(viewport=viewport)
    page.goto(url)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    took = page.evaluate("performance.now() - Math.min(...Ink.plans.map(p => p.at))")
    shot = page.screenshot()
    diff = pixel_diff(page, shot, reference_shot)
    check(
        diff["differ"] == 0,
        f"same pixels: the first screen, written, looks exactly as it does without the animation ({diff['differ']} pixels differ)",
    )
    check(took < 2500 * 1.25, f"revenue.sql: the first screen is written within the budget ({took:.0f} ms)")

    page = browser.new_page(viewport=viewport)
    page.goto("file://" + gallery)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    took = page.evaluate("performance.now() - Math.min(...Ink.plans.map(p => p.at))")
    for step in range(12):
        page.mouse.wheel(0, 800)
        page.wait_for_timeout(1500)
    smooth = page.evaluate(FRAME_STATS)
    print(
        f"     charts.sql: first screen in {took:.0f} ms; frames while writing: "
        + ", ".join(f"{k} {v:.1f}" for k, v in smooth.items())
    )
    check(
        took < 2500 * 1.25 and smooth["gap95"] < 34,
        f"charts.sql: the first screen is written within the budget, at a smooth frame rate ({took:.0f} ms)",
    )

    # A reader's own change (a tab) is drawn at once, not written out at length.
    page = browser.new_page(viewport=viewport)
    page.goto(url)
    page.wait_for_function("Ink.plans.length > 0")
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    tab = page.locator(".nb-cell[data-idx='7'] .nb-tab", has_text="table")
    tab.scroll_into_view_if_needed()
    page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
    tab.click()
    quick = page.evaluate("Ink.plans[Ink.plans.length - 1]")
    page.wait_for_timeout(250)
    check(
        quick["dur"] <= 200 and page.evaluate(INK_STATE)["running"] == 0,
        f"switching a tab draws the new tab within 200 ms ({quick['key']}, {quick['dur']} ms)",
    )

    # 8. Switches: each gives the complete page and no jobs.
    for name, setup in (
        ("reduced motion", {"reduced_motion": "reduce"}),
        ("print media", None),
        ("the gear menu's setting", "localStorage.setItem('nb.ink', 'off')"),
    ):
        page = browser.new_page(viewport=viewport, **(setup if isinstance(setup, dict) else {}))
        if name == "print media":
            page.emulate_media(media="print")
        elif isinstance(setup, str):
            page.add_init_script(setup)
        opened(page, url)
        settle(page)
        state = page.evaluate(INK_STATE)
        # Print media lays the page out differently: compare only what does not depend on layout.
        same = html_of(page, "#nb-root") == reference_html or name == "print media"
        check(state["jobs"] == 0 and state["canvases"] == 0 and same, f"{name} switches the handwriting off ({state})")
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url)
    page.evaluate("Ink.advance(300)")
    frames(page, 2)
    page.evaluate("window.dispatchEvent(new Event('beforeprint'))")
    settle(page)
    state = page.evaluate(INK_STATE)
    check(
        state["canvases"] == 0
        and state["hidden"] == 0
        and state["running"] == 0
        and html_of(page, "#nb-root") == reference_html,
        f"printing part-way through finishes the page at once ({state})",
    )
    page.evaluate("Ink.advance(300)")
    page.keyboard.press("Escape")
    check(page.evaluate(INK_STATE)["running"] == 0, "a key press finishes everything on screen")

    # 9. PDF: the page handed to Chrome is marked not to animate.
    wrapper = os.path.join(work, "chrome.sh")
    with open(wrapper, "w") as f:
        f.write(
            f"#!/bin/sh\nfor a in \"$@\"; do case $a in file://*) cp \"${{a#file://}}\" '{work}/printed.html';; esac; done\nexec '{chromium}' \"$@\"\n"
        )
    os.chmod(wrapper, 0o755)
    pdf = os.path.join(work, "ink.pdf")
    sql(db, f"FROM notebook_export('{revenue}', file := '{pdf}', format := 'pdf', chrome := '{wrapper}')")
    printed = open(os.path.join(work, "printed.html")).read()
    check(
        open(pdf, "rb").read(5) == b"%PDF-" and '<html lang="en" data-ink="off" data-width-fixed="normal">' in printed,
        "PDF export prints a page marked not to animate, at the normal width (DESIGN §22)",
    )
    page = browser.new_page(viewport=viewport)
    page.goto("file://" + os.path.join(work, "printed.html"))
    page.wait_for_function("window.__NB_READY === true")
    frames(page, 3)
    check(page.evaluate(INK_STATE)["jobs"] == 0, "the page Chrome prints draws without handwriting")


def test_ink_editor(browser, work):
    # 5 and 6 in the editor, whose render() rebuilds every cell on every change.
    notebook = os.path.join(work, "ink_editor.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    shell, url = serve(work, notebook)
    try:
        off = browser.new_page(viewport={"width": 1400, "height": 1000})
        off.goto(still(url))
        off.wait_for_selector(".nb-cell")
        off.wait_for_function("document.fonts.status === 'loaded'")
        settle(off)
        reference = html_of(off, "#cells")

        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        errors = watch(page)
        page.add_init_script("window.__INK_PAUSE = true")
        page.goto(url)
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.fonts.status === 'loaded'")
        frames(page, 3)
        page.evaluate("Ink.advance(700)")
        frames(page, 2)
        before = page.evaluate(INK_STATE)
        before_hidden = page.locator("#cells [style*='visibility: hidden']").count()
        page.evaluate("App.render()")
        frames(page, 2)
        after = page.evaluate(INK_STATE)
        after_hidden = page.locator("#cells [style*='visibility: hidden']").count()
        check(
            before["running"] > 0
            and after["jobs"] == before["jobs"]
            and after["running"] == before["running"]
            and after_hidden == before_hidden,
            f"a render part-way through continues each piece where it was ({before} -> {after}, hidden paths {before_hidden} -> {after_hidden})",
        )
        page.evaluate("Ink.advance(10000)")
        frames(page, 3)
        page.evaluate("Ink.finishAll()")
        settle(page)
        done = page.evaluate(INK_STATE)
        check(
            done["canvases"] == 0 and done["styled"] == 0 and html_of(page, "#cells") == reference,
            f"no trace in the editor: written, its cells are exactly the cells drawn without the animation ({done})",
        )
        page.evaluate("App.render()")
        frames(page, 3)
        again = page.evaluate(INK_STATE)
        check(
            again["jobs"] == done["jobs"] and again["canvases"] == 0,
            f"rendering again after everything is written writes nothing ({again})",
        )
        # Typing never writes: code is keyed to the notebook as opened, not to its text.
        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        page.goto(url)
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=10000)
        codes = page.evaluate("Ink.plans.filter(p => p.key.endsWith('/code')).length")
        area = page.locator(".nb-cell[data-idx='3'] textarea")
        area.click()
        area.press("End")
        area.type(" -- typed", delay=20)
        page.wait_for_timeout(900)
        check(
            page.evaluate("Ink.plans.filter(p => p.key.endsWith('/code')).length") == codes
            and page.locator(".nb-cell[data-idx='3'] canvas.ink-layer").count() == 0,
            "typing in a cell writes nothing",
        )
        page.click("#gear")
        page.select_option("#ink", "off")
        page.keyboard.press("Escape")
        check(
            page.evaluate("localStorage.getItem('nb.ink')") == "off" and not page.evaluate("Ink.enabled()"),
            "the gear menu switches the handwriting off",
        )
        check(not errors, f"no browser errors in the written editor {errors[:3]}")

        # Watching in the editor: the gear menu's "watch" writes the page again with one pen, and a run
        # that finishes while the pen is on an early cell waits its turn in reading order.
        page = browser.new_page(viewport={"width": 1400, "height": 1000})
        errors = watch(page)
        page.add_init_script("window.__INK_PAUSE = true; localStorage.removeItem('nb.ink')")
        page.goto(url)
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.fonts.status === 'loaded'")
        frames(page, 3)
        page.evaluate("Ink.finishAll()")
        page.click("#gear")
        page.select_option("#ink", "watch")
        page.click("#gear")
        frames(page, 3)
        page.evaluate("Ink.advance(4000)")
        frames(page, 2)
        st = page.evaluate(WATCH_STATE)
        check(
            st["mode"] == "watch"
            and len(st["running"]) == 1
            and st["waiting"] > 10
            and page.evaluate("localStorage.getItem('nb.ink')") == "watch",
            f"the gear menu's watch writes the page again, one piece at a time ({st})",
        )
        first = page.evaluate("Ink.plans.length")
        page.click("#run-all")
        page.wait_for_function("!App.running", timeout=30000)
        page.wait_for_timeout(500)
        page.evaluate("localStorage.setItem('nb.ink.speed', '8')")
        page.keyboard.press("+")
        for _ in range(300):
            page.evaluate("Ink.advance(1000)")
            frames(page, 1)
            if page.evaluate("Ink.jobs().length") == 0:
                break
        ids = [
            int(re.match(r"cell(\d+)/", k).group(1))
            for k in page.evaluate(f"Ink.plans.slice({first}).filter(p => p.watch).map(p => p.key)")
            if k.startswith("cell")
        ]
        drops = [i for i in range(1, len(ids)) if ids[i] < ids[i - 1]]
        check(
            ids and len(drops) <= 1,
            f"after Run all the pen goes on in reading order, going back at most once for results above it ({ids})",
        )
        settle(page)
        done = page.evaluate(INK_STATE)
        check(
            page.evaluate("Ink.jobs().length") == 0 and done["canvases"] == 0 and done["styled"] == 0,
            f"the watched editor ends with no trace ({done})",
        )
        check(not errors, f"no browser errors watching the editor {errors[:3]}")
    finally:
        stop(shell)


# One pen at a time: the running piece, and how many of its atoms (not counting the second pass of a
# doubled line, which a real hand draws with the first) are in motion now.
WATCH_STATE = """() => {
  const running = Ink.jobs().filter(j => j.state === 'running');
  const rel = running.length ? Ink.penNow() - running[0].start : 0;
  const moving = running.length ? running[0].atoms.filter(a => !a.with && !a.instant && a.t0 <= rel && rel < a.t1).length : 0;
  return { running: running.map(j => j.key), moving, waiting: Ink.jobs().filter(j => j.state === 'parked').length,
           mark: !!document.querySelector('.ink-watch'), nib: !!document.querySelector('.ink-nib'),
           held: Ink.held(), speed: Ink.speed(), following: Ink.following(), pen: Ink.penNow(), mode: Ink.mode() };
}"""


def advance(page, ms, step=100):
    for _ in range(int(ms / step)):
        page.evaluate(f"Ink.advance({step})")
        frames(page, 1)


def test_ink_watch(browser, work):
    """Watch mode (ink=watch): one pen writes the page in reading order, at a pace to watch."""
    revenue = os.path.join(ROOT, "examples", "revenue.sql")
    db = os.path.join(work, "watch.duckdb")
    html = os.path.join(work, "watch_revenue.html")
    sql(db, f"FROM notebook_export('{revenue}', file := '{html}')")
    url = "file://" + html
    viewport = {"width": 1300, "height": 950}

    off = browser.new_page(viewport=viewport)
    opened(off, still(url))
    settle(off)
    reference = html_of(off, "#nb-root")

    # The URL flag opens straight into watch mode; one thing is in motion at a time.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url + "?ink=watch")
    states, keys = [], []
    for _ in range(120):
        page.evaluate("Ink.advance(50)")
        frames(page, 1)
        st = page.evaluate(WATCH_STATE)
        states.append(st)
        if st["running"] and (not keys or keys[-1] != st["running"][0]):
            keys.append(st["running"][0])
    check(
        states[-1]["mode"] == "watch"
        and all(len(st["running"]) <= 1 for st in states)
        and max(st["moving"] for st in states) == 1,
        f"ink=watch: one piece at a time, and one stroke or letter of it at a time (pieces {keys})",
    )
    check(
        states[-1]["mark"] and states[-1]["nib"] and states[-1]["waiting"] > 10,
        f"the controls and the nib show while the pen has work ({states[-1]})",
    )
    plans = page.evaluate("Ink.plans.filter(p => p.watch).map(p => p.key)")
    cells = [int(re.match(r"cell(\d+)/", k).group(1)) for k in plans if k.startswith("cell")]
    check(plans and plans[0] == "header" and cells == sorted(cells), f"pieces are written in reading order ({plans})")
    check(
        page.evaluate("document.querySelectorAll('.nb-cell canvas.ink-layer').length") <= 2,
        "only the piece being written has canvases over it",
    )

    # Pause and resume, on the pen's own clock.
    page.keyboard.press("Space")
    held = page.evaluate(WATCH_STATE)
    page.evaluate("Ink.advance(3000)")
    frames(page, 2)
    still_held = page.evaluate(WATCH_STATE)
    check(
        held["held"] and still_held["pen"] == held["pen"] and still_held["running"] == held["running"],
        "space pauses the pen: time passes and nothing moves",
    )
    page.keyboard.press("Space")
    page.evaluate("Ink.advance(200)")
    frames(page, 2)
    check(page.evaluate(WATCH_STATE)["pen"] > held["pen"], "space again goes on from where it was")

    # Faster and slower.
    speed = page.evaluate("Ink.speed()")
    page.keyboard.press("+")
    faster = page.evaluate("Ink.speed()")
    page.keyboard.press("-")
    page.keyboard.press("-")
    slower = page.evaluate("Ink.speed()")
    page.keyboard.press("+")
    check(
        faster > speed > slower and page.evaluate("Ink.speed()") == speed,
        f"+ and - change the pen's speed ({slower} < {speed} < {faster})",
    )

    # Skip to the end of the piece being written: the next one starts.
    before = page.evaluate(WATCH_STATE)["running"]
    page.keyboard.press("n")
    page.evaluate("Ink.advance(1200)")
    frames(page, 2)
    after = page.evaluate(WATCH_STATE)["running"]
    done = page.evaluate("k => !Ink.jobs().some(j => j.key === k)", before[0]) if before else False
    check(
        before and after and before != after and done,
        f"n finishes the piece being written and the pen moves on ({before} -> {after})",
    )

    # Follow the pen: the page has scrolled down with it. A reader's wheel stops that.
    advance(page, 70000, 500)
    scrolled = page.evaluate("scrollY")
    check(scrolled > 200, f"the page scrolls to keep the pen in view ({scrolled} px)")
    page.mouse.move(600, 400)
    page.mouse.wheel(0, -2000)
    page.wait_for_timeout(200)
    mine = page.evaluate("scrollY")
    page.evaluate("Ink.advance(300)")
    frames(page, 3)
    check(
        not page.evaluate("Ink.following()") and page.evaluate("scrollY") == mine,
        "scrolling away stops the page following the pen",
    )
    page.keyboard.press("f")
    check(page.evaluate("Ink.following()"), "a key press follows the pen again")

    # Everything at once: the page is exactly the page drawn without the animation.
    page.keyboard.press("e")
    settle(page)
    state = page.evaluate(INK_STATE)
    final = page.evaluate(WATCH_STATE)
    check(
        state["canvases"] == 0
        and state["hidden"] == 0
        and state["styled"] == 0
        and html_of(page, "#nb-root") == reference
        and not final["mark"]
        and not final["nib"],
        f"e writes everything now, and the page is identical to one drawn without the animation, controls gone ({state})",
    )

    # A reader's own action updates at once, without the pen.
    tab = page.locator(".nb-cell[data-idx='7'] .nb-tab", has_text="table")
    tab.scroll_into_view_if_needed()
    tab.click()
    frames(page, 2)
    st = page.evaluate(WATCH_STATE)
    check(
        not st["running"] and st["waiting"] == 0 and page.evaluate(INK_STATE)["canvases"] == 0,
        f"switching a tab while watching shows it at once ({st})",
    )
    check(not errors, f"no browser errors while watching {errors[:3]}")

    # The pen writes to the end by itself, and leaves no trace.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true; localStorage.setItem('nb.ink.speed', '8')")
    opened(page, url + "?ink=watch")
    seen = []
    for _ in range(2000):
        page.evaluate("Ink.advance(250)")
        frames(page, 1)
        seen.append(page.evaluate(WATCH_STATE))
        if page.evaluate("Ink.jobs().length") == 0:
            break
    plans = page.evaluate("Ink.plans.filter(p => p.watch).map(p => p.key)")
    cells = [int(re.match(r"cell(\d+)/", k).group(1)) for k in plans if k.startswith("cell")]
    check(
        plans[0] == "header" and cells == sorted(cells) and len(set(cells)) > 8,
        f"the whole page is written in reading order ({len(plans)} pieces)",
    )
    check(
        all(len(st["running"]) <= 1 and st["moving"] <= 1 for st in seen),
        "from start to end, one piece and one stroke or letter in motion at a time",
    )
    settle(page)
    state = page.evaluate(INK_STATE)
    check(
        page.evaluate("Ink.jobs().length") == 0
        and state["canvases"] == 0
        and state["styled"] == 0
        and html_of(page, "#nb-root") == reference,
        f"watched to the end, the page is identical to one drawn without the animation ({state})",
    )
    check(not errors, f"no browser errors watching to the end {errors[:3]}")

    # The export's watch button writes the page again; Escape leaves the mode.
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, still(url).replace("ink=off", "ink=gentle"))
    page.evaluate("Ink.finishAll()")
    page.click("button.ink-watch-start")
    frames(page, 3)
    page.evaluate("Ink.advance(500)")
    frames(page, 2)
    st = page.evaluate(WATCH_STATE)
    check(
        st["mode"] == "watch" and st["waiting"] > 10 and len(st["running"]) == 1,
        f"the export's watch button writes the page again by one pen ({st})",
    )
    page.keyboard.press("Escape")
    frames(page, 2)
    st = page.evaluate(WATCH_STATE)
    check(
        st["mode"] == "gentle" and not st["running"] and st["waiting"] == 0 and not st["mark"],
        f"Escape leaves watch mode with the page complete ({st})",
    )

    # Reduced motion still means no animation, whatever the URL asks.
    page = browser.new_page(viewport=viewport, reduced_motion="reduce")
    opened(page, url + "?ink=watch")
    settle(page)
    check(
        page.evaluate(INK_STATE)["jobs"] == 0 and not page.locator("button.ink-watch-start").is_visible(),
        "reduced motion switches watch mode off too",
    )

    # Presenting while watching: each screen is written by the one pen, and moving on goes on with it.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url + "?ink=watch#present")
    advance(page, 3000, 100)
    first = page.evaluate(WATCH_STATE)
    page.keyboard.press("ArrowRight")
    frames(page, 2)
    advance(page, 3000, 100)
    second = page.evaluate(WATCH_STATE)
    check(
        page.evaluate("document.body.classList.contains('presenting')")
        and len(first["running"]) == 1
        and len(second["running"]) == 1
        and first["running"] != second["running"]
        and first["mode"] == "watch",
        f"presenting while watching: each screen is written by one pen ({first['running']} -> {second['running']})",
    )
    check(not errors, f"no browser errors presenting while watching {errors[:3]}")

    # Frame times on a running clock: one pen is cheap.
    page = browser.new_page(viewport=viewport)
    page.add_init_script("localStorage.setItem('nb.ink.speed', '2')")
    opened(page, url + "?ink=watch")
    page.wait_for_timeout(15000)
    smooth = page.evaluate(FRAME_STATS)
    print("     watching revenue.sql: " + ", ".join(f"{k} {v:.1f}" for k, v in smooth.items()))
    check(smooth["gap95"] < 34 and smooth["work95"] < 8, "watching keeps a smooth frame rate")


# ---- Presenting (frontend/render-present.js, docs/DESIGN.md §18) ---------------------------------

PRESENT_NOTEBOOK = (
    """-- %% [markdown]
/*
# Screens
*/

-- %% numbers hide=code
SELECT range AS n, range * range AS square FROM range(6);

-- %% [markdown] screen=join
/*
A note that stays with the table above it.
*/

-- %% secret hide=all
SELECT 'never shown' AS note;

-- %% chart left from=numbers span=6
/*
mark: bar
x: {field: n, type: ordinal}
y: square
*/

-- %% chart right from=numbers span=6
/*
mark: line
x: n
y: square
*/

-- %% chart alone from=numbers span=6 screen=new
/*
mark: point
x: n
y: square
*/

-- %% skipped screen=skip
SELECT 'left out of the presentation' AS note;

-- %% [markdown]
/*
"""
    + "\n\n".join(f"Line {i} of a screen that is taller than any window." for i in range(60))
    + """
*/
"""
)

PRESENT_STATE = """() => ({
    on: document.body.classList.contains('presenting'),
    page: (document.querySelector('.nb-present-page') || {}).textContent || '',
    cells: [...document.querySelectorAll('.nb-cell')].map(c => +c.dataset.idx),
    hash: location.hash, scale: Present.scale })"""


def present_state(page):
    return page.evaluate(PRESENT_STATE)


def test_present(browser, work):
    db = os.path.join(work, "present.duckdb")
    notebook = os.path.join(work, "screens.sql")
    with open(notebook, "w") as f:
        f.write(PRESENT_NOTEBOOK)
    screens, pre = os.path.join(work, "screens.html"), os.path.join(work, "present_pre.html")
    revenue = os.path.join(ROOT, "examples", "revenue.sql")
    sql(
        db,
        f"FROM notebook_export('{notebook}', file := '{screens}')",
        f"FROM notebook_export('{revenue}', file := '{pre}', interactive := 'precomputed')",
    )
    viewport = {"width": 1366, "height": 768}

    # How screens are formed, as a function of the cells alone.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.goto(still("file://" + screens))
    page.wait_for_function("window.__NB_READY === true")
    formed = page.evaluate(
        """() => {
      const cell = (kind, span, screen, visible) => ({ kind, span, screen, visible: visible !== false });
      return JSON.stringify([
        Present.screens([cell('markdown'), cell('sql'), cell('chart', 6), cell('chart', 6), cell('chart', 6)]),
        Present.screens([cell('input', 6), cell('input', 6), cell('chart', 4), cell('chart', 4), cell('chart', 4), cell('sql')]),
        Present.screens([cell('sql'), cell('markdown', 12, 'join'), cell('chart', 6), cell('chart', 6, 'new'), cell('sql', 12, 'skip'), cell('sql', 12, null, false)])
      ]);
    }"""
    )
    check(
        formed == "[[[0],[1],[2,3],[4]],[[0,1,2,3,4],[5]],[[0,1],[2],[3]]]",
        f"a screen is a row of the reading layout; inputs go with what follows; screen= joins, splits and skips ({formed})",
    )

    # Entering from an export's quiet control; code is hidden as in the reading view.
    check(
        page.locator("pre.nb-source").count() > 0 and not present_state(page)["on"],
        "an export opens as a page, with its code",
    )
    start = page.locator("button.nb-present-start")
    check(
        start.is_visible() and start.evaluate("e => getComputedStyle(e).borderTopWidth") == "0px",
        "an export has a quiet control to present it",
    )
    start.click()
    st = present_state(page)
    check(
        st["on"] and st["page"] == "1 / 5" and st["cells"] == [0] and st["hash"] == "#present",
        f"the control opens presenting on the first screen, with a page indicator ({st})",
    )
    mark = page.locator(".nb-present-page")
    check(
        mark.evaluate("e => getComputedStyle(e).color") == "rgb(134, 142, 150)"
        and "Excalifont" in mark.evaluate("e => getComputedStyle(e).fontFamily")
        and not page.locator(".nb-viewer-header").is_visible(),
        "the indicator is in the handwriting, in the muted ink, and nothing else is on the page",
    )
    grid = page.locator(".nb-cells").bounding_box()
    check(
        st["scale"] > 1.1
        and abs(grid["x"] - (1366 - grid["x"] - grid["width"])) < 2
        and grid["x"] > 60
        and abs(grid["y"] - (768 - grid["y"] - grid["height"])) < 2,
        f"a screen is drawn larger than the page and centred, with margins (scale {st['scale']:.2f})",
    )

    # Moving: keys, and clicks on the page.
    def moves(action, expect_page, expect_cells, message):
        action()
        st = present_state(page)
        check(
            st["page"] == expect_page and st["cells"] == expect_cells, f"{message} ({st['page']}, cells {st['cells']})"
        )

    moves(
        lambda: page.keyboard.press("ArrowRight"),
        "2 / 5",
        [1, 2],
        "Right arrow moves on; screen=join keeps a cell with the one before it",
    )
    check(
        page.locator("pre.nb-source").count() == 0
        and page.locator(".nb-cell[data-idx='1'] table.nb-grid").count() == 1,
        "presenting shows results, not code",
    )
    moves(
        lambda: page.keyboard.press(" "),
        "3 / 5",
        [4, 5],
        "Space moves on; cells that share a row share a screen, and hide=all has none",
    )
    moves(lambda: page.keyboard.press("PageDown"), "4 / 5", [6], "Page Down moves on; screen=new starts a screen")
    alone = page.locator(".nb-cells").bounding_box()
    check(
        abs(alone["x"] - (1366 - alone["x"] - alone["width"])) < 2,
        "a cell narrower than the page is centred on its screen",
    )
    moves(lambda: page.keyboard.press("ArrowLeft"), "3 / 5", [4, 5], "Left arrow goes back")
    moves(lambda: page.keyboard.press("PageUp"), "2 / 5", [1, 2], "Page Up goes back")
    moves(lambda: page.keyboard.press("End"), "5 / 5", [8], "End jumps to the last screen; screen=skip has none")
    st = present_state(page)
    tall = page.evaluate(
        "(() => { const r = document.getElementById('nb-root'); return [r.scrollHeight, r.clientHeight]; })()"
    )
    check(
        abs(st["scale"] - 0.8) < 0.01 and tall[0] > tall[1] + 500,
        f"a screen far taller than the window is drawn a little smaller than the reading view, no further, and scrolls (scale {st['scale']:.2f}, {tall[0]} > {tall[1]})",
    )
    moves(lambda: page.keyboard.press("ArrowRight"), "5 / 5", [8], "there is nothing after the last screen")
    moves(lambda: page.keyboard.press("Home"), "1 / 5", [0], "Home jumps to the first screen")
    moves(lambda: page.mouse.click(40, 400), "2 / 5", [1, 2], "a click on the page moves on")
    moves(
        lambda: page.locator(".nb-cell[data-idx='1'] td").first.click(), "3 / 5", [4, 5], "so does a click on a table"
    )
    page.keyboard.press("ArrowLeft")
    moves(lambda: page.locator(".nb-cell[data-idx='2'] .nb-md").click(), "3 / 5", [4, 5], "and a click on text")
    page.keyboard.press("ArrowRight")
    moves(lambda: page.locator("svg.nb-chart-svg").click(), "4 / 5", [6], "a click on a chart does not move on")
    check(present_state(page)["hash"] == "#present=4", "the screen is kept in the URL hash")
    page.reload()
    page.wait_for_function("window.__NB_READY === true")
    moves(lambda: None, "4 / 5", [6], "a reload comes back to the same screen")

    # Leaving.
    page.keyboard.press("Escape")
    st = present_state(page)
    check(
        not st["on"]
        and st["cells"] == [0, 1, 2, 4, 5, 6, 7, 8]
        and st["hash"] == ""
        and page.locator("pre.nb-source").count() > 0
        and page.locator(".nb-present-mark").count() == 0
        and page.locator(".nb-cells").evaluate("e => e.getAttribute('style') || ''") == "",
        f"Escape leaves: the whole page is back, code and all, with nothing left behind ({st})",
    )
    box = page.locator(".nb-cell[data-idx='6']").bounding_box()
    check(0 <= box["y"] < 768, "leaving lands on the cell that was being presented")
    page.locator("button.nb-present-start").click()
    page.locator("button.nb-present-leave").click()
    check(not present_state(page)["on"], "the faint ✕ beside the page number leaves too")

    # A link can open straight into presenting. Print gets the whole notebook.
    page.goto("file://" + screens + "?ink=off#present=3")
    page.wait_for_function("window.__NB_READY === true")
    moves(lambda: None, "3 / 5", [4, 5], "#present=3 in a link opens the third screen")
    page.goto("about:blank")
    page.goto("file://" + screens + "?ink=off#present=99")
    page.wait_for_function("window.__NB_READY === true")
    st = present_state(page)
    check(
        st["page"] == "5 / 5" and st["hash"] == "#present=5",
        f"#present=99 shows the last screen and says so in the URL ({st})",
    )
    page.evaluate("window.dispatchEvent(new Event('beforeprint'))")
    st = present_state(page)
    check(not st["on"] and len(st["cells"]) == 8, "printing leaves presenting, so paper gets the whole notebook")
    page.emulate_media(media="print")
    check(not page.locator("button.nb-present-start").is_visible(), "the control is not printed")
    page.emulate_media(media="screen")
    for width, height in ((1920, 1080), (700, 500)):
        page.set_viewport_size({"width": width, "height": height})
        page.goto("file://" + screens + f"?ink=off&v={width}#present=3")
        page.wait_for_function("window.__NB_READY === true")
        page.wait_for_timeout(200)
        g, st = page.locator(".nb-cells").bounding_box(), present_state(page)
        check(
            g["x"] >= 20
            and g["x"] + g["width"] <= width - 20
            and g["y"] >= 20
            and g["y"] + g["height"] <= height - 20
            and page.evaluate("document.documentElement.scrollWidth <= innerWidth"),
            f"at {width}x{height} the screen fits the window with margins (scale {st['scale']:.2f})",
        )
    check(not errors, f"no browser errors presenting an export {errors[:3]}")

    # Inputs stay live: a precomputed export switches results without moving on.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.goto("file://" + pre + "?ink=off#present=3")
    page.wait_for_function("window.__NB_READY === true")
    st = present_state(page)
    check(st["cells"] == [4, 5, 6], f"inputs are presented with the cell that follows them ({st})")
    regions = "() => [...new Set([...document.querySelectorAll(\".nb-cell[data-idx='6'] tbody td:nth-child(4)\")].map(e => e.textContent))].join()"
    before = page.evaluate(regions)
    page.locator(".nb-input select").first.select_option(label="US")
    page.wait_for_function(f"({regions})() === 'US'")
    st = present_state(page)
    check(
        before != "US" and st["page"].startswith("3 /") and st["cells"] == [4, 5, 6],
        f"an input changes what depends on it and stays on its screen ({before} -> US, {st['page']})",
    )
    page.keyboard.press("ArrowRight")
    check(
        present_state(page)["page"].startswith("4 /"),
        "after a value is picked, the arrow keys move between screens again",
    )
    page.keyboard.press("Home")
    check("US" in page.locator(".nb-md").first.text_content(), "the other screens show the new value")
    check(not errors, f"no browser errors presenting a precomputed export {errors[:3]}")

    # The pen. Each screen is written as it arrives, scaled with it; one already shown is not written again.
    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    page.add_init_script("window.__INK_PAUSE = true")
    page.goto("file://" + screens + "#present")
    page.wait_for_function("window.__NB_READY === true && document.fonts.status === 'loaded'")
    frames(page, 3)
    page.evaluate("Ink.advance(300)")
    frames(page, 2)
    state = page.evaluate(INK_STATE)
    check(
        state["canvases"] > 0 and state["hidden"] > 0 and page.locator(".nb-cells").inner_text().strip() == "Screens",
        f"a screen is written as it arrives: part-way through, a canvas writes text that is already in the DOM ({state})",
    )
    job = page.evaluate(
        "(() => { const j = Ink.jobs().filter(j => j.state === 'running' && j.textRoot && /^p1cell/.test(j.key)).sort((a, b) => b.start + b.end - a.start - a.end)[0]; return { start: j.start, end: j.end, now: Ink.now(), z: j.z }; })()"
    )
    page.evaluate(f"Ink.advance({job['start'] + job['end'] - job['now'] - 0.5})")
    frames(page, 2)
    heading = page.locator(".nb-cells")
    last_frame = heading.screenshot()
    writing = page.evaluate(INK_STATE)["canvases"] > 0
    page.evaluate("Ink.advance(5)")
    frames(page, 2)
    diff = pixel_diff(page, last_frame, heading.screenshot())
    check(
        writing and job["z"] > 1.1 and diff["ink"] > 1000 and diff["differ"] < diff["ink"] * 0.02,
        f"on a scaled screen the pen's last frame matches the text it hands over to (scale {job['z']:.2f}, {diff['differ']} of {diff['ink']} inked pixels differ)",
    )
    page.keyboard.press("ArrowRight")
    frames(page, 3)
    plan = page.evaluate("Ink.plans.filter(p => /^p1cell1\\//.test(p.key)).pop()")
    check(
        page.evaluate(INK_STATE)["running"] > 0 and plan and plan["dur"] > 200,
        f"moving on writes the next screen at the pen's own pace, not as a quick update ({plan})",
    )
    page.evaluate("Ink.advance(60000)")
    frames(page, 3)
    written = page.evaluate(INK_STATE)["jobs"]
    page.keyboard.press("ArrowLeft")
    frames(page, 3)
    back = page.evaluate(INK_STATE)
    # Only the page number is new.
    check(
        back["jobs"] == written + 1 and page.evaluate("Ink.jobs().every(j => j.key === 'present/mark')"),
        f"going back to a screen already shown writes nothing: it is there at once ({written} -> {back['jobs']} jobs)",
    )
    page.evaluate("Ink.advance(60000)")
    frames(page, 3)
    page.keyboard.press("Escape")
    page.evaluate("Ink.advance(60000); Ink.finishAll()")
    settle(page)
    done = page.evaluate(INK_STATE)
    off = browser.new_page(viewport=viewport)
    off.goto(still("file://" + screens))
    off.wait_for_function("window.__NB_READY === true && document.fonts.status === 'loaded'")
    settle(off)
    check(
        done["canvases"] == 0
        and done["hidden"] == 0
        and done["styled"] == 0
        and html_of(page, "#nb-root") == html_of(off, "#nb-root"),
        f"no trace: after presenting, the page is exactly the page drawn without it ({done})",
    )
    check(not errors, f"no browser errors while a presentation is written {errors[:3]}")


def test_present_editor(browser, work):
    notebook = os.path.join(work, "present_editor.sql")
    shutil.copy(os.path.join(ROOT, "examples", "revenue.sql"), notebook)
    shell, url = serve(work, notebook)
    try:
        page = browser.new_page(viewport={"width": 1366, "height": 768})
        errors = watch(page)
        # A link that opens straight into presenting is not the reader asking to run anything (FORMAT §8).
        runs = []
        page.on("request", lambda r: runs.append(r.url) if "/api/run" in r.url else None)
        page.goto(still(url) + "#present")
        page.wait_for_selector(".nb-present-page")
        page.wait_for_timeout(1500)
        check(
            not runs and not page.evaluate("App.live"),
            f"opening the editor at #present runs nothing ({len(runs)} runs)",
        )
        page.keyboard.press("Escape")
        page.wait_for_selector(".nb-cell")
        page.click("#gear")
        check(page.locator("#view-present").is_visible(), "the gear menu has Present")
        page.click("#view-present")
        # Nothing had run: presenting runs the notebook first.
        page.wait_for_function(
            "(document.querySelector('.nb-present-page') || {}).textContent === '1 / 7'", timeout=30000
        )
        st = present_state(page)
        check(
            st["on"]
            and st["cells"] == [1]
            and not page.locator(".nb-toolbar").is_visible()
            and not page.locator("#menu").is_visible()
            and page.locator("textarea:visible").count() == 0
            and page.locator(".nb-tools:visible, .nb-margin:visible, .nb-add:visible").count() == 0,
            f"Present shows the first screen alone: no toolbar, no code, no cell chrome ({st})",
        )
        check(
            "EU" in page.locator(".nb-md").text_content() and st["scale"] > 1.1,
            "a notebook that had not run is run before it is presented",
        )
        page.keyboard.press("ArrowRight")
        page.keyboard.press("ArrowRight")
        st = present_state(page)
        check(
            st["page"] == "3 / 7" and st["cells"] == [4, 5, 6] and st["hash"] == "#present=3",
            f"the inputs share a screen with the query after them ({st})",
        )

        # Inputs work, and do not move on.
        regions = "() => [...new Set([...document.querySelectorAll(\".nb-cell[data-idx='6'] tbody td:nth-child(4)\")].map(e => e.textContent))].join()"
        page.locator(".nb-input select").first.select_option(label="US")
        page.wait_for_function(f"({regions})() === 'US'", timeout=30000)
        st = present_state(page)
        check(
            st["page"] == "3 / 7" and st["cells"] == [4, 5, 6],
            f"an input re-runs what reads it and stays on its screen ({st['page']})",
        )
        slider = page.locator(".nb-input input[type=range]")
        slider.fill("300")
        slider.dispatch_event("change")
        page.wait_for_function(
            "[...document.querySelectorAll(\".nb-cell[data-idx='6'] tbody td:nth-child(6)\")].every(e => parseFloat(e.textContent.replace(/,/g, '')) >= 300)",
            timeout=30000,
        )
        check(present_state(page)["page"] == "3 / 7", "so does a slider")

        # Brushing a chart filters what depends on it, without moving on.
        page.keyboard.press("ArrowRight")
        overlay = page.locator(".nb-cell[data-idx='7'] .nb-brush-overlay")
        box = overlay.bounding_box()
        page.evaluate("App.status('')")
        page.mouse.move(box["x"] + box["width"] * 0.2, box["y"] + 20)
        page.mouse.down()
        page.mouse.move(box["x"] + box["width"] * 0.3, box["y"] + 40, steps=5)
        page.mouse.up()
        page.wait_for_function(
            "!App.running && document.getElementById('status').textContent.startsWith('Ran')", timeout=30000
        )
        check(
            present_state(page)["page"] == "4 / 7" and page.locator(".nb-cell[data-idx='7'] .nb-brush").count() >= 1,
            "brushing a chart does not move on",
        )
        page.keyboard.press("End")
        st = present_state(page)
        months = page.locator(".nb-cell[data-idx='10'] table.nb-grid tbody tr td:nth-child(3)").all_text_contents()
        check(
            st["page"] == "7 / 7" and len(months) > 0 and all(m[5:7] in ("03", "04") for m in months),
            f"the brush has filtered a later screen ({sorted(set(m[:7] for m in months))})",
        )

        # A reload stays in the presentation, and Escape gives the editor back as it was.
        page.reload()
        page.wait_for_function(
            "(document.querySelector('.nb-present-page') || {}).textContent === '7 / 7'", timeout=30000
        )
        check(present_state(page)["cells"] == [10], "a reload comes back to the same screen")
        page.wait_for_function("App.live && !App.running", timeout=30000)
        check(True, "after a reload the session is still live: inputs and brushes keep working")
        page.keyboard.press("Escape")
        st = present_state(page)
        check(
            not st["on"]
            and len(st["cells"]) == 11
            and page.locator(".nb-toolbar").is_visible()
            and page.locator("textarea:visible").count() > 3
            and not page.evaluate("App.reading")
            and st["hash"] == "",
            f"Escape leaves presenting for the editor as it was ({st})",
        )
        check(
            page.locator("#cells").evaluate("e => e.getAttribute('style') || ''") == ""
            and page.locator(".nb-present-mark").count() == 0,
            "nothing of the presentation is left on the page",
        )
        check(not errors, f"no browser errors presenting in the editor {errors[:3]}")
    finally:
        stop(shell)


def test_input_widget(browser, work):
    """B31: an input cell's widget is the same in the editor and in an export (NB.inputWidget)."""
    db = os.path.join(work, "widget.duckdb")
    out = os.path.join(work, "widget.html")
    sql(db, f"FROM notebook_export('{os.path.join(ROOT, 'examples', 'revenue.sql')}', file := '{out}')")
    page = browser.new_page()
    page.goto(still("file://" + out))
    page.wait_for_function("window.__NB_READY === true")
    differ = page.evaluate(
        """() => {
      const cells = [
        {name: 'n', widget: 'slider', value: 1000, options: {min: '0', max: '5000'}},
        {name: 'n', widget: 'number', value: null, options: {label: '', step: ''}},
        {name: 'n', widget: 'text', value: 'x', options: {label: 'Name', placeholder: ''}},
        {name: 'n', widget: 'date', value: '2026-01-02', options: {min: ''}},
      ];
      return cells.map(c => [NB.inputWidget(c, () => {}).outerHTML, NB.inputWidget(c, () => {}, {exported: true}).outerHTML])
                  .filter(([a, b]) => a !== b);
    }"""
    )
    check(not differ, f"an input looks the same in the editor and an export ({differ})")
    shown = page.evaluate(
        """() => [1000, 2026, 123.45].map(v =>
      NB.inputWidget({name: 'n', widget: 'slider', value: v, options: {}}, () => {}).querySelector('.nb-input-value').textContent)"""
    )
    check(
        shown == ["1000", "2026", "123.45"],
        f"a slider shows its exact value: a year is not 2,026, a fraction is not rounded ({shown})",
    )
    empty = page.evaluate(
        "NB.inputWidget({name: 'n', widget: 'number', value: null, options: {label: '', step: ''}}, () => {}).outerHTML"
    )
    check(
        "<label>n</label>" in empty and 'step="any"' in empty and "value=" not in empty,
        f"an empty label= or step= is not set, and an empty number has no value ({empty})",
    )
    page.close()


def test_chart_marks(browser, work):
    """Rules and bands on charts (FORMAT 5.1), in an export of test/notebooks/references.sql."""
    db = os.path.join(work, "marks.duckdb")
    out = os.path.join(work, "references.html")
    sql(db, f"FROM notebook_export('{os.path.join(ROOT, 'test', 'notebooks', 'references.sql')}', file := '{out}')")
    page = browser.new_page(viewport={"width": 1300, "height": 950})
    errors = watch(page)
    page.goto(still("file://" + out))
    page.wait_for_function("window.__NB_READY === true")
    chart = page.locator(".nb-cell[data-idx='1']")
    labels = chart.locator(".nb-ref-labels text").evaluate_all("els => els.map(e => e.textContent)")
    check(
        chart.locator(".nb-ref-rule").count() == 6 and chart.locator(".nb-ref-band").count() == 2,
        "a chart draws its rules and bands",
    )
    check(
        "average 49.5" in labels and "24.75 to 74.25" in labels and "budget 100" in labels,
        f"aggregates are computed by DuckDB and written into the labels ({labels})",
    )
    order = chart.locator("svg.nb-chart-svg > g").evaluate(
        "g => Array.from(g.children).map(e => e.getAttribute('class')).filter(c => /nb-refs|nb-plot|nb-ref-labels/.test(c || ''))"
    )
    check(
        order == ["nb-refs", "nb-plot", "nb-ref-labels"],
        f"rules and bands are drawn behind the marks, their labels over them ({order})",
    )
    muted = chart.locator(".nb-ref-rule path").first.evaluate("e => getComputedStyle(e).stroke")
    red = chart.locator(".nb-ref-rule").nth(5).locator("path").first.evaluate("e => getComputedStyle(e).stroke")
    check(
        muted == "rgb(134, 142, 150)" and red == "rgb(224, 49, 49)",
        f"they are in muted ink unless given a colour ({muted}, {red})",
    )
    # y: 120 is above every point (n < 100): the axis grows to show it.
    clear = chart.evaluate(
        """(cell) => {
      const line = cell.querySelector('.nb-plot .nb-line'), n = line.getTotalLength(), m = line.getScreenCTM(), pts = [];
      for (let i = 0; i <= 800; i++) { const p = line.getPointAtLength(n * i / 800).matrixTransform(m); pts.push([p.x, p.y]); }
      return Array.from(cell.querySelectorAll('.nb-ref-labels text')).filter(t => {
        const b = t.getBoundingClientRect();
        return pts.some(p => p[0] > b.left && p[0] < b.right && p[1] > b.top && p[1] < b.bottom);
      }).map(t => t.textContent);
    }"""
    )
    check(clear == [], f"rule and band labels are placed clear of the line ({clear})")
    target = chart.locator(".nb-ref-rule[data-ref='Target'] path").first.bounding_box()
    frame, line = chart.locator("svg.nb-chart-svg").bounding_box(), chart.locator(".nb-plot .nb-line").bounding_box()
    check(
        target is not None and frame["y"] <= target["y"] < line["y"],
        "the axis grows to include a rule outside the data",
    )
    chart.locator(".nb-tab", has_text="Bars by group").click()
    labels = chart.locator(".nb-ref-labels text").evaluate_all("els => els.map(e => e.textContent)")
    check(labels == ["Target", "average 2,475"], f"a rule's label uses the axis's format ({labels})")
    check(chart.locator(".nb-ref-rule").count() == 2, "a rule on a category axis runs through its category")

    # Notes (FORMAT 5.1): handwriting attached to data positions, drawn after the marks.
    boxes = """(cell) => {
      const box = e => { const b = e.getBoundingClientRect(); return [b.left, b.top, b.right, b.bottom]; };
      return { texts: Array.from(cell.querySelectorAll('.nb-note')).map(n => Array.from(n.querySelectorAll('text')).map(t => ({ text: t.textContent, box: box(t) }))),
               bars: Array.from(cell.querySelectorAll('.nb-plot .nb-mark .sk-hit')).map(box),
               paths: Array.from(cell.querySelectorAll('.nb-note')).map(n => n.querySelectorAll('path').length) }; }"""
    geo = chart.evaluate(boxes)
    note, bar = geo["texts"][0][0], max(geo["bars"], key=lambda b: b[3] - b[1])
    check(
        note["text"] == "the bigger group"
        and note["box"][3] <= bar[1] + 2
        and note["box"][0] < bar[2] + 60
        and note["box"][2] > bar[0] - 60,
        f"a note on a category sits above its bar ({note['box']} over {bar})",
    )
    chart.locator(".nb-tab", has_text="Notes").click()
    geo = chart.evaluate(boxes)
    lines = [t["text"] for t in geo["texts"][0]]
    check(
        len(geo["texts"]) == 3
        and len(lines) >= 2
        and all(len(l) <= 18 for l in lines)
        and " ".join(lines) == "A note that is long enough to wrap",
        f"a long note is wrapped into short lines ({lines})",
    )
    order = chart.locator("svg.nb-chart-svg > g").evaluate(
        "g => Array.from(g.children).map(e => e.getAttribute('class')).filter(c => /nb-plot|nb-notes/.test(c || ''))"
    )
    check(
        order == ["nb-plot", "nb-notes"]
        and chart.locator(".nb-notes").evaluate("e => getComputedStyle(e).pointerEvents") == "none",
        "notes are drawn after the marks and leave the pointer to them",
    )
    # The ring of the first note is on the line at its date; no note's text touches the line.
    hits = chart.evaluate(
        """(cell) => {
      const line = cell.querySelector('.nb-plot .nb-line'), n = line.getTotalLength(), m = line.getScreenCTM(), pts = [];
      for (let i = 0; i <= 400; i++) { const p = line.getPointAtLength(n * i / 800).matrixTransform(m); pts.push([p.x, p.y]); }
      const ring = cell.querySelector('.nb-note path').getBoundingClientRect(), cx = (ring.left + ring.right) / 2, cy = (ring.top + ring.bottom) / 2;
      const near = Math.min(...pts.map(p => Math.hypot(p[0] - cx, p[1] - cy)));
      const texts = Array.from(cell.querySelectorAll('.nb-note text')).map(t => t.getBoundingClientRect());
      const touching = texts.filter(b => pts.some(p => p[0] > b.left && p[0] < b.right && p[1] > b.top && p[1] < b.bottom)).length;
      return { near, touching, width: ring.width };
    }"""
    )
    check(hits["near"] < 4 and 18 < hits["width"] < 34, f"a circled note rings its data point ({hits})")
    check(hits["touching"] == 0, "notes are placed clear of the line they point at")
    check(
        geo["paths"][0] >= 2 and geo["paths"][1] >= 1 and geo["paths"][2] == 3,
        f"a note has an arrow, a range has a bracket ({geo['paths']})",
    )
    bad = page.locator(".nb-cell[data-idx='2']")
    check(
        "rules: avg needs a numeric or date x axis" in bad.text_content(), "a bad rule is reported as the chart's error"
    )
    # The pen writes a chart in document order: rules and bands, then the marks, then the notes.
    pen = browser.new_page(viewport={"width": 1300, "height": 950})
    pen.add_init_script("window.__INK_PAUSE = true")
    opened(pen, "file://" + out)
    order = """() => {
      const out = { refs: [], plot: [], notes: [] };
      Ink.jobs().forEach(j => (j.atoms || []).forEach(a => {
        const el = a.el || (a.unit && a.unit.el) || (a.nodes && a.nodes[0]);
        if (!el || !el.closest || !el.isConnected || !el.closest('.nb-cell[data-idx="1"] svg.nb-chart-svg')) return;
        const part = el.closest('.nb-notes') ? 'notes' : el.closest('.nb-plot') ? 'plot' : el.closest('.nb-refs') ? 'refs' : null;
        if (part) out[part].push([a.t0, a.order]);
      }));
      const span = list => list.length ? [Math.min(...list.map(a => a[0])), Math.max(...list.map(a => a[0])), Math.min(...list.map(a => a[1])), Math.max(...list.map(a => a[1]))] : null;
      return { refs: span(out.refs), plot: span(out.plot), notes: span(out.notes) };
    }"""
    starts = pen.evaluate(order)
    check(
        starts["refs"]
        and starts["plot"]
        and starts["refs"][1] <= starts["plot"][0]
        and starts["refs"][3] < starts["plot"][2],
        f"the pen draws rules and bands before the marks ({starts})",
    )
    pen.locator(".nb-cell[data-idx='1'] .nb-tab", has_text="Notes").click()
    frames(pen, 3)
    starts = pen.evaluate(order)
    # What follows a click is drawn almost at once, but still in order.
    check(
        starts["notes"]
        and starts["plot"]
        and starts["plot"][0] <= starts["notes"][0]
        and starts["plot"][3] < starts["notes"][2],
        f"the pen writes notes after the marks ({starts})",
    )
    pen.close()
    check(not errors, f"no browser errors drawing rules and bands {errors[:3]}")
    page.close()


def test_peek(browser, work):
    # Peek under the pen: with the caret in a CTE or sub-query of a query cell, its first rows show in a
    # note beside the code. Nothing is asked before the notebook has run, and nothing else changes.
    notebook = os.path.join(work, "peek.sql")
    shutil.copy(os.path.join(ROOT, "test", "notebooks", "peek.sql"), notebook)
    shell, url = serve(
        work, notebook, setup="CREATE SEQUENCE nb_peek_seq; CREATE MACRO nb_peek_sneaky() AS nextval('nb_peek_seq');"
    )

    def caret(page, cell, needle, skip=0):
        """Puts the caret just before `needle` (plus `skip`) in a cell's SQL, with a real key press."""
        area = page.locator(f".nb-cell[data-idx='{cell}'] textarea.nb-code-over").first
        area.scroll_into_view_if_needed()
        area.click()
        area.evaluate(
            "(e, a) => { const i = e.value.indexOf(a[0]) + a[1] - 1; e.setSelectionRange(i, i); }", [needle, skip]
        )
        page.keyboard.press("ArrowRight")
        return area

    def note_text(page):
        return page.locator(".nb-peek").inner_text().replace("\t", " ").split("\n")

    def peek(page, statement, needle):
        return page.evaluate(
            "([s, n]) => App.api('POST', '/api/peek', {sql: s, cursor: s.indexOf(n), rows: 5})", [statement, needle]
        )

    def gone(page):
        page.wait_for_selector(".nb-peek", state="detached", timeout=5000)

    try:
        page = browser.new_page(viewport={"width": 1300, "height": 900})
        errors = watch(page)
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.wait_for_function("document.fonts.status === 'loaded'")

        # Trust: opening a notebook does not run it, so there is nothing to peek at yet.
        caret(page, 3, "amount >= $floor")
        page.wait_for_timeout(900)
        refused = peek(page, "SELECT * FROM (SELECT 1 AS a) q", "SELECT 1")
        check(
            page.locator(".nb-peek").count() == 0 and refused.get("reason") == "nothing has been run yet",
            f"nothing is peeked at in a notebook that has not been run ({refused})",
        )

        page.click("#run-all")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        settle(page)
        # Positions relative to the first cell: clicking into a cell may scroll the page, which is not a layout change.
        boxes = "() => { const all = [...document.querySelectorAll('.nb-cell')], top = all[0].getBoundingClientRect().y; return all.map(c => { const r = c.getBoundingClientRect(); return [r.x, r.y - top, r.width, r.height].map(Math.round).join(','); }).join(';'); }"
        layout_before = page.evaluate(boxes)

        # A CTE: its name, a few rows and columns, and how many rows there are.
        area = caret(page, 3, "amount >= $floor")
        page.wait_for_selector(".nb-peek", timeout=5000)
        lines = note_text(page)
        check(
            lines[0] == "big"
            and lines[1] == "cte"
            and lines[2] == "id region day amount"
            and lines[3] == "2 east 2026-01-03 84"
            and len(lines) == 9
            and lines[8] == "first 5 of 320 rows",
            f"with the caret in a CTE, a note shows its name, its first rows and its row count ({lines})",
        )
        marks = page.locator(".nb-cell[data-idx='3'] .nb-peek-marks i").count()
        check(marks == 3, f"the part the note describes is marked in the code, line by line ({marks} lines)")
        # The note is fixed to the window and beside the code: no line of code is under it, nothing moved.
        overlap = page.evaluate(
            """() => {
          const n = document.querySelector('.nb-peek').getBoundingClientRect();
          const pre = document.querySelector(".nb-cell[data-idx='3'] .nb-code-hl"), range = document.createRange();
          range.selectNodeContents(pre);
          return [...range.getClientRects()].filter(r => r.width > 0 && r.right > n.left && r.left < n.right && r.bottom > n.top && r.top < n.bottom).length;
        }"""
        )
        inside = page.evaluate(
            "() => { const n = document.querySelector('.nb-peek').getBoundingClientRect(); return n.left >= 0 && n.top >= 0 && n.right <= innerWidth && n.bottom <= innerHeight; }"
        )
        layout_after = page.evaluate(boxes)
        check(
            overlap == 0
            and inside
            and page.evaluate("getComputedStyle(document.querySelector('.nb-peek')).position") == "fixed"
            and layout_after == layout_before
            and page.evaluate("document.querySelector('.nb-peek').parentNode === document.body"),
            f"the note covers no code, fits the window and takes no part in the layout ({overlap} lines under it, inside {inside}, "
            f"layout {'kept' if layout_after == layout_before else layout_before + ' -> ' + layout_after})",
        )
        check(
            page.locator(".nb-cell[data-idx='3'] .nb-badge.fresh").count() == 1
            and page.locator(".nb-cell[data-idx='3'] table.nb-grid tbody tr").count() == 4,
            "the cell's own result and state are untouched by a peek",
        )

        # A sub-query in FROM is named by its alias; one in an expression has no name.
        caret(page, 3, "sum(total)")
        page.wait_for_function("(document.querySelector('.nb-peek-name') || {}).textContent === 's'", timeout=5000)
        lines = note_text(page)
        check(
            lines == ["s", "sub-query", "everything", "41,440", "1 row"],
            f"a sub-query in FROM shows under its alias ({lines})",
        )
        caret(page, 4, "share > 0.2")
        page.wait_for_function(
            "(document.querySelector('.nb-peek-name') || {}).textContent === 'sub-query'", timeout=5000
        )
        lines = note_text(page)
        check(
            lines[:2] == ["sub-query", "region"]
            and sorted(lines[2:6]) == ["east", "north", "south", "west"]
            and lines[6] == "4 rows",
            f"a sub-query in an expression is peeked at too, reading the cells above ({lines})",
        )

        # The main query is not a part: the note goes away when the caret moves on.
        caret(page, 3, "ORDER BY t.region")
        gone(page)
        check(
            page.locator(".nb-peek-marks").count() == 0,
            "moving the caret into the main query takes the note and its marks away",
        )

        # Typing: the peek follows the text as typed (and the session's variables), the result does not.
        area = caret(page, 3, "$floor", len("$floor"))
        page.wait_for_selector(".nb-peek", timeout=5000)
        area.type(" + 100", delay=15)
        page.wait_for_function(
            "(document.querySelector('.nb-peek-foot') || {}).textContent === 'first 5 of 120 rows'", timeout=5000
        )
        check(
            note_text(page)[3] == "4 north 2026-01-05 158"
            and page.locator(".nb-cell[data-idx='3'] table.nb-grid tbody tr td").nth(2).inner_text() == "80",
            "typing in a CTE updates its peek, not the cell's result",
        )
        # SQL that does not parse shows nothing; mended, the note returns.
        area.type(" (", delay=15)
        gone(page)
        page.keyboard.press("Backspace")
        page.keyboard.press("Backspace")
        page.wait_for_selector(".nb-peek", timeout=5000)
        check(note_text(page)[0] == "big", "nothing shows while the SQL does not parse; the note returns when it does")

        # Escape puts the note away until the caret moves to another part.
        page.keyboard.press("Escape")
        gone(page)
        page.keyboard.press("ArrowLeft")
        page.wait_for_timeout(900)
        still_gone = page.locator(".nb-peek").count() == 0
        caret(page, 3, "count(*)")
        page.wait_for_function("(document.querySelector('.nb-peek-name') || {}).textContent === 'totals'", timeout=5000)
        check(still_gone, "Escape dismisses the note for that part; another part shows its own")
        # Leaving the cell takes it away.
        page.locator("#title").click()
        gone(page)
        check(True, "the note goes when the caret leaves the cell")

        # Safety, through the API the editor uses: statements that change data, parts with side
        # effects, anything that would write, and anything slow.
        refusals = [
            peek(
                page,
                "INSERT INTO nb_peek_sales SELECT * FROM (SELECT 1000, 'north', DATE '2026-01-01', 1) q",
                "SELECT 1000",
            ),
            peek(
                page,
                "WITH gone AS (DELETE FROM nb_peek_sales RETURNING id) SELECT * FROM (SELECT 1 AS a) q",
                "SELECT 1",
            ),
            peek(page, "SELECT * FROM (SELECT nextval('nb_peek_seq') AS n) q", "nextval"),
            peek(page, "SELECT * FROM (SELECT nb_peek_sneaky() AS n) q", "nb_peek_sneaky"),
        ]
        reasons = [r.get("reason", "") for r in refusals]
        check(
            "not a query" in reasons[0]
            and "changes data" in reasons[1]
            and "side effects" in reasons[2]
            and "read-only" in reasons[3]
            and not any("rows" in r for r in refusals),
            f"statements that change data, side effects and writes are never peeked at ({reasons})",
        )
        slow = page.evaluate(
            """async () => { const t = performance.now();
          const s = 'SELECT * FROM (SELECT count(*) AS n FROM range(100000000000) a, range(1000) b WHERE a.range % 7 = b.range) q';
          const r = await App.api('POST', '/api/peek', {sql: s, cursor: s.indexOf('count'), rows: 5}); return [performance.now() - t, r.reason || '']; }"""
        )
        check(
            slow[0] < 4000 and "execution time" in slow[1],
            f"a peek that takes too long is given up ({round(slow[0])} ms: {slow[1][:60]})",
        )
        check(
            page.evaluate("App.api('GET', '/api/page?cell=3&offset=0&limit=5').then(r => r.rows.length)") == 4,
            "the session's views and variables are as they were after peeking",
        )
        shell.stdin.write("SELECT 'left: ' || count(*) FROM nb_peek_sales; SELECT 'seq: ' || nextval('nb_peek_seq');\n")
        shell.stdin.flush()
        left = [shell.stdout.readline().strip(), shell.stdout.readline().strip()]
        check(left == ["left: 400", "seq: 1"], f"nothing was inserted, deleted or advanced by peeking ({left})")

        # The gear's switch turns it off, remembered in this browser.
        page.click("#gear")
        page.uncheck("#peek")
        page.keyboard.press("Escape")
        caret(page, 3, "count(*)")
        page.wait_for_timeout(900)
        check(
            page.locator(".nb-peek").count() == 0 and page.evaluate("localStorage.getItem('nb.peek')") == "off",
            "the gear menu's peek switch turns it off",
        )
        page.reload()
        page.wait_for_selector(".nb-cell")
        check(not page.locator("#peek").is_checked(), "the switch is remembered after a reload")
        page.click("#gear")
        page.check("#peek")
        page.keyboard.press("Escape")
        page.click("#run-all")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        caret(page, 3, "count(*)")
        page.wait_for_selector(".nb-peek", timeout=5000)
        check(note_text(page)[0] == "totals", "switched on again, it peeks again")
        reference_note = html_of(page, ".nb-peek")
        # Presenting shows the notebook a screen at a time: no note there, and none after leaving it until asked.
        page.evaluate("Present.enter(0)")
        page.wait_for_function("document.body.classList.contains('presenting')", timeout=5000)
        page.wait_for_timeout(700)
        presenting_notes = page.locator(".nb-peek:visible").count()
        page.evaluate("Present.leave()")
        check(presenting_notes == 0, "no peek note shows while presenting")

        # A narrow window has no room beside the code for a wide note: it goes under it, still over no code.
        narrow = browser.new_page(viewport={"width": 600, "height": 800})
        narrow.goto(still(url))
        narrow.wait_for_selector(".nb-cell")
        narrow.click("#run-all")
        narrow.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        caret(narrow, 3, "amount >= $floor")
        narrow.wait_for_selector(".nb-peek", timeout=5000)
        fits = narrow.evaluate(
            """() => {
          const n = document.querySelector('.nb-peek').getBoundingClientRect();
          const pre = document.querySelector(".nb-cell[data-idx='3'] .nb-code-hl"), range = document.createRange();
          range.selectNodeContents(pre);
          const under = [...range.getClientRects()].filter(r => r.width > 0 && r.right > n.left && r.left < n.right && r.bottom > n.top && r.top < n.bottom).length;
          return [under, n.left >= 0 && n.right <= innerWidth, document.documentElement.scrollWidth <= innerWidth];
        }"""
        )
        check(
            fits == [0, True, True], f"in a narrow window the note still covers no code and stays on the page ({fits})"
        )
        narrow.close()

        # Written by the pen, it leaves no trace: the finished note is the note drawn with ink off.
        inked = browser.new_page(viewport={"width": 1300, "height": 900})
        inked_errors = watch(inked)
        inked.goto(url)
        inked.wait_for_selector(".nb-cell")
        inked.click("#run-all")
        inked.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        inked.wait_for_function("Ink.jobs().every(j => j.state === 'parked')", timeout=20000)
        caret(inked, 3, "count(*)")
        inked.wait_for_selector(".nb-peek", timeout=5000)
        started = inked.evaluate("performance.now()")
        inked.wait_for_function(
            "!Ink.jobs().some(j => j.key === 'peek') && !document.querySelector('.nb-peek canvas.ink-layer, .nb-peek .ink-hide')",
            timeout=10000,
        )
        took = inked.evaluate("performance.now()") - started
        state = inked.evaluate(
            "() => ({ canvases: document.querySelectorAll('.nb-peek canvas.ink-layer').length, hidden: document.querySelectorAll('.nb-peek .ink-hide, .nb-peek.ink-hide').length, wrote: Ink.plans ? Ink.plans.some(p => p.key === 'peek') : null })"
        )
        check(
            state["canvases"] == 0
            and state["hidden"] == 0
            and html_of(inked, ".nb-peek") == reference_note
            and took < 1500,
            f"the pen writes the note in {round(took)} ms and leaves it exactly as drawn without the animation ({state})",
        )
        check(not inked_errors, f"no browser errors while the note is written {inked_errors[:3]}")
        inked.close()

        # Trust again: text that arrives from the file on disk has not been run by the reader.
        with open(notebook, "a") as f:
            f.write("\n-- %% added\nSELECT * FROM (SELECT 42 AS answer) q;\n")
        page.wait_for_function("document.querySelectorAll('.nb-cell').length === 6", timeout=15000)
        caret(page, 5, "42")
        page.wait_for_timeout(900)
        refused = peek(page, "SELECT * FROM (SELECT 1 AS a) q", "SELECT 1")
        check(
            page.locator(".nb-peek").count() == 0 and refused.get("reason") == "nothing has been run yet",
            "a notebook reloaded from disk is not peeked at until it is run again",
        )
        check(not errors, f"no browser errors while peeking {errors[:3]}")
    finally:
        stop(shell)


def test_peek_cached(browser, work):
    # The result cache shows a notebook's last results when it is opened again. That is not a run: until
    # the reader runs something in this server, nothing is peeked at (DESIGN section 3).
    notebook = os.path.join(work, "peek_cached.sql")
    shutil.copy(os.path.join(ROOT, "test", "notebooks", "peek.sql"), notebook)
    db = os.path.join(work, "peek_cached.duckdb")

    def start():
        return serve(work, notebook, db)

    probe = "() => App.api('POST', '/api/peek', {sql: 'SELECT * FROM (SELECT 1 AS a) q', cursor: 20, rows: 5})"
    shell, url = start()
    try:
        page = browser.new_page(viewport={"width": 1300, "height": 900})
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.click("#run-all")
        page.wait_for_function("document.getElementById('status').textContent.startsWith('Ran')", timeout=30000)
        check(page.evaluate(probe).get("kind") == "subquery", "after a run, peeks are answered")
        page.close()
    finally:
        stop(shell)
    shell, url = start()
    try:
        page = browser.new_page(viewport={"width": 1300, "height": 900})
        errors = watch(page)
        page.goto(still(url))
        page.wait_for_function(
            "document.querySelectorAll(\".nb-cell[data-idx='3'] table.nb-grid tbody tr\").length === 4", timeout=10000
        )
        peeks = []
        page.on("request", lambda r: peeks.append(r.url) if "/api/peek" in r.url else None)
        area = page.locator(".nb-cell[data-idx='3'] textarea.nb-code-over")
        area.click()
        area.evaluate("e => { const i = e.value.indexOf('count(*)') - 1; e.setSelectionRange(i, i); }")
        page.keyboard.press("ArrowRight")
        page.wait_for_timeout(900)
        check(not peeks, f"the editor does not ask for peeks the server would refuse ({len(peeks)} sent)")
        refused = page.evaluate(probe)
        check(
            page.locator(".nb-peek").count() == 0 and refused.get("reason") == "nothing has been run yet",
            f"a notebook reopened with its cached results is not peeked at until it is run ({refused})",
        )
        check(not errors, f"no browser errors reopening a cached notebook {errors[:3]}")
        page.close()
    finally:
        stop(shell)


WIDE_NOTEBOOK = """-- %% [markdown]
/*
# Wide

A paragraph long enough to show where prose stops when the page is wide: it keeps a line length a
reader can follow from one end to the other, however wide the window is, while the tables and charts
below it take all the room they are given. It goes on a little longer to be sure it wraps.
*/

-- %% series
SELECT range AS day, (range * 37) % 101 + range AS value, 'a' || (range % 3) AS grp FROM range(60)
/* chart
mark: line
x: day
y: value
*/

-- %% chart fixed from=series width=500
/*
mark: bar
x: {field: grp, type: nominal}
y: {field: value, aggregate: sum}
*/

-- %% wide_table
SELECT range AS a, range * 2 AS b, range * 3 AS c, range * 4 AS d, range * 5 AS e, range * 6 AS f,
       range * 7 AS g, range * 8 AS h, range * 9 AS i, range * 10 AS j, range * 11 AS k, range * 12 AS l,
       'a longer text value ' || range AS m, 'another longer text value ' || range AS n, range * 13 AS o
FROM range(5);
"""

WIDE_STATE = """() => {
    const svg = sel => { const e = document.querySelector(sel); if (!e) return null;
      const b = e.getBoundingClientRect(); return { view: +e.getAttribute('viewBox').split(' ')[2], w: Math.round(b.width), h: Math.round(b.height),
        ticks: e.querySelectorAll('.nb-axis text').length,
        overhang: Math.round(Math.max(...[...e.querySelectorAll('.nb-axis text')].map(t => t.getBoundingClientRect().right)) - e.closest('.nb-output').getBoundingClientRect().right) }; };
    const cells = document.querySelector('.nb-cells').getBoundingClientRect();
    const p = document.querySelector('.nb-md p');
    return { mode: document.documentElement.getAttribute('data-width'), cells: Math.round(cells.width),
      fluid: svg('.nb-cell[data-idx="1"] svg.nb-chart-svg'), fixed: svg('.nb-cell[data-idx="2"] svg.nb-chart-svg'),
      prose: p ? Math.round(p.getBoundingClientRect().width) : 0,
      columns: document.querySelectorAll('.nb-cell[data-idx="3"] table.nb-grid thead th:not(.nb-col-off)').length,
      scroll: document.documentElement.scrollWidth - innerWidth };
}"""


def test_wide(browser, work):
    db = os.path.join(work, "wide.duckdb")
    notebook, declared = os.path.join(work, "wide.sql"), os.path.join(work, "wide_declared.sql")
    with open(notebook, "w") as f:
        f.write(WIDE_NOTEBOOK)
    with open(declared, "w") as f:
        f.write("-- %% [notebook]\n/*\ntitle: Wide by default\nwidth: wide\n*/\n\n" + WIDE_NOTEBOOK)
    html, html_declared = os.path.join(work, "wide.html"), os.path.join(work, "wide_declared.html")
    sql(
        db,
        f"FROM notebook_export('{notebook}', file := '{html}')",
        f"FROM notebook_export('{declared}', file := '{html_declared}')",
    )
    url = "file://" + html
    viewport = {"width": 1700, "height": 1000}

    page = browser.new_page(viewport=viewport)
    errors = watch(page)
    opened(page, still(url))
    settle(page)
    normal, normal_html = page.evaluate(WIDE_STATE), html_of(page, "#nb-root")
    check(
        normal["mode"] == "normal" and normal["cells"] <= 1000 and normal["fluid"]["view"] == 640,
        f"an export opens in its reading column, its charts at their compiled width ({normal})",
    )

    toggle = page.locator("button.nb-width-toggle")
    check(
        toggle.is_visible()
        and toggle.evaluate("e => getComputedStyle(e).borderTopWidth") == "0px"
        and "on" not in toggle.get_attribute("class"),
        "an export has a quiet wide control",
    )
    toggle.click()
    settle(page)
    wide = page.evaluate(WIDE_STATE)
    check(
        wide["mode"] == "wide" and wide["cells"] > 1550 and wide["scroll"] <= 0,
        f"wide: the cells use the window's width, less a gutter ({wide['cells']}px of 1700)",
    )
    check(
        wide["fluid"]["view"] > 1400
        and wide["fluid"]["w"] > 1400
        and abs(wide["fluid"]["h"] - normal["fluid"]["h"]) <= 2
        and wide["fluid"]["ticks"] > normal["fluid"]["ticks"],
        f"a chart is drawn again for the wider box: same height, more ticks, not stretched ({normal['fluid']} -> {wide['fluid']})",
    )
    check(wide["fluid"]["overhang"] <= 0, f"its last tick label stays inside its box ({wide['fluid']['overhang']}px)")
    check(
        wide["fixed"]["view"] == 500 and wide["fixed"]["w"] == 500,
        f"a chart whose cell sets width= keeps it ({wide['fixed']})",
    )
    check(
        wide["prose"] <= 760 and wide["columns"] > normal["columns"],
        f"prose keeps its line length while a table shows more columns ({wide['prose']}px; {normal['columns']} -> {wide['columns']} columns)",
    )
    check(
        "on" in toggle.get_attribute("class") and page.evaluate("localStorage.getItem('nb.width')") == "wide",
        "the control shows the choice and remembers it in this browser",
    )

    page.reload()
    page.wait_for_function("window.__NB_READY === true")
    settle(page)
    check(page.evaluate(WIDE_STATE)["mode"] == "wide", "the choice holds when the page is opened again")
    opened(page, still(url) + "&width=normal")
    check(page.evaluate(WIDE_STATE)["mode"] == "normal", "width=normal in the address wins over the saved choice")

    # Back to normal: nothing of the wide page is left, and the page is as it was first drawn.
    opened(page, still(url))
    settle(page)
    page.locator("button.nb-width-toggle").click()
    settle(page)
    check(
        page.evaluate(WIDE_STATE)["mode"] == "normal" and html_of(page, "#nb-root") == normal_html,
        "switching back draws the page exactly as a normal page",
    )

    # Print and PDF keep the normal width; the page comes back wide afterwards.
    page.locator("button.nb-width-toggle").click()
    settle(page)
    page.evaluate("window.dispatchEvent(new Event('beforeprint'))")
    printing = page.evaluate(WIDE_STATE)
    page.evaluate("window.dispatchEvent(new Event('afterprint'))")
    after = page.evaluate(WIDE_STATE)
    check(
        printing["mode"] == "normal"
        and printing["fluid"]["view"] == 640
        and after["mode"] == "wide"
        and after["fluid"]["view"] > 1400,
        f"print gets the normal page, and the wide page comes back after ({printing['mode']}, {after['mode']})",
    )
    page.evaluate("document.documentElement.setAttribute('data-width-fixed', 'normal')")
    page.evaluate("window.dispatchEvent(new Event('resize'))")
    check(
        page.evaluate("NB.width.mode()") == "normal",
        "a page fixed at the normal width (the PDF export's) ignores the reader's choice",
    )
    page.evaluate("document.documentElement.removeAttribute('data-width-fixed')")

    # A resized wide window lays its charts out again.
    page.set_viewport_size({"width": 1300, "height": 1000})
    page.wait_for_timeout(500)
    settle(page)
    resized = page.evaluate(WIDE_STATE)
    check(
        resized["mode"] == "wide" and 1000 < resized["fluid"]["view"] < 1300 and resized["scroll"] <= 0,
        f"resizing a wide window draws the charts again at the new width ({resized['fluid']})",
    )

    # Presenting is unaffected: the same screens at the same scale.
    page.set_viewport_size(viewport)
    page.wait_for_timeout(500)
    page.evaluate("Present.enter(1)")
    settle(page)
    presented_wide = page.evaluate(
        "() => ({ mode: document.documentElement.getAttribute('data-width'), scale: Present.scale, view: +document.querySelector('svg.nb-chart-svg').getAttribute('viewBox').split(' ')[2], width: document.querySelector('.nb-cells').offsetWidth })"
    )
    page.keyboard.press("Escape")
    settle(page)
    left = page.evaluate(WIDE_STATE)
    page.evaluate("NB.width.set('normal')")
    page.evaluate("Present.enter(1)")
    settle(page)
    presented_normal = page.evaluate(
        "() => ({ mode: document.documentElement.getAttribute('data-width'), scale: Present.scale, view: +document.querySelector('svg.nb-chart-svg').getAttribute('viewBox').split(' ')[2], width: document.querySelector('.nb-cells').offsetWidth })"
    )
    page.keyboard.press("Escape")
    check(
        presented_wide == presented_normal and left["mode"] == "wide",
        f"presenting a wide page shows the same screen at the same scale as a normal one, and leaving it is wide again ({presented_wide})",
    )
    check(not errors, f"no browser errors switching width {errors[:3]}")
    page.close()

    # The author's default: width: wide in the [notebook] cell, for readers who have not chosen.
    page = browser.new_page(viewport=viewport)
    opened(page, still("file://" + html_declared))
    check(page.evaluate(WIDE_STATE)["mode"] == "wide", "width: wide in the [notebook] cell opens the export wide")
    opened(page, still("file://" + html_declared) + "&width=normal")
    check(page.evaluate(WIDE_STATE)["mode"] == "normal", "the address still wins over the notebook's default")
    page.evaluate("localStorage.setItem('nb.width', 'normal')")
    opened(page, still("file://" + html_declared))
    check(page.evaluate(WIDE_STATE)["mode"] == "normal", "a reader's own choice wins over the notebook's default")
    page.close()

    # The pen: a wide page, written, ends identical to the same page drawn with ink=off; so does one
    # switched to wide while it was being written.
    off = browser.new_page(viewport=viewport)
    opened(off, still(url) + "&width=wide")
    settle(off)
    reference = html_of(off, "#nb-root")
    off.close()
    for switched in (False, True):
        page = browser.new_page(viewport=viewport)
        errors = watch(page)
        page.add_init_script("window.__INK_PAUSE = true")
        opened(page, url + ("" if switched else "?width=wide"))
        page.evaluate("Ink.advance(400)")
        if switched:
            page.locator("button.nb-width-toggle").click()
            page.evaluate("Ink.advance(300)")
        page.evaluate("Ink.advance(60000); Ink.finishAll()")
        settle(page)
        state = page.evaluate(INK_STATE)
        check(
            state["canvases"] == 0
            and state["hidden"] == 0
            and state["styled"] == 0
            and html_of(page, "#nb-root") == reference,
            f"written {'after switching to wide mid-way' if switched else 'wide'}, the page is identical to it drawn with ink=off ({state})",
        )
        check(not errors, f"no browser errors writing a wide page {errors[:3]}")
        page.close()

    # Watch mode: the same pen, slowly; it ends on the same page.
    page = browser.new_page(viewport=viewport)
    page.add_init_script("window.__INK_PAUSE = true")
    opened(page, url + "?ink=watch&width=wide")
    advance(page, 4000)
    page.evaluate("Ink.finishAll()")
    settle(page)
    check(html_of(page, "#nb-root") == reference, "watched, a wide page ends identical to it drawn with ink=off")
    page.close()

    # A phone: wide changes nothing a narrow window can show.
    phone = browser.new_page(viewport={"width": 390, "height": 844})
    opened(phone, still(url) + "&width=wide")
    settle(phone)
    st = phone.evaluate(WIDE_STATE)
    check(
        st["mode"] == "wide" and st["scroll"] <= 0 and st["fluid"]["view"] == 640 and st["fluid"]["w"] < 390,
        f"on a phone a wide page does not scroll sideways and its charts shrink as before ({st})",
    )
    phone.close()


def test_wide_editor(browser, work):
    notebook = os.path.join(work, "wide_editor.sql")
    with open(notebook, "w") as f:
        f.write(WIDE_NOTEBOOK)
    shell = subprocess.Popen(
        [DUCKDB, "-init", "/dev/null", "-noheader", "-list"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        cwd=work,
        bufsize=1,
    )
    shell.stdin.write(f"SELECT url FROM notebook_start('{notebook}', open := false);\n")
    shell.stdin.flush()
    url = shell.stdout.readline().strip()
    try:
        page = browser.new_page(viewport={"width": 1700, "height": 1000})
        errors = watch(page)
        page.goto(still(url))
        page.wait_for_selector(".nb-cell")
        page.click("#run-all")
        page.wait_for_selector(".nb-cell[data-idx='1'] svg.nb-chart-svg")
        settle(page)
        normal = page.evaluate(WIDE_STATE)
        check(
            normal["mode"] == "normal" and normal["cells"] <= 1000 and normal["fluid"]["view"] == 640,
            f"the editor opens with its cells in the reading column ({normal['cells']}px)",
        )
        page.click("#gear")
        check(
            page.locator("#wide").is_visible() and not page.locator("#wide").is_checked(),
            "the gear menu has a wide switch, off",
        )
        page.locator("#wide").check()
        settle(page)
        wide = page.evaluate(WIDE_STATE)
        check(
            wide["mode"] == "wide"
            and wide["cells"] > 1300
            and wide["fluid"]["view"] > 1200
            and wide["fixed"]["view"] == 500
            and abs(wide["fluid"]["h"] - normal["fluid"]["h"]) <= 2
            and wide["scroll"] <= 0,
            f"wide: the editor's cells and charts use the window's width ({wide['cells']}px, chart {wide['fluid']})",
        )
        check(page.locator(".nb-margin").first.bounding_box()["x"] >= 0, "the cells' margins still have room")

        page.reload()
        page.wait_for_selector(".nb-cell[data-idx='1'] svg.nb-chart-svg")
        settle(page)
        page.click("#gear")
        check(
            page.evaluate(WIDE_STATE)["mode"] == "wide" and page.locator("#wide").is_checked(),
            "the editor remembers wide in this browser, and the switch shows it",
        )
        page.keyboard.press("Escape")

        # Reading view and presenting.
        page.goto(still(url) + "&width=wide#present=1")
        page.wait_for_function(
            "(document.querySelector('.nb-present-page') || {}).textContent.startsWith('1 /')", timeout=30000
        )
        settle(page)
        wide_screen = page.evaluate(
            "() => ({ mode: document.documentElement.getAttribute('data-width'), scale: Present.scale, width: document.querySelector('.nb-cells').offsetWidth })"
        )
        page.goto(still(url) + "&width=normal#present=1")
        page.wait_for_function(
            "(document.querySelector('.nb-present-page') || {}).textContent.startsWith('1 /')", timeout=30000
        )
        settle(page)
        normal_screen = page.evaluate(
            "() => ({ mode: document.documentElement.getAttribute('data-width'), scale: Present.scale, width: document.querySelector('.nb-cells').offsetWidth })"
        )
        check(
            wide_screen == normal_screen and wide_screen["mode"] == "normal",
            f"presenting from the editor is the same wide or not ({wide_screen} vs {normal_screen})",
        )
        check(not errors, f"no browser errors with a wide editor {errors[:3]}")
    finally:
        shell.stdin.write("SELECT stopped FROM notebook_stop();\n.quit\n")
        shell.stdin.flush()
        shell.communicate(timeout=30)


def main():
    work = tempfile.mkdtemp(prefix="notebook_browser_test_")
    with sync_playwright() as p:
        browser = p.chromium.launch()
        try:
            test_server(browser, work)
            test_metadata_error(browser, work)
            test_metadata_first(browser, work)
            test_cache(browser, work)
            test_cache_written_later(browser, work)
            test_cache_redraw_stale(browser, work)
            test_cache_variables_reloaded(browser, work)
            test_session_store(browser, work)
            test_session_keys(browser, work)
            test_input_widget(browser, work)
            test_brush_listeners(browser, work)
            # The headless shell prints PDFs too and needs fewer system libraries than full Chrome.
            shells = glob.glob(
                os.path.expanduser(
                    os.environ.get("PLAYWRIGHT_BROWSERS_PATH", "~/.cache/ms-playwright")
                    + "/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell"
                )
            )
            chromium = shells[0] if shells else p.chromium.executable_path
            test_exports(browser, work, chromium)
            test_chart_marks(browser, work)
            test_ink(browser, work, chromium)
            test_ink_editor(browser, work)
            test_ink_watch(browser, work)
            test_present(browser, work)
            test_present_editor(browser, work)
            test_peek(browser, work)
            test_peek_cached(browser, work)
            test_wide(browser, work)
            test_wide_editor(browser, work)
        finally:
            browser.close()
            for shell in list(running):
                stop(shell)
    shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} failure(s)" if failures else "browser tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
