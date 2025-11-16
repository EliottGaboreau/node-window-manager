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
  return `  #${rank} [${zLabel}${visLabel}] "${safeTitle}" x=${x} y=${y} w=${width} h=${height}${exeLabel}`
}

let previousState = []
let updateCount = 0
let lastUpdateTime = process.hrtime.bigint()

function compareStates(current, previous) {
  const changes = []
  
  // Create maps for easier comparison
  const prevMap = new Map(previous.map(w => [w.id, w]))
  const currMap = new Map(current.map(w => [w.id, w]))
  
  // Check for new windows
  for (const win of current) {
    if (!prevMap.has(win.id)) {
      changes.push({ type: 'created', window: win })
    }
  }
  
  // Check for closed windows
  for (const win of previous) {
    if (!currMap.has(win.id)) {
      changes.push({ type: 'destroyed', window: win })
    }
  }
  
  // Check for moved/resized/reordered windows
  for (const win of current) {
    const prev = prevMap.get(win.id)
    if (prev) {
      if (prev.zOrder !== win.zOrder) {
        changes.push({ type: 'reordered', window: win, oldZ: prev.zOrder, newZ: win.zOrder })
      }
      if (prev.bounds.x !== win.bounds.x || prev.bounds.y !== win.bounds.y) {
        changes.push({ type: 'moved', window: win, oldBounds: prev.bounds, newBounds: win.bounds })
      }
      if (prev.bounds.width !== win.bounds.width || prev.bounds.height !== win.bounds.height) {
        changes.push({ type: 'resized', window: win, oldBounds: prev.bounds, newBounds: win.bounds })
      }
      if (prev.isVisible !== win.isVisible) {
        changes.push({ type: 'visibility', window: win, visible: win.isVisible })
      }
    }
  }
  
  return changes
}

function handleWindowsUpdate(summaries) {
  const now = process.hrtime.bigint()
  const timeSinceLastUpdate = getElapsedMs(lastUpdateTime)
  
  // Detect what changed
  const changes = compareStates(summaries, previousState)
  
  // Skip updates with no changes
  if (changes.length === 0) {
    previousState = summaries
    return
  }
  
  updateCount++
  lastUpdateTime = now
  
  console.log(`\n${"=".repeat(80)}`)
  console.log(`Update #${updateCount} (${timeSinceLastUpdate.toFixed(2)}ms since last update)`)
  console.log(`${"=".repeat(80)}`)
  
  console.log(`\nDetected ${changes.length} change(s):`)
  for (const change of changes) {
    const w = change.window
    const title = w.title || "(no title)"
    const exeName = w.path ? path.basename(w.path) : ""
    
    switch (change.type) {
      case 'created':
        console.log(`  + NEW: "${title}" (${exeName}) - Z=${w.zOrder}`)
        break
      case 'destroyed':
        console.log(`  - CLOSED: "${title}" (${exeName})`)
        break
      case 'reordered':
        console.log(`  ↕ REORDERED: "${title}" - Z: ${change.oldZ} → ${change.newZ}`)
        break
      case 'moved':
        console.log(`  → MOVED: "${title}" - from (${change.oldBounds.x},${change.oldBounds.y}) to (${change.newBounds.x},${change.newBounds.y})`)
        break
      case 'resized':
        console.log(`  ⇔ RESIZED: "${title}" - ${change.oldBounds.width}x${change.oldBounds.height} → ${change.newBounds.width}x${change.newBounds.height}`)
        break
      case 'visibility':
        console.log(`  👁 VISIBILITY: "${title}" - visible: ${change.visible}`)
        break
    }
  }
  
  // Filter visible windows only and sort by Z-order
  const visibleWindows = summaries.filter(w => w.isVisible)
  const sorted = [...visibleWindows].sort((a, b) => {
    const az = a.zOrder >= 0 ? a.zOrder : Number.MAX_SAFE_INTEGER
    const bz = b.zOrder >= 0 ? b.zOrder : Number.MAX_SAFE_INTEGER
    return az - bz
  })
  
  console.log(`\nCurrent visible window state (${sorted.length} windows):`)
  sorted.slice(0, 10).forEach((w, idx) => {
    const exeName = w.path ? path.basename(w.path) : ""
    console.log(formatWindowLine(idx + 1, w.zOrder, w.title, w.bounds, exeName, w.isVisible))
  })
  
  if (sorted.length > 10) {
    console.log(`  ... and ${sorted.length - 10} more windows`)
  }
  
  previousState = summaries
}

async function main() {
  console.log("Starting window monitoring...")
  console.log("Press Ctrl+C to stop\n")
  
  // Request permissions if needed
  if (process.platform === 'darwin') {
    const hasAccessibility = windowManager.requestAccessibility()
    const hasScreenCapture = windowManager.requestScreenCapture()
    
    if (!hasAccessibility) {
      console.log("⚠️  Accessibility permission required on macOS")
      console.log("   Please grant permission in System Preferences > Security & Privacy > Privacy > Accessibility")
    }
    if (!hasScreenCapture) {
      console.log("⚠️  Screen Recording permission may be required on macOS 10.15+")
      console.log("   Please grant permission in System Preferences > Security & Privacy > Privacy > Screen Recording")
    }
    console.log()
  }
  
  // Register event listener
  windowManager.on('windows-summary-updated', handleWindowsUpdate)
  
  console.log("Monitoring started. Waiting for window changes...")
  
  // Handle Ctrl+C gracefully
  process.on('SIGINT', () => {
    console.log("\n\nStopping window monitoring...")
    windowManager.removeAllListeners('windows-summary-updated')
    console.log(`Total updates received: ${updateCount}`)
    process.exit(0)
  })
  
  // Keep the process alive
  await new Promise(() => {})
}

main().catch(err => {
  console.error("Failed to monitor windows:", err)
  process.exitCode = 1
})

