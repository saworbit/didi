#!/usr/bin/env python3
"""Assemble the Didi project website from the brand sources and the published docs.

Everything on the site that could go stale is derived rather than typed.

* The marks are inlined from ``docs/brand/svg`` at build time, so the pages use
  the same bytes the addon and the README use, and ``currentColor`` lets each
  one follow the reader's colour scheme.
* The PNG exports are copied from ``docs/brand/png`` unchanged.
* The version comes from ``CMakeLists.txt`` and the surface counts from the
  README status block, which ``tools/validate_documentation.py`` already keeps
  aligned with the built binary. The site cannot publish a number the README
  does not.

The templates under ``site/templates`` are plain HTML with a few placeholders::

    {{partial:NAME}}      site/partials/NAME.html, expanded first
    {{svg:NAME}}          docs/brand/svg/NAME.svg, inlined verbatim
    {{svg:NAME|ATTRS}}    the same, with ATTRS added to the root <svg> tag
    {{root}}              path from the page back to the site root
    {{version}} {{canonical}} {{implemented}} {{unimplemented}}
    {{legacy}} {{total}} {{tests}}
    {{title}} {{description}} {{canonical_url}}

Every rendered page is then checked: no placeholder may survive, and every
relative ``src``, ``href`` and ``srcset`` must name a file the build wrote.
A missing asset fails the build rather than the reader.

Usage::

    python site/build.py --out _site    # write the site
    python site/build.py --check        # build into a temporary directory, verify, discard

Standard library only, like the rest of the repository's tooling.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SITE = REPOSITORY_ROOT / "site"
BRAND = REPOSITORY_ROOT / "docs" / "brand"

# The one place that knows where the site is served from. A project site on
# GitHub Pages lives under the repository name, and the 404 page is served at
# every depth below it, so that page has to use absolute paths.
SITE_URL = "https://saworbit.github.io/didi/"
BASE_PATH = "/didi/"

# Written into the output so a later build can tell the directory is its own
# before removing it. Refusing to delete anything else is the whole point.
MARKER = ".nojekyll"


@dataclass(frozen=True)
class Page:
    template: str
    output: str
    root: str
    title: str
    description: str


PAGES = (
    Page(
        template="index.html",
        output="index.html",
        root="./",
        title="Didi: native MCP server for Godot",
        description=(
            "Didi is a native C++20 Model Context Protocol server for Godot 4.5+. "
            "A standalone binary speaks MCP to your assistant and a GDExtension speaks "
            "Godot inside the editor, over a local named pipe rather than a network."
        ),
    ),
    Page(
        template="brand.html",
        output="brand/index.html",
        root="../",
        title="The Didi mark",
        description=(
            "The Didi identity: a lowercase d whose bowl is entered by a node and a pipe. "
            "The mark, the wordmark, the lockups, the palette, and the files."
        ),
    ),
    Page(
        template="404.html",
        output="404.html",
        root=BASE_PATH,
        title="Not found",
        description="There is nothing at this address.",
    ),
)

PARTIAL_TOKEN = re.compile(r"\{\{partial:([A-Za-z0-9_-]+)\}\}")
SVG_TOKEN = re.compile(r"\{\{svg:([A-Za-z0-9_.-]+)(?:\|([^}]*))?\}\}")
FIELD_TOKEN = re.compile(r"\{\{([a-z_]+)\}\}")
REFERENCE = re.compile(r"""\b(src|href|srcset)=(?:"([^"]*)"|'([^']*)')""")
EXTERNAL = re.compile(r"^(?:[a-z][a-z0-9+.\-]*:|#|//)", re.IGNORECASE)


def _find(pattern: str, text: str, what: str, source: str) -> re.Match[str]:
    match = re.search(pattern, text)
    if match is None:
        raise SystemExit(f"{source}: could not find {what}; the site refuses to guess it")
    return match


def read_facts(root: Path) -> dict[str, str]:
    """Read the version and the surface counts from the files that own them."""

    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    readme = (root / "README.md").read_text(encoding="utf-8")

    version = _find(
        r"project\s*\(\s*didi\s+VERSION\s+(\d+\.\d+\.\d+)\b",
        cmake,
        "the project version",
        "CMakeLists.txt",
    ).group(1)
    status = _find(
        r"\*\*Canonical implementation:\*\*\s+`(\d+)/(\d+)`",
        readme,
        "the canonical implementation status block",
        "README.md",
    )
    implemented, canonical = int(status.group(1)), int(status.group(2))
    legacy = int(
        _find(
            r"\b(\d+) additional legacy registrations",
            readme,
            "the legacy registration count",
            "README.md",
        ).group(1)
    )
    tests = int(
        _find(
            r"img\.shields\.io/badge/tests-(\d+)-",
            readme,
            "the tests badge",
            "README.md",
        ).group(1)
    )
    return {
        "version": version,
        "canonical": str(canonical),
        "implemented": str(implemented),
        "unimplemented": str(canonical - implemented),
        "legacy": str(legacy),
        "total": str(canonical + legacy),
        "tests": str(tests),
    }


def expand_partials(text: str) -> str:
    """Replace every {{partial:NAME}} with site/partials/NAME.html, bounded."""

    for _ in range(8):
        def replace(match: re.Match[str]) -> str:
            path = SITE / "partials" / f"{match.group(1)}.html"
            if not path.is_file():
                raise SystemExit(f"missing partial: {path.relative_to(REPOSITORY_ROOT).as_posix()}")
            return path.read_text(encoding="utf-8").strip()

        expanded = PARTIAL_TOKEN.sub(replace, text)
        if expanded == text:
            return text
        text = expanded
    raise SystemExit("partials nest more than eight levels deep; something includes itself")


def inline_svgs(text: str) -> str:
    """Replace every {{svg:NAME}} with the brand source of that name."""

    def replace(match: re.Match[str]) -> str:
        name, attributes = match.group(1), (match.group(2) or "").strip()
        path = BRAND / "svg" / f"{name}.svg"
        if not path.is_file():
            raise SystemExit(f"missing brand source: {path.relative_to(REPOSITORY_ROOT).as_posix()}")
        source = path.read_text(encoding="utf-8").strip()
        if not source.startswith("<svg "):
            raise SystemExit(f"{path.name}: does not start with an <svg> tag")
        if attributes:
            source = source.replace("<svg ", f"<svg {attributes} ", 1)
        return source

    return SVG_TOKEN.sub(replace, text)


def render(page: Page, facts: dict[str, str]) -> str:
    template = SITE / "templates" / page.template
    if not template.is_file():
        raise SystemExit(f"missing template: {template.relative_to(REPOSITORY_ROOT).as_posix()}")
    text = expand_partials(template.read_text(encoding="utf-8"))
    text = inline_svgs(text)

    fields = dict(facts)
    fields.update(
        root=page.root,
        title=page.title,
        description=page.description,
        canonical_url=SITE_URL + ("" if page.output == "index.html" else page.output.removesuffix("index.html")),
    )

    def replace(match: re.Match[str]) -> str:
        key = match.group(1)
        if key not in fields:
            raise SystemExit(f"{page.template}: unknown placeholder {{{{{key}}}}}")
        return fields[key]

    text = FIELD_TOKEN.sub(replace, text)
    if "{{" in text:
        index = text.index("{{")
        raise SystemExit(f"{page.template}: unexpanded placeholder near: {text[index:index + 40]!r}")
    return text


def prepare_output(out: Path) -> None:
    """Empty the output directory, but only one this script wrote."""

    if out.exists():
        if not out.is_dir():
            raise SystemExit(f"{out}: exists and is not a directory")
        if any(out.iterdir()) and not (out / MARKER).exists():
            raise SystemExit(
                f"{out}: is not empty and was not written by this script "
                f"(no {MARKER} marker); refusing to delete it"
            )
        shutil.rmtree(out)
    out.mkdir(parents=True)


def copy_assets(out: Path) -> None:
    for kind in ("svg", "png"):
        source = BRAND / kind
        if not source.is_dir():
            raise SystemExit(f"missing brand directory: {source.relative_to(REPOSITORY_ROOT).as_posix()}")
        shutil.copytree(source, out / "brand" / kind)
    for static in sorted((SITE / "static").iterdir()):
        if static.is_file():
            shutil.copy2(static, out / static.name)


def _targets(value: str, attribute: str) -> list[str]:
    if attribute == "srcset":
        return [candidate.strip().split()[0] for candidate in value.split(",") if candidate.strip()]
    return [value.strip()]


def verify(out: Path, written: list[Path]) -> list[str]:
    """Every relative reference in every page must resolve inside the output."""

    errors: list[str] = []
    out = out.resolve()
    for page in written:
        text = page.read_text(encoding="utf-8")
        relative_page = page.relative_to(out).as_posix()
        for match in REFERENCE.finditer(text):
            attribute = match.group(1)
            value = match.group(2) if match.group(2) is not None else match.group(3)
            for target in _targets(value, attribute):
                if not target or EXTERNAL.match(target):
                    continue
                path_part = target.split("#", 1)[0].split("?", 1)[0]
                if not path_part:
                    continue
                if path_part.startswith("/"):
                    if not path_part.startswith(BASE_PATH):
                        errors.append(f"{relative_page}: {target} is absolute but outside {BASE_PATH}")
                        continue
                    candidate = out / path_part[len(BASE_PATH):]
                else:
                    candidate = page.parent / path_part
                resolved = candidate.resolve()
                try:
                    resolved.relative_to(out)
                except ValueError:
                    errors.append(f"{relative_page}: {target} escapes the site")
                    continue
                if resolved.is_dir():
                    resolved = resolved / "index.html"
                if not resolved.is_file():
                    errors.append(f"{relative_page}: missing target {target}")
    return errors


def build(out: Path) -> None:
    facts = read_facts(REPOSITORY_ROOT)
    prepare_output(out)
    copy_assets(out)
    written: list[Path] = []
    for page in PAGES:
        destination = out / page.output
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(render(page, facts), encoding="utf-8", newline="\n")
        written.append(destination)
    (out / MARKER).write_text("", encoding="utf-8")

    errors = verify(out, written)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        raise SystemExit(f"{len(errors)} broken reference(s); the site was not verified")

    total = sum(path.stat().st_size for path in out.rglob("*") if path.is_file())
    print(
        f"site written to {out}: {len(written)} pages, "
        f"v{facts['version']}, {facts['implemented']}/{facts['canonical']} canonical tools, "
        f"{total // 1024} KB"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build the Didi project website.")
    parser.add_argument(
        "--out",
        type=Path,
        default=REPOSITORY_ROOT / "_site",
        help="output directory (default: _site at the repository root)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="build into a temporary directory, verify it, and discard it",
    )
    arguments = parser.parse_args(argv)

    if arguments.check:
        with tempfile.TemporaryDirectory(prefix="didi-site-") as temporary:
            build(Path(temporary) / "site")
        return 0

    out = arguments.out.resolve()
    try:
        out.relative_to(SITE.resolve())
    except ValueError:
        pass
    else:
        raise SystemExit("the output directory must not be inside site/")
    build(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
