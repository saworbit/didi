"""Executable bindings and nested fields for independent observed-state cases."""
import ast
import json
import re
from pathlib import Path

TESTS = Path(__file__).resolve().parent


def case_binding_problems(binding, tests=TESTS, tool=None):
    if not isinstance(binding, dict) or set(binding) != {"source", "function"}:
        return ["case must name exactly source and function"]
    source, function = binding["source"], binding["function"]
    if not isinstance(source, str) or Path(source).name != source or not source.endswith((".py", ".ps1")):
        return ["case source must be a Python or PowerShell file in tests"]
    if not isinstance(function, str) or not function:
        return ["case function must name an executable"]
    path = tests / source
    if not path.is_file():
        return [f"case source {source} is not defined"]
    text = path.read_text(encoding="utf-8")
    observes = False
    if path.suffix == ".py":
        parts = function.split(".")
        if len(parts) != 2 or not parts[1].startswith("test_"):
            return ["Python case must name Class.test_method"]
        tree = ast.parse(text)
        methods = [method for node in tree.body if isinstance(node, ast.ClassDef) and node.name == parts[0]
                   for method in node.body if isinstance(method, ast.FunctionDef) and method.name == parts[1]]
        found = bool(methods)
        if found and tool:
            observes = any(isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                           and node.func.attr == "check_observed" and node.args
                           and isinstance(node.args[0], ast.Constant) and node.args[0].value == tool
                           for node in ast.walk(methods[0]))
    else:
        definition = re.search(r"^function\s+" + re.escape(function) + r"(?:\s|\()", text, re.M | re.I)
        found = definition is not None
        if found and tool:
            following = re.search(r"^function\s+", text[definition.end():], re.M)
            end = definition.end() + following.start() if following else len(text)
            body = text[definition.end():end]
            observes = bool(re.search(r"^\s*Assert-ObservedExternalFields\s+(['\"])" + re.escape(tool) + r"\1(?:\s|$)", body, re.M))
    if not found:
        return [f"case {source}:{function} is not defined"]
    if tool and not observes:
        return [f"case {source}:{function} does not assert observed state for {tool}"]
    return []


def field_value(payload, path):
    current = payload
    for part in path.split("."):
        if not part or not isinstance(current, dict) or part not in current:
            raise AssertionError(f"Observed field {path} is missing")
        current = current[part]
    return current


def assert_observed(test, tool, payload, witness):
    entry = json.loads((TESTS / "observed_post_state.json").read_text(encoding="utf-8"))["tools"][tool]
    for field in entry["observed"]:
        test.assertEqual(field_value(payload, field), field_value(witness, field), f"{tool}.{field}")


if __name__ == "__main__":
    registry = json.loads((TESTS / "observed_post_state.json").read_text(encoding="utf-8"))
    problems = [f"{tool}: {problem}" for tool, entry in registry["tools"].items() if "case" in entry
                for problem in case_binding_problems(entry["case"], tool=tool)]
    if problems:
        raise SystemExit("\n".join(problems))
    print("External observed cases: executable bindings valid.")
