import { lazy, Suspense, useId, useState } from "react";
import type { LocationStatus } from "@/api/generated/models";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { getBusynessBorderClasses } from "@/lib/busyness-colors";
import { getTrendEmoji, getTrendLabel } from "@/lib/trend-helpers";
import { formatReadingTime, isReadingStale } from "@/lib/time";
import { ChevronDown, ChevronUp } from "lucide-react";

const BusynessChart = lazy(() =>
  import("@/components/busyness-chart").then((module) => ({
    default: module.BusynessChart,
  })),
);

interface DiningHallCardProps {
  location: LocationStatus;
  now: number;
}

export function DiningHallCard({ location, now }: DiningHallCardProps) {
  const [isExpanded, setIsExpanded] = useState(false);
  const chartId = useId();
  const {
    busyness_percentage,
    vs_typical_percentage,
    trend,
    today_data,
    timestamp,
  } = location;
  const status = isReadingStale(timestamp, now) ? "stale" : location.status;
  const percentage = status === "active" ? busyness_percentage : null;
  const hasHistory = today_data.some(
    (point) => point.busyness_percentage !== null,
  );
  const trendLabel = getTrendLabel(trend);
  const statusText =
    status === "closed"
      ? "Closed"
      : status === "stale"
        ? "Data delayed"
        : "Unavailable";
  const comparisonClasses =
    vs_typical_percentage !== null && vs_typical_percentage >= 5
      ? "bg-red-100 text-red-800 dark:bg-red-900 dark:text-red-200"
      : vs_typical_percentage !== null && vs_typical_percentage <= -5
        ? "bg-green-100 text-green-800 dark:bg-green-900 dark:text-green-200"
        : "bg-gray-100 text-gray-800 dark:bg-gray-800 dark:text-gray-200";

  return (
    <Card
      className={getBusynessBorderClasses(percentage) + " transition-colors"}
    >
      <CardHeader>
        <button
          type="button"
          className="flex w-full items-center justify-between text-left rounded-sm focus-visible:outline-2 focus-visible:outline-offset-4"
          disabled={!hasHistory}
          aria-expanded={isExpanded}
          aria-controls={chartId}
          onClick={() => setIsExpanded((expanded) => !expanded)}
        >
          <CardTitle className="text-xl">{location.location}</CardTitle>
          {hasHistory &&
            (isExpanded ? (
              <ChevronUp className="h-5 w-5" />
            ) : (
              <ChevronDown className="h-5 w-5" />
            ))}
        </button>
      </CardHeader>
      <CardContent>
        <div className="space-y-3">
          <div className="text-center">
            {percentage !== null ? (
              <div className="text-6xl font-bold tabular-nums">
                {Math.round(percentage)}%
              </div>
            ) : (
              <div className="text-4xl font-bold text-muted-foreground">
                {statusText}
              </div>
            )}
          </div>
          {percentage !== null && (
            <div className="flex items-center justify-between text-sm">
              <div>
                {vs_typical_percentage !== null && (
                  <span
                    className={
                      "inline-flex items-center px-2.5 py-0.5 rounded-full font-medium " +
                      comparisonClasses
                    }
                  >
                    {vs_typical_percentage > 0 ? "+" : ""}
                    {Math.round(vs_typical_percentage)}% vs typical
                  </span>
                )}
              </div>
              {trend !== null && (
                <div
                  className="flex items-center gap-1 text-muted-foreground"
                  title="Change over the last five minutes"
                >
                  <span className="text-lg" aria-label={trendLabel}>
                    {getTrendEmoji(trend)}
                  </span>
                  <span className="text-xs uppercase tracking-wider">
                    {trendLabel}
                  </span>
                </div>
              )}
            </div>
          )}
          <p className="text-center text-xs text-muted-foreground">
            {timestamp ? (
              <>
                Last reading{" "}
                <time dateTime={timestamp}>{formatReadingTime(timestamp)}</time>
              </>
            ) : (
              "No readings available yet"
            )}
          </p>
        </div>
        <div id={chartId} hidden={!isExpanded}>
          {isExpanded && hasHistory && (
            <div className="-mx-2 pt-5 mt-4 border-t overflow-hidden">
              <p className="text-sm text-muted-foreground mb-3 px-2">
                Today's activity
              </p>
              <Suspense
                fallback={
                  <div className="h-36 sm:h-44 text-sm text-muted-foreground">
                    Loading chart…
                  </div>
                }
              >
                <BusynessChart data={today_data} />
              </Suspense>
            </div>
          )}
        </div>
      </CardContent>
    </Card>
  );
}
