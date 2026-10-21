"""Versioned, conservative direct-dependency selection; no model calls."""
import ast
import re
from pathlib import Path


def enabled(source: Path) -> bool:
    import workflow as core
    return core.load(source / 'workflow/tool-lock.json')['ocr'].get('review_policy_version') == 2


def documents(source: Path, names: list[str]) -> list[str]:
    if not enabled(source):
        return []
    return sorted(n for n in names if n.endswith('.md') and (
        n == 'docs/README.md' or n.startswith(('docs/research/', 'docs/archive/'))))


def related(group: dict, callers: dict, extra: list[str], removed: list[str] = ()) -> dict:
    """One-hop imports, reverse imports and explicit file references, never truncation."""
    current = callers
    # Register removed paths without running/adding their historical body. Current
    # imports and file references must still resolve to them for reverse edges.
    callers = dict(callers, **{name: '' for name in removed if name not in callers})
    edges = {name: set() for name in callers}
    modules = {}
    for name in callers:
        if name.endswith('.py'):
            modules.setdefault(Path(name).stem, set()).add(name)
            modules.setdefault(name[:-3].replace('/', '.'), set()).add(name)
            if name.endswith('/__init__.py'):
                modules.setdefault(name[:-12].replace('/', '.'), set()).add(name)
                modules.setdefault(Path(name).parent.name, set()).add(name)
    for name, text in callers.items():
        if name.endswith('.py'):
            tree = ast.parse(text, filename=name)
            for node in ast.walk(tree):
                imported = []
                if isinstance(node, ast.Import):
                    imported = [item.name for item in node.names]
                elif isinstance(node, ast.ImportFrom):
                    imported = [node.module or '']
                    imported += [((node.module + '.') if node.module else '') + a.name for a in node.names]
                elif isinstance(node, ast.Call) and (isinstance(node.func, ast.Name) and node.func.id == '__import__'
                        or isinstance(node.func, ast.Attribute) and node.func.attr == 'import_module'):
                    if not node.args or not isinstance(node.args[0], ast.Constant) or not isinstance(node.args[0].value, str):
                        # Only obstruct when this module belongs to the selected neighborhood.
                        edges[name].add('@dynamic')
                    else:
                        imported = [node.args[0].value]
                for module in imported:
                    candidates = modules.get(module, set())
                    local = str(Path(name).parent / (module.replace('.', '/') + '.py')).replace('\\', '/')
                    package = (Path(name).parent / module.replace('.', '/') / '__init__.py').as_posix()
                    if local in callers:
                        edges[name].add(local)
                    elif package in callers:
                        edges[name].add(package)
                    elif len(candidates) == 1:
                        edges[name].update(candidates)
                    elif candidates:
                        edges[name].add('@dynamic')
        # Local include paths and literal repository path references.
        for target in re.findall(r'[\'"]([^\'"\n]+)[\'"]', text):
            local = (Path(name).parent / target).as_posix()
            for candidate in (target.replace('\\', '/'), local):
                if candidate in callers:
                    edges[name].add(candidate)
        edges[name].update(target for target in callers if target != name and target in text)
    selected = set(group) | set(extra) | set(removed)
    for name in set(group) | set(removed):
        selected.update(edges.get(name, ()))
        selected.update(caller for caller, targets in edges.items() if name in targets)
    if any('@dynamic' in edges.get(name, ()) for name in selected) or '@dynamic' in selected:
        raise ValueError('Unresolved dynamic dependency; split or make the review dependency explicit before payment')
    missing = set(extra) - set(callers)
    if missing:
        raise ValueError('Explicit review_paths are missing or unsupported: ' + ', '.join(sorted(missing)))
    result = {name: current[name] for name in sorted(selected) if name in current}
    if any(len(text.encode('utf-8')) > 100000 for text in result.values()):
        raise ValueError('Caller context file exceeds 100000-byte limit')
    if sum(len(text.encode('utf-8')) for text in result.values()) > 250000:
        raise ValueError('Direct dependency context exceeds 250000 bytes; split task before payment')
    return result
