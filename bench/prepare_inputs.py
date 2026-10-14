# SPDX-License-Identifier: GPL-3.0-or-later
"""Private native conversion, deterministic identities and fixture publication.

Only the unrouted derivative is converted/refilled; source boards and rule bytes
are never rewritten. No routing-retention policy belongs in this module.
"""
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import uuid
from collections import Counter, defaultdict

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOKENS = r'"(?:\\.|[^"\\])*"|[()]|[^\s()"]+'


def expressions(text):
    """Yield balanced expression spans (start, end, depth, head)."""
    stack = []
    for match in re.finditer(TOKENS, text):
        token = match.group()
        if token == '(':
            stack.append([match.start(), len(stack), None])
        elif token == ')':
            if not stack:
                raise ValueError('unbalanced expression')
            start, depth, head = stack.pop()
            yield start, match.end(), depth, head
        elif stack and stack[-1][2] is None:
            stack[-1][2] = token
    if stack:
        raise ValueError('unbalanced expression')


def parse(text):
    stack, current = [], []
    for match in re.finditer(TOKENS, text):
        token = match.group()
        if token == '(':
            stack.append(current)
            current = []
        elif token == ')':
            if not stack:
                raise ValueError('unbalanced expression')
            node, current = current, stack.pop()
            current.append(node)
        else:
            current.append(re.sub(r'\\(.)', r'\1', token[1:-1]) if token.startswith('"') else token)
    if stack or len(current) != 1:
        raise ValueError('unbalanced expression')
    return current[0]


def child(node, key):
    return next((x for x in node if isinstance(x, list) and x and x[0] == key), None)


def replace_spans(text, changes):
    for start, stop, replacement in sorted(changes, reverse=True):
        text = text[:start] + replacement + text[stop:]
    return text


def stabilize_legacy_paths(text, board_name):
    """Avoid loader-random UUIDs for non-KiCad schematic path components."""
    seed = hashlib.sha256(text.encode()).hexdigest()
    changes = []
    for start, stop, _, head in expressions(text):
        if head != 'path':
            continue
        node = parse(text[start:stop])
        if len(node) != 2 or not node[1].startswith('/'):
            continue
        components = node[1].split('/')
        stable = []
        for component in components:
            if not component or re.fullmatch(r'[0-9a-fA-F]{1,8}', component):
                stable.append(component)  # KiCad converts legacy timestamps deterministically.
                continue
            try:
                if len(component) != 36:
                    raise ValueError('non-native path UUID')
                uuid.UUID(component)
                stable.append(component)
            except ValueError:
                stable.append(str(uuid.uuid5(uuid.NAMESPACE_URL,
                                             f'tracemaker-prepared-path:{board_name}:{seed}:{component}')))
        if stable != components:
            changes.append((start, stop, '(path ' + json.dumps('/'.join(stable)) + ')'))
    return replace_spans(text, changes)


def canonicalize_generated_ids(text, original, board_name):
    """Preserve unique native source IDs; replace conversion-generated identities.

    KiCad saves drawings/zones in UUID order. Identity-free content, rather than
    converted list position, seeds both containers and their nested items.
    """
    def semantic(node):
        if not isinstance(node, list):
            return node
        values, groups = [], defaultdict(list)
        for item in node:
            if isinstance(item, list) and item[0] in ('uuid', 'tstamp', 'filled_polygon', 'fill_segments'):
                continue
            if isinstance(item, list) and any(child(item, key) is not None for key in ('uuid', 'tstamp')):
                groups[item[0]].append(len(values))
            values.append(semantic(item))
        for slots in groups.values():
            ordered = sorted((values[index] for index in slots),
                             key=lambda value: json.dumps(value, separators=(',', ':')))
            for index, value in zip(slots, ordered):
                values[index] = value
        return values

    def fingerprint(span):
        node = parse(text[span[0]:span[1]])
        return hashlib.sha256(json.dumps(semantic(node), separators=(',', ':')).encode()).hexdigest()

    native = Counter()
    for start, stop, _, head in expressions(original):
        if head in ('uuid', 'tstamp'):
            value = parse(original[start:stop])[1]
            if len(value) == 36:  # Legacy eight-hex tstamp is not a globally unique identity.
                native[str(uuid.UUID(value))] += 1
    preserved = {value for value, count in native.items() if count == 1 and uuid.UUID(value).int}
    seed = hashlib.sha256(original.encode()).hexdigest()
    stack, changes, occurrences = [], [], Counter()
    used, fingerprints = set(preserved), {}
    for start, stop, depth, head in sorted(expressions(text)):
        while stack and stack[-1][1] <= start:
            stack.pop()
        if head == 'uuid':
            value = str(uuid.UUID(parse(text[start:stop])[1]))
            if value not in preserved:
                owner = stack[-1]
                container = stack[1] if len(stack) > 1 else owner
                for span in (container, owner):
                    if span not in fingerprints:
                        fingerprints[span] = fingerprint(span)
                key = (fingerprints[container], fingerprints[owner])
                rank = occurrences[key]
                occurrences[key] += 1
                attempt = 0
                while True:
                    value = str(uuid.uuid5(uuid.NAMESPACE_URL,
                                          f'tracemaker-prepared-v2:{board_name}:{seed}:{key}:{rank}:{attempt}'))
                    if value not in used:
                        break
                    attempt += 1
                used.add(value)
                changes.append((start, stop, f'(uuid "{value}")'))
        stack.append((start, stop))
    return replace_spans(text, changes)


def repair_design_ids(text, board_name):
    """Repair zero/duplicate saved IDs and missing zone/via IDs without geometry changes."""
    spans = sorted(expressions(text))
    used = {str(uuid.UUID(parse(text[s:e])[1])) for s, e, _, head in spans if head == 'uuid'}
    seen, changes = set(), []
    for index, (start, stop, depth, head) in enumerate(spans):
        if head == 'uuid':
            value = str(uuid.UUID(parse(text[start:stop])[1]))
            if uuid.UUID(value).int and value not in seen:
                seen.add(value)
                continue
            key = f'identity:{index}'
            gap = False
        elif depth == 1 and head in ('zone', 'via') and child(parse(text[start:stop]), 'uuid') is None:
            key = f'{head}:{index}'
            gap = True
        else:
            continue
        attempt = 0
        while True:
            replacement = str(uuid.uuid5(uuid.NAMESPACE_URL,
                                         f'tracemaker-prepared:{board_name}:{key}:{attempt}'))
            if replacement not in used:
                break
            attempt += 1
        used.add(replacement)
        seen.add(replacement)
        changes.append((stop - 1, stop - 1, f'\n(uuid "{replacement}")') if gap
                       else (start, stop, f'(uuid "{replacement}")'))
    return replace_spans(text, changes)


def canonicalize_item_order(text):
    """Sort same-kind UUID-bearing siblings before KiCad consumes their geometry."""
    spans = sorted(expressions(text))
    stack, siblings = [], defaultdict(list)
    for span in spans:
        while stack and stack[-1][1] <= span[0]:
            stack.pop()
        if stack:
            siblings[stack[-1]].append(span)
        stack.append(span)
    rendered = {}
    for span in reversed(spans):
        start, stop, _, _ = span
        groups = defaultdict(list)
        for item in siblings[span]:
            identity = next((text[s:e] for s, e, _, head in siblings[item] if head == 'uuid'), None)
            if identity is not None:
                groups[item[3]].append((identity, item))
        replacements = {}
        for group in groups.values():
            ordered = sorted(group, key=lambda pair: (pair[0], rendered[pair[1]]))
            for (_, slot), (_, item) in zip(group, ordered):
                replacements[slot] = rendered[item]
        result, cursor = [], start
        for item in siblings[span]:
            result.extend((text[cursor:item[0]], replacements.get(item, rendered[item])))
            cursor = item[1]
        result.append(text[cursor:stop])
        rendered[span] = ''.join(result)
    return text[:spans[0][0]] + rendered[spans[0]] + text[spans[0][1]:] if spans else text


def native_tools():
    cli = os.environ.get('KICAD_CLI') or shutil.which('kicad-cli') or str(ROOT / 'scripts/kicad-cli')
    interpreter = os.environ.get('KICAD_PYTHON')
    if not interpreter:
        resolved = pathlib.Path(shutil.which(cli) or cli).resolve()
        if resolved == (ROOT / 'scripts/kicad-cli').resolve():
            interpreter = str(ROOT / 'scripts/kicad-python')
        elif resolved.parent.name == 'MacOS':
            versions = resolved.parent.parent / 'Frameworks/Python.framework/Versions'
            candidates = [versions / 'Current/bin/python3', *sorted(versions.glob('*/bin/python3'))]
            interpreter = next((str(p) for p in candidates if p.is_file() and os.access(p, os.X_OK)), None)
        else:
            interpreter = sys.executable
    if not interpreter:
        raise RuntimeError('KiCad Python not found; set KICAD_PYTHON to its interpreter')
    return cli, interpreter


def convert_board(work, cli, interpreter, timeout):
    """Load/Save only: never run DRC/refill with uncorrected loader identities."""
    version = subprocess.run([cli, 'version'], capture_output=True, text=True, timeout=30, check=True)
    match = re.search(r'(?<!\d)(\d+\.\d+\.\d+)(?!\d)', version.stdout)
    if not match:
        raise RuntimeError(f'Unrecognised KiCad version: {version.stdout!r}')
    script = r'''
import re
import sys
import pcbnew
actual = pcbnew.Version() if hasattr(pcbnew, "Version") else pcbnew.GetBuildVersion()
match = re.search(r"(?<!\d)(\d+\.\d+\.\d+)(?!\d)", actual)
if not match or match.group(1) != sys.argv[1]:
    raise RuntimeError("KiCad Python version " + actual + " != CLI " + sys.argv[1])
board = pcbnew.LoadBoard(sys.argv[2])
if board is None or not pcbnew.SaveBoard(sys.argv[2], board):
    raise RuntimeError("Cannot convert board: " + sys.argv[2])
'''
    result = subprocess.run([interpreter, '-c', script, match.group(1), str(work)],
                            capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f'KiCad conversion failed (set KICAD_PYTHON for pcbnew): {result.stderr or result.stdout}')


def invalidate_fixture(destination):
    """Remove only generated runnable artifacts; preserve human boards/DSN files."""
    artifacts = [destination, destination.with_suffix('.kicad_pro'), destination.with_suffix('.kicad_dru'),
                 destination.parent / 'preparation.drc.json']
    for path in artifacts:
        path.unlink(missing_ok=True)


def prepare_board(source, destination, transform, extra_files=None, timeout=600):
    """Publish the board last; failure removes runnable stale artifacts, not human originals."""
    if source.resolve() == destination.resolve():
        raise ValueError('preparation source must not be the generated destination')
    destination.parent.mkdir(parents=True, exist_ok=True)
    report = destination.parent / 'preparation.drc.json'
    artifacts = [destination, destination.with_suffix('.kicad_pro'), destination.with_suffix('.kicad_dru'), report]
    # Existing consumers do not read manifests: an old runnable board must not
    # survive a failed re-preparation and masquerade as the requested input.
    invalidate_fixture(destination)
    try:
        with tempfile.TemporaryDirectory(prefix='.prepare-', dir=destination.parent) as tmp:
            stage = pathlib.Path(tmp)
            work = stage / destination.name
            private_report = stage / report.name
            original = transform(source.read_text(encoding='utf-8'))
            work.write_text(stabilize_legacy_paths(original, destination.parent.name), encoding='utf-8')
            for ext in ('.kicad_pro', '.kicad_dru'):
                rules = source.with_suffix(ext)
                if rules.exists():
                    shutil.copyfile(rules, work.with_suffix(ext))
            cli, interpreter = native_tools()
            convert_board(work, cli, interpreter, timeout)
            corrected = canonicalize_generated_ids(work.read_text(encoding='utf-8'), original, destination.parent.name)
            corrected = repair_design_ids(corrected, destination.parent.name)
            corrected = canonicalize_item_order(corrected)
            corrected = replace_spans(corrected, [(s, e, '') for s, e, _, head in expressions(corrected)
                                                  if head in ('filled_polygon', 'fill_segments')])
            work.write_text(corrected, encoding='utf-8')
            result = subprocess.run([cli, 'pcb', 'drc', '--format', 'json', '--severity-all', '--all-track-errors',
                                     '--refill-zones', '--save-board', '-o', str(private_report), str(work)],
                                    capture_output=True, text=True, timeout=timeout)
            if result.returncode != 0 or not private_report.exists():
                raise RuntimeError(f'KiCad refill failed for {source}: {result.stderr or result.stdout}')
            json.loads(private_report.read_text(encoding='utf-8'))
            # KiCad aliases dimension display-text UUIDs to their parent on save.
            work.write_text(repair_design_ids(work.read_text(encoding='utf-8'), destination.parent.name), encoding='utf-8')
            # Native tools may update a private project; publish the source's exact rule bytes.
            for ext in ('.kicad_pro', '.kicad_dru'):
                rules = source.with_suffix(ext)
                if rules.exists():
                    shutil.copyfile(rules, work.with_suffix(ext))
                else:
                    work.with_suffix(ext).unlink(missing_ok=True)
            for name, path in (extra_files or {}).items():
                shutil.copyfile(path, stage / name)
            for path in sorted(stage.iterdir()):
                if path != work and path.name in {p.name for p in artifacts} | set(extra_files or {}):
                    os.replace(path, destination.parent / path.name)
            os.replace(work, destination)
    except BaseException:
        invalidate_fixture(destination)
        raise
    return destination
