import type { LocationStatus } from "@/api/generated/models";

export function getTrendEmoji(trend: LocationStatus["trend"]): string {
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

export function getTrendLabel(trend: LocationStatus["trend"]): string {
  return trend ?? "Unknown";
}
