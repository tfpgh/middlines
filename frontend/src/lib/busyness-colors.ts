/**
 * Returns border color and width classes based on busyness percentage.
 * Color scale with more granular variation:
 * 0-20%: Light green
 * 20-40%: Medium green
 * 40-60%: Dark green
 * 60-70%: Yellow-green
 * 70-80%: Yellow-orange
 * 80-90%: Orange-red
 * 90-100%: Dark red
 */
export function getBusynessBorderClasses(busyness: number | null): string {
  if (busyness === null) {
    return 'border-2 border-muted'
  }

  const clamped = Math.max(0, Math.min(100, busyness))

  // Green spectrum (0-60%)
  if (clamped < 20) return 'border-2 border-green-300'      // Light green
  if (clamped < 40) return 'border-2 border-green-400'      // Medium green
  if (clamped < 60) return 'border-3 border-green-600'      // Dark green

  // Yellow spectrum (60-80%)
  if (clamped < 70) return 'border-3 border-lime-500'       // Yellow-green
  if (clamped < 80) return 'border-3 border-yellow-500'     // Yellow-orange

  // Red spectrum (80-100%)
  if (clamped < 90) return 'border-4 border-orange-600'     // Orange-red
  return 'border-4 border-red-600'                          // Dark red
}
