"""Read the tool calls a tester made, whichever client hosted it.

Coverage and the bridge verdict both come from the transcript rather than the
server log, because `handleRequest` names a tool only when the call throws. That
decision is sound and it quietly tied both readers to one client's file format.

Trial 04 is the first run by a tester that is not Claude, and a second engine is
the whole point: three runs of the same seed have now agreed on which 40% of the
surface an agent reaches for, and one client's habits cannot be told apart from
an agent's habits until a different client has been asked.

Two shapes arrive here:

* The Claude client files a transcript of message records whose content holds
  `tool_use` blocks and, in a later record, the matching `tool_result`.
* `codex exec --json` prints an event per line, and a completed MCP call arrives
  as one `item.completed` record carrying the server, the tool and the result
  together.

Both reduce to the same thing: a call, in order, with whatever came back from
it. Correlating the Claude shape by id rather than scanning loose matters and is
kept here: a log file the tester happened to read, or a payload it quoted into a
note, is not evidence that a call was made.
"""

from __future__ import annotations

import json
from typing import Iterable, NamedTuple


class Invocation(NamedTuple):
    """One call to the server under test, and what came back.

    `result` is None when the transcript holds no answer for the call. That is
    not the same as an empty answer: a run cut off mid-call, or a client that
    does not record results, both land here, and a reader that treats None as
    "nothing to see" reports a clean bridge for a run it could not read.

    `arguments` is what the call was sent, as the client recorded it, and
    `is_error` whether the answer says the call failed. Both exist so a run can
    be scored on how it called tools, not only on which ones.
    """

    tool: str
    result: str | None = None
    arguments: dict | None = None
    is_error: bool = False


def _claude_blocks(record: object) -> list[dict]:
    if not isinstance(record, dict):
        return []
    message = record.get("message")
    if not isinstance(message, dict):
        return []
    content = message.get("content")
    if not isinstance(content, list):
        return []
    return [block for block in content if isinstance(block, dict)]


def _result_text(content: object) -> str:
    """The text of a result, whichever shape the client wrote it in.

    A result arrives as a plain string, as a list of content blocks, or as an
    object holding that list, depending on the client and the tool. A reader
    that handles only the shape it happened to see first reports a clean bridge
    for a run it could not read.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, dict):
        return _result_text(content.get("content"))
    if isinstance(content, list):
        return "\n".join(
            part.get("text", "")
            for part in content
            if isinstance(part, dict) and isinstance(part.get("text"), str)
        )
    return ""


def _arguments(raw: object) -> dict:
    """A call's arguments as an object, whether the client wrote one or its text."""
    if isinstance(raw, str):
        try:
            raw = json.loads(raw)
        except json.JSONDecodeError:
            return {}
    return raw if isinstance(raw, dict) else {}


def _reports_error(result: object, text: str) -> bool:
    """Whether an answer says the call failed.

    The client's own flag is the first witness, and the callers check it. This
    reads the answer for the ones that carry no flag: an MCP result with
    `isError`, a tool's `{"error": ...}` payload, or, in a trial run by hand, the
    whole JSON-RPC envelope written out as text.
    """
    if isinstance(result, dict) and result.get("isError") is True:
        return True
    try:
        value = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return False
    if not isinstance(value, dict):
        return False
    if value.get("isError") is True or (value.get("error") and "result" not in value):
        return True
    inner = value.get("result")
    return isinstance(inner, dict) and inner.get("isError") is True


def _codex_item(record: object) -> dict | None:
    """The completed MCP call in a `codex exec --json` event, if that is what this is.

    Only `item.completed` counts. `item.started` announces the same call before
    it has an answer, so counting both doubles every invocation in the run.
    """
    if not isinstance(record, dict) or record.get("type") != "item.completed":
        return None
    item = record.get("item")
    if not isinstance(item, dict) or item.get("type") != "mcp_tool_call":
        return None
    return item


def iter_invocations(
    transcript_lines: Iterable[str], server: str = "didi"
) -> list[Invocation]:
    """Every call this run made to `server`, in the order the transcript records them.

    Read into a list rather than streamed, because the Claude shape puts a
    result in a later record than its call and a call with no answer still
    counts. A transcript is appended to while a session runs and can hold
    partial or non-message records, so an unreadable line is skipped rather than
    fatal.
    """
    prefix = f"mcp__{server}__"
    invocations: list[Invocation] = []
    # Index into `invocations` rather than a name, so a result attaches to the
    # call that produced it even when the same tool is called many times.
    pending: dict[str, int] = {}

    for line in transcript_lines:
        stripped = line.strip()
        if not stripped:
            continue
        try:
            record = json.loads(stripped)
        except json.JSONDecodeError:
            continue

        item = _codex_item(record)
        if item is not None:
            if item.get("server") != server:
                continue
            tool = item.get("tool")
            if isinstance(tool, str) and tool:
                result = item.get("result")
                text = _result_text(result) if result is not None else None
                invocations.append(Invocation(
                    tool, text, _arguments(item.get("arguments")),
                    bool(item.get("error")) or _reports_error(result, text or ""),
                ))
            continue

        for block in _claude_blocks(record):
            kind = block.get("type")
            if kind == "tool_use":
                name = block.get("name")
                identifier = block.get("id")
                if isinstance(name, str) and name.startswith(prefix):
                    invocations.append(
                        Invocation(name[len(prefix) :], arguments=_arguments(block.get("input")))
                    )
                    if isinstance(identifier, str):
                        pending[identifier] = len(invocations) - 1
            elif kind == "tool_result":
                index = pending.pop(block.get("tool_use_id"), None)
                if index is not None:
                    text = _result_text(block.get("content"))
                    invocations[index] = invocations[index]._replace(
                        result=text,
                        is_error=block.get("is_error") is True or _reports_error(None, text),
                    )
    return invocations
