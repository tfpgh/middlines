import type { DataPoint } from '@/api/generated/models'

/**
 * Minimum busyness threshold to consider a dining hall "open".
 * Values below this are likely baseline/closed readings.
 */
const OPEN_THRESHOLD = 5 // 5% busyness

/**
 * Filters today_data to show only data from when the dining hall opened.
 * Detects opening time by finding the first point with significant busyness.
 */
export function filterNonClosedData(data: DataPoint[]): DataPoint[] {
  // Remove null values
  const nonNullData = data.filter(point => point.busyness_percentage !== null)

  if (nonNullData.length === 0) {
    return []
  }

  // Find the first point where busyness exceeds the threshold (dining hall opened)
  const openingIndex = nonNullData.findIndex(
    point => point.busyness_percentage! >= OPEN_THRESHOLD
  )

  // If no opening found (all values below threshold), return empty
  if (openingIndex === -1) {
    return []
  }

  // Return data from opening time onwards
  return nonNullData.slice(openingIndex)
}

/**
 * Formats timestamp for chart display.
 */
export function formatTimeForChart(timestamp: string): string {
  const date = new Date(timestamp)
  return date.toLocaleTimeString('en-US', {
    hour: 'numeric',
    minute: '2-digit',
    hour12: true
  })
}
