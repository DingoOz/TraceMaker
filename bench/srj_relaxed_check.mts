// SPDX-License-Identifier: GPL-3.0-or-later
// Runs tscircuit's own "relaxed DRC" (evaluateRelaxedDrc in tscircuit-autorouter, the check behind the pass rate
// on its benchmark dashboard) on solved Simple Route JSON files. Called by bench/srj_score.py:
//   tsx --tsconfig <autorouter>/tsconfig.json bench/srj_relaxed_check.mts <autorouter> <list.json> <out.jsonl>
// list.json: [{"name": ..., "solved": path}]. One JSON line per file: name, error count, errors by type.
import { readFileSync, writeFileSync } from "node:fs"
import { pathToFileURL } from "node:url"
import { join } from "node:path"

const [autorouter, listPath, outPath] = process.argv.slice(2)
const { evaluateRelaxedDrc } = await import(pathToFileURL(join(autorouter, "lib/testing/evaluate-relaxed-drc.ts")).href)

const lines: string[] = []
for (const item of JSON.parse(readFileSync(listPath, "utf8")) as Array<{ name: string; solved: string }>) {
  try {
    const { traces, ...problem } = JSON.parse(readFileSync(item.solved, "utf8"))
    // As tscircuit's benchmark calls it (scripts/benchmark/benchmark-run-task.ts): the problem as given, the
    // routed traces, default options (0.1 mm trace and via clearance).
    const { errors } = evaluateRelaxedDrc({ inputSrj: problem, srjWithPointPairs: problem, routedTraces: traces ?? [] })
    const types: Record<string, number> = {}
    const messages: string[] = []
    for (const e of errors as Array<Record<string, unknown>>) {
      const type = String(e.error_type ?? e.type ?? "unknown")
      types[type] = (types[type] ?? 0) + 1
      if (messages.length < 3) messages.push(String(e.message ?? ""))
    }
    lines.push(JSON.stringify({ name: item.name, errors: errors.length, types, messages }))
  } catch (e) {
    lines.push(JSON.stringify({ name: item.name, failed: e instanceof Error ? e.message : String(e) }))
  }
}
writeFileSync(outPath, lines.join("\n") + "\n")
