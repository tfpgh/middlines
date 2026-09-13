import type { DataPoint } from "@/api/generated/models";
import {
  LineChart,
  Line,
  XAxis,
  YAxis,
  ResponsiveContainer,
  Tooltip,
} from "recharts";
import { formatTimeForChart } from "@/lib/time";

export function BusynessChart({ data }: { data: DataPoint[] }) {
  const first = data.findIndex((point) => point.busyness_percentage !== null);
  if (first === -1)
    return (
      <p className="text-sm text-muted-foreground">
        No activity data available yet
      </p>
    );

  // Trim only leading empty time; retain gaps within the day's activity.
  const points = data.slice(first).map((point) => ({
    time: Date.parse(point.timestamp),
    busyness: point.busyness_percentage,
  }));

  return (
    <div
      className="h-36 sm:h-44 w-full"
      role="img"
      aria-label="Today's smoothed busyness in Eastern time; gaps indicate unavailable or closed periods"
    >
      <ResponsiveContainer width="100%" height="100%">
        <LineChart
          data={points}
          margin={{ top: 5, right: 5, bottom: 20, left: 5 }}
        >
          <XAxis
            dataKey="time"
            type="number"
            scale="time"
            domain={["dataMin", "dataMax"]}
            tickFormatter={formatTimeForChart}
            tick={{ fontSize: 11, fill: "currentColor" }}
            tickLine={false}
            axisLine={false}
            interval="preserveStartEnd"
            height={20}
          />
          <YAxis hide domain={[0, 100]} />
          <Tooltip
            content={({ active, payload, label }) => {
              const value = payload?.[0]?.value;
              if (!active || typeof value !== "number" || label === undefined)
                return null;
              return (
                <div className="bg-background border border-border/50 rounded-lg px-3 py-2 shadow-xl">
                  <div className="text-sm font-semibold">
                    {Math.round(value)}%
                  </div>
                  <div className="text-xs text-muted-foreground">
                    {formatTimeForChart(Number(label))} ET
                  </div>
                </div>
              );
            }}
          />
          <Line
            type="monotone"
            dataKey="busyness"
            stroke="currentColor"
            className="text-blue-600 dark:text-blue-400"
            strokeWidth={3}
            dot={false}
            activeDot={{ r: 5 }}
            connectNulls={false}
            isAnimationActive={false}
          />
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}
