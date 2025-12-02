import { useState } from "react";
import type { LocationStatus } from "@/api/generated/models";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { getBusynessBorderClasses } from "@/lib/busyness-colors";
import { getTrendEmoji, getTrendLabel } from "@/lib/trend-helpers";
import { BusynessChart } from "@/components/busyness-chart";
import { ChevronDown, ChevronUp } from "lucide-react";

interface DiningHallCardProps {
  location: LocationStatus;
}

export function DiningHallCard({ location }: DiningHallCardProps) {
  const [isExpanded, setIsExpanded] = useState(false);

  const {
    location: name,
    busyness_percentage,
    vs_typical_percentage,
    trend,
    today_data,
  } = location;

  const borderClasses = getBusynessBorderClasses(busyness_percentage);
  const trendEmoji = getTrendEmoji(trend);
  const trendLabel = getTrendLabel(trend);
  const isClosed = busyness_percentage === null;
  const hasHistoricalData = today_data.some(d => d.busyness_percentage !== null);
  const isInteractive = !isClosed || hasHistoricalData;

  return (
    <Card
      className={`${borderClasses} transition-all duration-300 ease-in-out ${isInteractive ? 'cursor-pointer hover:shadow-lg' : ''}`}
      onClick={() => isInteractive && setIsExpanded(!isExpanded)}
    >
      <CardHeader>
        <div className="flex items-center justify-between">
          <CardTitle className="text-xl">{name}</CardTitle>
          {isInteractive && (
            isExpanded ? (
              <ChevronUp className="h-5 w-5 text-muted-foreground transition-transform" />
            ) : (
              <ChevronDown className="h-5 w-5 text-muted-foreground transition-transform" />
            )
          )}
        </div>
      </CardHeader>

      <CardContent>
        <div className="space-y-3">
          {/* Busyness percentage display */}
          <div className="text-center">
            {isClosed ? (
              <div className="text-4xl font-bold text-muted-foreground">
                Closed
              </div>
            ) : (
              <div className="text-6xl font-bold tabular-nums">
                {Math.round(busyness_percentage!)}%
              </div>
            )}
          </div>

          {/* Vs Typical & Trend Row */}
          {!isClosed && (
            <div className="flex items-center justify-between text-sm">
              {/* Vs Typical Badge */}
              <div>
                {vs_typical_percentage !== null && (
                  <span
                    className={`inline-flex items-center px-2.5 py-0.5 rounded-full font-medium ${
                      vs_typical_percentage >= 5
                        ? "bg-red-100 text-red-800 dark:bg-red-900 dark:text-red-200"
                        : vs_typical_percentage <= -5
                          ? "bg-green-100 text-green-800 dark:bg-green-900 dark:text-green-200"
                          : "bg-gray-100 text-gray-800 dark:bg-gray-800 dark:text-gray-200"
                    }`}
                  >
                    {vs_typical_percentage > 0 ? "+" : ""}
                    {Math.round(vs_typical_percentage)}% vs typical
                  </span>
                )}
              </div>

              {/* Trend Indicator */}
              <div className="flex items-center gap-1 text-muted-foreground">
                <span className="text-lg" aria-label={trendLabel}>
                  {trendEmoji}
                </span>
                <span className="text-xs uppercase tracking-wider">
                  {trendLabel}
                </span>
              </div>
            </div>
          )}
        </div>

        {/* Expanded Chart */}
        {isExpanded && hasHistoricalData && (
          <div className="-mx-2 pt-5 mt-4 border-t overflow-hidden">
            {isClosed && (
              <p className="text-sm text-muted-foreground mb-3 px-2">Today's Activity</p>
            )}
            <BusynessChart data={today_data} />
          </div>
        )}

        {/* Empty state when expanded but no data */}
        {isExpanded && !hasHistoricalData && (
          <div className="pt-5 mt-4 pb-2 border-t text-center text-sm text-muted-foreground">
            No activity data available yet
          </div>
        )}
      </CardContent>
    </Card>
  );
}
