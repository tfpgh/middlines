import type { LocationStatusTrend } from "@/api/generated/models";

export function getTrendEmoji(trend: LocationStatusTrend): string {
  switch (trend) {
    case "Increasing":
      return "↗";
    case "Steady":
      return "→";
    case "Decreasing":
      return "↘";
    default:
      return "—";
  }
}

export function getTrendLabel(trend: LocationStatusTrend): string {
  return trend ?? "Unknown";
}
