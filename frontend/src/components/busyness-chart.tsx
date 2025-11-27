import type { DataPoint } from '@/api/generated/models'
import { ChartContainer, ChartTooltip } from '@/components/ui/chart'
import { LineChart, Line, XAxis, ResponsiveContainer } from 'recharts'
import { filterNonClosedData, formatTimeForChart } from '@/lib/data-filters'
import { useTheme } from '@/components/theme-provider'

interface BusynessChartProps {
  data: DataPoint[]
}

export function BusynessChart({ data }: BusynessChartProps) {
  const { theme } = useTheme()
  const isDark = theme === 'dark' || (theme === 'system' && window.matchMedia('(prefers-color-scheme: dark)').matches)

  // Light mode: darker blue for contrast, Dark mode: lighter blue for visibility
  const lineColor = isDark ? '#60a5fa' : '#2563eb'

  const filteredData = filterNonClosedData(data)

  if (filteredData.length === 0) {
    return (
      <div className="h-64 flex items-center justify-center text-muted-foreground">
        No data available
      </div>
    )
  }

  const chartData = filteredData.map((point) => ({
    time: formatTimeForChart(point.timestamp),
    timestamp: point.timestamp,
    busyness: point.busyness_percentage,
  }))

  return (
    <div className="h-36 sm:h-44 w-full">
      <ChartContainer
        config={{
          busyness: {
            label: 'Busyness',
            color: lineColor,
          },
        }}
      >
        <ResponsiveContainer width="100%" height="100%">
          <LineChart
            data={chartData}
            margin={{ top: 5, right: 5, bottom: 20, left: 5 }}
          >
            <XAxis
              dataKey="time"
              tick={{ fontSize: 11, fill: 'currentColor' }}
              tickLine={false}
              axisLine={false}
              interval="preserveStartEnd"
              className="text-muted-foreground"
              height={20}
            />
            <ChartTooltip
              content={({ active, payload }) => {
                if (!active || !payload?.length) return null
                const data = payload[0]
                const percentage = Math.round(data.value as number)
                const time = data.payload.time
                return (
                  <div className="bg-background border border-border/50 rounded-lg px-3 py-2 shadow-xl">
                    <div className="text-sm font-semibold">
                      {percentage}%
                    </div>
                    <div className="text-xs text-muted-foreground">
                      {time}
                    </div>
                  </div>
                )
              }}
            />
            <Line
              type="monotone"
              dataKey="busyness"
              stroke={lineColor}
              strokeWidth={3}
              dot={false}
              activeDot={{ r: 5 }}
              animationDuration={250}
              isAnimationActive={true}
            />
          </LineChart>
        </ResponsiveContainer>
      </ChartContainer>
    </div>
  )
}
