import { windowManager } from "../dist/index.js"
import path from "node:path"

function getElapsedMs(start) {
  const diff = process.hrtime.bigint() - start
  return Number(diff) / 1e6
}

function formatWindowLine(rank, z, title, bounds, exeName, isVisible) {
  const safeTitle = title ?? ""
  const { x = 0, y = 0, width = 0, height = 0 } = bounds || {}
  const zLabel = Number.isInteger(z) && z >= 0 ? `Z=${z}` : "Z=unknown"
  const visLabel = isVisible !== undefined ? ` vis=${isVisible}` : ""
  const exeLabel = exeName ? ` exe=${exeName}` : ""
  return `#${rank} [${zLabel}${visLabel}] "${safeTitle}" x=${x} y=${y} w=${width} h=${height}${exeLabel}`
}

async function main() {
  const start = process.hrtime.bigint()

  const summaries = windowManager.getWindowsSummary()

  const info = summaries.map(s => ({
    z: s.zOrder,
    title: s.title,
    bounds: s.bounds,
    exeName: s.path ? path.basename(s.path) : "",
    isVisible: s.isVisible
  }))

  info.sort((a, b) => {
    const az = a.z >= 0 ? a.z : Number.MAX_SAFE_INTEGER
    const bz = b.z >= 0 ? b.z : Number.MAX_SAFE_INTEGER
    return az - bz
  })

  info.forEach((w, idx) => {
    console.log(formatWindowLine(idx + 1, w.z, w.title, w.bounds, w.exeName, w.isVisible))
  })

  const totalMs = getElapsedMs(start).toFixed(2)
  console.log(`Total time: ${totalMs} ms`)
}

main().catch(err => {
  console.error("Failed to list windows:", err)
  process.exitCode = 1
})


