---
status: current
code: [scripts/make_release.py, scripts/make_pcm_package.py, scripts/test_release_install.py, .github/workflows/release.yml, .github/workflows/docs.yml]
---

# Make a release

A release is a version tag, two files built by CI ([D121](../decisions/D121.md)), release notes and a versioned
copy of this documentation. The owner pushes the tag and publishes the release.

## 1. Measure

On the release branch:

1. `ctest --preset release`: every test passes (skips are listed in the record).
2. Tiers A–D at 120 s ([benchmarks](benchmarks.md#tiers-ad-release-numbers)), ideally on an idle machine. Run the
   previous release's binary beside it if the machine is shared.
3. Copy the runs, the test log and a README with the tables into `bench/records/release-X.Y.Z/`, then
   `python3 scripts/gen_reference.py bench`.

## 2. Update the version and the words

- The version in `CMakeLists.txt` (`project(... VERSION X.Y.Z)`), which `tracemaker version` prints.
- README: *How good is it?*, the test count, *Known limits*. State both readings when the judge changed between
  releases.
- `docs/11-roadmap.md` and `dev/progress.json` for the milestones the release closes.
- `python3 scripts/gen_reference.py cli --build build/release` if options changed (CI checks it).

## 3. Build the files

CI does this on every tag; to try it first:

```bash
python3 scripts/make_release.py --build-viewer     # tarball and PCM zip in build/release-files/
python3 scripts/test_release_install.py build/release-files/tracemaker-*-linux-x86_64.tar.gz \
    build/release-files/tracemaker-*-pcm-linux-x86_64.zip   # clean container, no compiler: routes the example board
```

The **Release files** workflow (`.github/workflows/release.yml`) can also be run by hand (Actions > Release files >
Run workflow); it keeps the files as workflow artifacts and publishes nothing.

## 4. Tag and publish

```bash
git tag -a vX.Y.Z -m "TraceMaker X.Y.Z"
git push origin vX.Y.Z
```

- **Release files** builds the tarball and the PCM zip again, tests them, and attaches them with `SHA256SUMS` to a
  **draft** release for the tag.
- **Docs** publishes this site as version `X.Y` and moves the `latest` alias to it
  ([Write and publish the docs](docs.md#publishing)).

Put the release notes on the draft, read them, and publish it.
