# TraceMaker KiCad plugin (IPC API, KiCad 10)

Copy this folder to `${KICAD_DOCUMENTS_HOME}/10.0/plugins/tracemaker/` (KiCad creates a virtual environment from
`requirements.txt`), enable the API server in Preferences > Plugins if needed, and set `TRACEMAKER` to the
`tracemaker` binary. The action routes the open board and adds the result as one undoable commit.

Status: the item construction is tested offline against kicad-python 0.8 (`test_offline.py`); the full round trip
needs a running KiCad GUI (KiCad 10 has no headless IPC server) and has not been exercised yet.
