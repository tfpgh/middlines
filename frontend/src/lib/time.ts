const chartTime = new Intl.DateTimeFormat("en-US", {
  timeZone: "America/New_York",
  hour: "numeric",
  minute: "2-digit",
});

export function formatTimeForChart(timestamp: string | number): string {
  return chartTime.format(new Date(timestamp));
}

export function isReadingStale(timestamp: string | null, now: number): boolean {
  return timestamp !== null && now - Date.parse(timestamp) > 5 * 60 * 1000;
}
