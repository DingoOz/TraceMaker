---
status: current
code: [mkdocs.yml, scripts/mkdocs_hooks.py, scripts/decisions.py, scripts/gen_reference.py, scripts/docs_check.py, scripts/check_api_docs.py, .github/workflows/docs.yml]
---

# Write and publish the docs

This site is built with [MkDocs](https://www.mkdocs.org/) and the Material theme from the Markdown in `docs/`, and
published to GitHub Pages by CI ([D134](../decisions/D134.md)). Every page also reads correctly on GitHub.

## Preview locally

```bash
python3 -m venv .venv && .venv/bin/pip install -r docs/requirements.txt
.venv/bin/mkdocs serve                      # http://127.0.0.1:8000, reloads on save
.venv/bin/mkdocs build --strict             # what CI runs: any broken link or missing page fails
```

## Where a page goes

The site follows [Diátaxis](https://diataxis.fr/): decide what the reader is doing, and the place follows.

| The reader wants to… | Kind | Folder |
|---|---|---|
| learn by doing, step by step | tutorial | `docs/tutorials/` |
| get one job done | how-to guide | `docs/how-to/` |
| look up a fact | reference | `docs/reference/` (generate it from the code where possible) |
| understand how and why | explanation | the numbered design docs, `docs/NN-name.md` |

The numbered design docs keep their names and stay in `docs/`, because code, commits and issues cite them as
"doc 05 §34" and `docs/05-routing.md`. A new design doc takes the next number. Add every new page to `nav` in
`mkdocs.yml` and to the index page of its section.

## The page header

Every page except the decision records and the generated pages starts with:

```yaml
---
status: current          # current | partly-built | plan | historical
code: [src/route, src/passes]   # the code the page describes; [] for none
reviewed: 2026-10-21     # optional: when someone last checked the page against the code
---
```

The site shows a banner on pages that are not `current`. `scripts/docs_check.py` (run by CI) fails on a missing or
wrong header and warns when a page's `code` has had more than 25 commits since the page last changed or was
reviewed. When it warns, read the page against the code, fix what is wrong, and set `reviewed` to today's date.

## Links

Link to other pages with relative paths to the `.md` file (`[routing](05-routing.md#5-cost-model)`), as GitHub does.
Links to files outside `docs/` (`../src/route/router.cpp`, `../PLAN.md`) are fine: on the site they are pointed at
the file on GitHub by `scripts/mkdocs_hooks.py`.

## Record a decision

A design decision is a record in `docs/decisions/`, one file each:

```bash
scripts/decisions.py new "Short title in plain words" --docs 05,10
```

It takes the next free number, fills in the template (Context, Decision, Alternatives, Consequences) and updates
the index in `docs/12-decisions.md`. Edit the file it prints. When a later decision replaces an earlier one, set
the earlier record's `status: superseded` and `superseded_by: DNNN`, then run `scripts/decisions.py index`.
`scripts/decisions.py check` (CI) verifies the records and that the index is up to date.

A branch started before the records existed may still append a row to the old table in `docs/12-decisions.md`.
`scripts/decisions.py import --title D132="Short title"` turns such rows into records.

## Generated pages

| Page | Made by | When to run it |
|---|---|---|
| [Command line](../reference/cli.md) | `scripts/gen_reference.py cli --build build/release` | after changing an option or its help text |
| [Benchmark results](../reference/benchmarks.md) | `scripts/gen_reference.py bench` | after adding a record to `bench/records/` |
| [Decision index](../12-decisions.md) | `scripts/decisions.py index` | `new` does it; after editing a record's title or status |
| [C++ API](../reference/api.md) | `mkdir -p build/api && doxygen docs/api/Doxyfile` | not committed; CI builds it |

CI fails when a committed generated page differs from what the script makes now.

## Publishing

`.github/workflows/docs.yml` builds the site on every pull request that touches the docs (strict, plus the checks
above) and publishes with [mike](https://github.com/jimporter/mike) to the `gh-pages` branch:

- a push to `main` publishes version `dev`;
- a tag `vX.Y.Z` publishes version `X.Y` and points `latest` (the default) at it.

The Doxygen reference is built in the same job and published under `api/` of each version. GitHub Pages must be set
to deploy from the `gh-pages` branch (repository Settings > Pages), once.
