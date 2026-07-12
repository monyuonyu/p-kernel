#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# consent_dom_cert.py — G3 headless-DOM cert: the ark-profile consent card
#                        must survive the first-run onboarding's DOM move.
#
# Regression this catches (fixed by commit 90feb043, PARENT = broken):
#   The consent card (.card, containing #arkh/#arktext/#arkfields/...) is ONE
#   DOM node that lives in TWO containers: the standalone #ark overlay
#   (teach-403 fallback / manifesto-updated re-consent) and, on first run,
#   the intro's #intromount — mountArkInIntro() (galaxy.html) physically
#   MOVES the node there. Before 90feb043 the card's rules were scoped
#   '#ark .card', '#ark pre', '#ark .opt', etc. The instant the card entered
#   #intromount every one of those rules died (wrong ancestor) and the card
#   fell back to pure UA defaults: a UA <pre>{white-space:pre} does not wrap,
#   so the manifesto's longest unbroken line became the card's min-content
#   width; as an unconstrained flex child it grew past a phone viewport and
#   the whole consent page bled off both edges — unscrollable (#intro is
#   overflow:hidden) — while '.opt{display:none}' also died, showing the
#   disclosure fields BEFORE consent (breaking the ARK-1 read->ack flow).
#   90feb043 rescoped every rule from '#ark X' to '.card X' (unique to the
#   consent card; #logov's twin uses .logcard) so the styling is invariant to
#   which container currently holds the node.
#
# Only the fable5 HTTP/JSON data-plane had certs before this (gap G3 from the
# CI-coverage audit) — the galaxy/consent PIXELS had zero automated coverage,
# so this exact CSS-scoping class of bug had no gate at all.
#
# METHOD: serve the real galaxy.html (whatever path is passed in) over a
# real HTTP origin from a tiny stub server — /manifesto returns a fixture
# with one long UNBROKEN line so the bug is font-metric-independent (any
# monospace renders it far past a phone's 411 CSS px); /profile.json,
# /langs are harmless stubs. This lets the page's OWN arkLoadManifesto()
# fetch populate the card exactly as production does. We then drive the
# SAME functions the real first-run onboarding uses — introOpen(true) and a
# real click on #introskip (whose already-registered handler calls
# introGo(CONSENT_IDX)) — which is what actually invokes mountArkInIntro()
# inside introRender(). HTML/JS structure is identical pre- and post-fix
# (only the <style> block changed in 90feb043), so this same script/DOM
# path runs unmodified against either version — see run_consent_dom.sh's
# sibling teeth-check note.
#
# Usage:
#   consent_dom_cert.py <path-to-galaxy.html>
# Exit 0 = PASS (card healthy at a phone viewport).
# Exit 1 = FAIL (the bug is back, or the cert itself is broken).
# ---------------------------------------------------------------------------
import sys
import os
import json
import http.server
import socket
import threading

from playwright.sync_api import sync_playwright

VIEWPORT = {"width": 411, "height": 800}  # a common phone CSS-px width

# One long UNBROKEN run (no spaces) so the reproduction does not depend on
# exact font metrics: under UA-default <pre> (white-space:pre, no wrap) this
# alone forces the box far past any phone viewport; under the fixed CSS
# (white-space:pre-wrap + overflow-wrap:anywhere) it must wrap and fit.
_LONG_TOKEN = "x" * 180
MARKER = "x" * 10  # substring we can wait for in the DOM
FIXTURE_MANIFESTO = (
    "this is a test manifesto fixture line one.\n"
    + _LONG_TOKEN
    + "\nthis is a test manifesto fixture line three.\n"
)


class _Handler(http.server.BaseHTTPRequestHandler):
    galaxy_path = None  # set by run()

    def log_message(self, *_args):  # keep CI logs quiet
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8", headers=None):
        payload = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/galaxy.html"):
            with open(self.galaxy_path, "rb") as f:
                body = f.read()
            self._send(200, body, "text/html; charset=utf-8")
        elif path == "/manifesto":
            self._send(
                200,
                FIXTURE_MANIFESTO,
                "text/plain; charset=utf-8",
                {"X-Manifesto-Id": "cert-fixture-1", "X-Manifesto-Lang": "en"},
            )
        elif path == "/profile.json":
            self._send(200, json.dumps({"none": True}), "application/json")
        elif path == "/langs":
            self._send(200, json.dumps({"en": "English"}), "application/json")
        else:
            self._send(404, "not found")

    def do_POST(self):
        self._send(404, "not found")


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _measure(galaxy_path):
    _Handler.galaxy_path = galaxy_path
    port = _free_port()
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(args=["--no-sandbox"])
            try:
                page = browser.new_page(viewport=VIEWPORT)
                page.goto(f"http://127.0.0.1:{port}/galaxy.html", wait_until="load")
                # sanity: the inline <script> ran and defined the real fns
                page.wait_for_function(
                    "typeof introOpen==='function' && "
                    "typeof mountArkInIntro==='function'",
                    timeout=10_000,
                )
                # drive the SAME first-run path production uses: open the
                # onboarding, then invoke the real, already-registered
                # #introskip handler (it internally calls
                # introGo(CONSENT_IDX) -> introRender() -> mountArkInIntro()
                # + arkEnterConsent()). We deliberately do NOT reference
                # CONSENT_IDX / introGo ourselves from outside the page's
                # own script scope — clicking the real button is the
                # faithful repro of what a first-run user does.
                page.evaluate("introOpen(true)")
                page.evaluate("document.getElementById('introskip').click()")
                # arkEnterConsent() -> arkResetToAck() fires arkLoadManifesto()
                # (a real, un-awaited fetch to our /manifesto stub); wait for
                # its fixture text to actually land in the DOM.
                page.wait_for_function(
                    "document.getElementById('arktext') && "
                    "document.getElementById('arktext').textContent.includes(%r)"
                    % MARKER,
                    timeout=10_000,
                )
                data = page.evaluate(
                    """() => {
                        const card = document.querySelector('#intromount .card');
                        const pre  = document.getElementById('arktext');
                        const opt  = document.getElementById('arkfields');
                        if (!card || !pre || !opt) {
                            return {mounted: false};
                        }
                        const cardCS = getComputedStyle(card);
                        const preCS  = getComputedStyle(pre);
                        const optCS  = getComputedStyle(opt);
                        return {
                            mounted: true,
                            cardWidth: card.getBoundingClientRect().width,
                            viewportWidth: window.innerWidth,
                            paddingTop: cardCS.paddingTop,
                            whiteSpace: preCS.whiteSpace,
                            optDisplay: optCS.display,
                        };
                    }"""
                )
            finally:
                browser.close()
    finally:
        httpd.shutdown()
        httpd.server_close()
    return data


def _check(results):
    """Returns (ok, [problem strings]). Each check is load-bearing for the
    exact '#ark X' -> 'X moved out of #ark' failure mode; see file header."""
    problems = []
    if not results.get("mounted"):
        problems.append(
            "card never appeared under #intromount .card "
            "(mountArkInIntro()/introskip wiring is broken, not just CSS)"
        )
        return False, problems

    if results["paddingTop"] == "0px":
        problems.append(
            "card paddingTop=0px -- UA default (div has no padding); "
            "the .card rule did not apply while mounted in #intromount"
        )
    if not (results["cardWidth"] <= results["viewportWidth"] + 1):
        problems.append(
            "card width %.1fpx > viewport %spx -- horizontal overflow "
            "(min-content blowout from an un-wrapped <pre>)"
            % (results["cardWidth"], results["viewportWidth"])
        )
    if results["whiteSpace"] != "pre-wrap":
        problems.append(
            "pre white-space=%r (want 'pre-wrap'); UA default 'pre' never "
            "wraps, so a long manifesto line sets the box's min-content width"
            % results["whiteSpace"]
        )
    if results["optDisplay"] != "none":
        problems.append(
            "#arkfields (.opt) display=%r (want 'none' pre-consent); "
            "the disclosure fields are showing before the user has agreed"
            % results["optDisplay"]
        )
    return (len(problems) == 0), problems


def main():
    if len(sys.argv) != 2:
        print("usage: consent_dom_cert.py <path-to-galaxy.html>", file=sys.stderr)
        return 2
    galaxy_path = os.path.abspath(sys.argv[1])
    if not os.path.isfile(galaxy_path):
        print(f"no such file: {galaxy_path}", file=sys.stderr)
        return 2

    results = _measure(galaxy_path)
    ok, problems = _check(results)

    label = galaxy_path
    if ok:
        print(f"[consent-dom] PASS {label}")
        print(f"[consent-dom]   {results}")
        return 0

    print(f"[consent-dom] FAIL {label}")
    for p in problems:
        print(f"[consent-dom]   - {p}")
    print(f"[consent-dom]   raw: {results}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
