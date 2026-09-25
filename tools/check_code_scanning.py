#!/usr/bin/env python3
"""Read the code scanning tab back, and name the alerts that were dismissed once already.

SECURITY.md argues that a Security tab which always shows alerts is one nobody
opens, and every finding here carries a written disposition. Nothing kept that
true. Every workflow that touches code scanning writes to it and none read it
back, so seven alerts sat open on 2026-09-20 without announcing themselves
(#807).

Two of those were the case that recurs by design. A CodeQL dismissal binds to
the code it was made against, so a commit that moves the lines around a
dismissed finding closes it and raises the same finding under a new number,
with the dismissal gone. This lists those apart from the rest, with the
dismissal each most likely repeats, because they look new and are not. It
dismisses nothing: SECURITY.md asks for the taint path to be read again first,
since the point of a re-raise is that the code moved.

Usage::

    python tools/check_code_scanning.py            # report
    python tools/check_code_scanning.py --check    # exit 1 while any alert is open
    python tools/check_code_scanning.py --json     # machine-readable

It reads GITHUB_TOKEN, or GH_TOKEN, which needs security-events read, and
GITHUB_REPOSITORY unless --repo is given. Exit 2 means the tab could not be
read, which is never reported as a clean one. The weekly job in
.github/workflows/supply-chain.yml runs --check and keeps one tracking issue.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from urllib.parse import urlencode

API = "https://api.github.com"
PAGE = 100


class ReadError(RuntimeError):
    pass


def fetch_alerts(repo: str, state: str, token: str) -> list[dict]:
    alerts: list[dict] = []
    page = 1
    while True:
        query = urlencode({"state": state, "per_page": PAGE, "page": page})
        request = urllib.request.Request(
            f"{API}/repos/{repo}/code-scanning/alerts?{query}",
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {token}",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                batch = json.load(response)
        except urllib.error.HTTPError as error:
            hint = ""
            if error.code in (401, 403, 404):
                hint = " The token needs security-events read on this repository."
            raise ReadError(
                f"GET code-scanning/alerts?state={state} answered {error.code}.{hint}"
            ) from error
        except (urllib.error.URLError, TimeoutError, ValueError) as error:
            raise ReadError(f"GET code-scanning/alerts?state={state} failed: {error}") from error
        if not isinstance(batch, list):
            raise ReadError(f"GET code-scanning/alerts?state={state} did not answer a list.")
        alerts.extend(batch)
        if len(batch) < PAGE:
            return alerts
        page += 1


def _location(alert: dict) -> tuple[str | None, int | None]:
    location = (alert.get("most_recent_instance") or {}).get("location") or {}
    return location.get("path"), location.get("start_line")


def _same_finding(alert: dict, other: dict) -> bool:
    return (
        other.get("number") != alert.get("number")
        and other["tool"]["name"] == alert["tool"]["name"]
        and other["rule"]["id"] == alert["rule"]["id"]
        and _location(other)[0] == _location(alert)[0]
    )


def findings(open_alerts: list[dict], dismissed: list[dict]) -> list[dict]:
    """One entry per open alert, oldest number first.

    An alert repeats a dismissal when the same tool raised the same rule in the
    same file. The line is not compared, because the line moving is exactly
    what raised it again. The newest such dismissal is the one named.
    """
    result = []
    for alert in sorted(open_alerts, key=lambda a: a["number"]):
        path, line = _location(alert)
        rule = alert["rule"]
        entry = {
            "number": alert["number"],
            "tool": alert["tool"]["name"],
            "rule": rule["id"],
            "severity": rule.get("security_severity_level") or rule.get("severity"),
            "path": path,
            "line": line,
            "created_at": alert.get("created_at"),
            "url": alert.get("html_url"),
        }
        earlier = sorted(
            (d for d in dismissed if _same_finding(alert, d)),
            key=lambda d: d.get("dismissed_at") or "",
            reverse=True,
        )
        if earlier:
            prior = earlier[0]
            entry["repeats"] = {
                "number": prior["number"],
                "dismissed_at": prior.get("dismissed_at"),
                "dismissed_reason": prior.get("dismissed_reason"),
                "dismissed_comment": prior.get("dismissed_comment"),
            }
        result.append(entry)
    return result


def _describe(entry: dict) -> str:
    where = entry["path"] or "no file"
    if entry["path"] and entry["line"]:
        where += f":{entry['line']}"
    since = (entry["created_at"] or "")[:10]
    return (
        f"  - #{entry['number']} {entry['tool']} {entry['rule']} ({entry['severity']}) "
        f"{where}, open since {since}"
    )


def render(entries: list[dict]) -> str:
    if not entries:
        return "No code scanning alerts are open.\n"
    fresh = [e for e in entries if "repeats" not in e]
    repeats = [e for e in entries if "repeats" in e]
    lines = [f"Open code scanning alerts: {len(entries)}"]
    if fresh:
        lines += ["", "Not seen before:"]
        lines += [_describe(e) for e in fresh]
    if repeats:
        lines += [
            "",
            "Raised again after a dismissal. The code around each one moved, so read",
            "the path again before dismissing it again (SECURITY.md):",
        ]
        for entry in repeats:
            prior = entry["repeats"]
            lines.append(_describe(entry))
            lines.append(
                f"    repeats #{prior['number']}, dismissed "
                f"{(prior['dismissed_at'] or '')[:10]} as {prior['dismissed_reason']}"
            )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"),
                        help="owner/name. Defaults to GITHUB_REPOSITORY.")
    parser.add_argument("--check", action="store_true",
                        help="Exit 1 while any alert is open.")
    parser.add_argument("--json", action="store_true", help="Print the findings as JSON.")
    args = parser.parse_args(argv)

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not args.repo or not token:
        print("check_code_scanning: needs --repo or GITHUB_REPOSITORY, and GITHUB_TOKEN "
              "or GH_TOKEN with security-events read.", file=sys.stderr)
        return 2
    try:
        open_alerts = fetch_alerts(args.repo, "open", token)
        dismissed = fetch_alerts(args.repo, "dismissed", token) if open_alerts else []
    except ReadError as error:
        print(f"check_code_scanning: {error}", file=sys.stderr)
        return 2

    entries = findings(open_alerts, dismissed)
    if args.json:
        print(json.dumps(entries, indent=2))
    else:
        sys.stdout.write(render(entries))
    return 1 if args.check and entries else 0


if __name__ == "__main__":
    sys.exit(main())
