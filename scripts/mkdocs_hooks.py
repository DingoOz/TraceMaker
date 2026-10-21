# SPDX-License-Identifier: GPL-3.0-or-later
"""MkDocs hooks for the documentation site (D134).

The design docs are written to be read on GitHub as well as on the site, so they link to files outside docs/
(`../PLAN.md`, `../research/...`, `../src/route/...`) with plain relative paths. On the site those files do not
exist; this hook points such links at the same file on GitHub instead of leaving them broken. The source Markdown
is not changed. TM_DOCS_REF picks the branch or tag the links point at (default: main).

It also puts a banner on pages whose header says they are not `current` (scripts/docs_check.py checks the headers),
and copies the Doxygen API reference (build/api/html, when it has been built) into the site as api/.
"""
import os
import re
import shutil

REPO = "https://github.com/DingoOz/TraceMaker"
REF = os.environ.get("TM_DOCS_REF", "main")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# [text](target) and [text](target "title"); images too. Targets with a scheme, a leading '/' or '#' are left alone.
LINK = re.compile(r"(\]\()([^)\s#]+)(#[^)\s]*)?((?:\s+\"[^\"]*\")?\))")
FENCE = re.compile(r"^(```|~~~)")
BANNERS = {
    "partly-built": ("info", "Partly built", "Part of this page describes code that exists; the rest is a plan. The page says which is which."),
    "plan": ("warning", "Plan", "Nothing on this page is built yet, or it is a proposal."),
    "historical": ("note", "Historical", "Kept for the record; the code has moved on."),
}


def _rewrite(target: str, page_dir: str, docs_dir: str, files) -> str | None:
    if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("/"):
        return None
    path = os.path.normpath(os.path.join(page_dir, target))
    rel_docs = os.path.relpath(path, docs_dir)
    if not rel_docs.startswith(".."):
        if files.get_file_from_path(rel_docs.replace(os.sep, "/")) is not None:
            return None  # a page or asset of the site: MkDocs resolves it
    rel_repo = os.path.relpath(path, ROOT)
    if rel_repo.startswith(".."):
        return None
    kind = "tree" if os.path.isdir(path) else "blob"
    return f"{REPO}/{kind}/{REF}/{rel_repo.replace(os.sep, '/')}"


def on_page_markdown(markdown, page, config, files):
    docs_dir = os.path.abspath(config["docs_dir"])
    page_dir = os.path.dirname(os.path.join(docs_dir, page.file.src_path))
    out, in_fence = [], False
    for line in markdown.split("\n"):
        if FENCE.match(line.lstrip()):
            in_fence = not in_fence
        if not in_fence and "](" in line:
            def sub(m):
                new = _rewrite(m.group(2), page_dir, docs_dir, files)
                return m.group(0) if new is None else f"{m.group(1)}{new}{m.group(3) or ''}{m.group(4)}"
            line = LINK.sub(sub, line)
        out.append(line)
    markdown = "\n".join(out)
    banner = BANNERS.get(page.meta.get("status", ""))
    if banner:
        kind, title, text = banner
        # After the page's first heading, so the title stays the title.
        m = re.search(r"^# .*$", markdown, re.M)
        at = m.end() if m else 0
        markdown = f'{markdown[:at]}\n\n!!! {kind} "Status: {title}"\n    {text}\n{markdown[at:]}'
    return markdown


def on_post_build(config):
    api = os.path.join(ROOT, "build", "api", "html")
    if os.path.isdir(api):
        shutil.copytree(api, os.path.join(config["site_dir"], "api"), dirs_exist_ok=True)
