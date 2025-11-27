import type { DataPoint } from '@/api/generated/models'
import { ChartContainer, ChartTooltip, ChartTooltipContent } from '@/components/ui/chart'
import { LineChart, Line, XAxis, YAxis, CartesianGrid, ResponsiveContainer } from 'recharts'
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
    <div className="h-40 w-full -mx-2 sm:mx-0 sm:h-48">
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
            margin={{ top: 5, right: 0, bottom: 0, left: -10 }}
          >
            <CartesianGrid strokeDasharray="3 3" className="stroke-muted" />
            <XAxis
              dataKey="time"
              tick={{ fontSize: 12 }}
              interval="preserveStartEnd"
              className="text-muted-foreground"
            />
            <YAxis
              domain={[0, 100]}
              tick={{ fontSize: 12 }}
              className="text-muted-foreground"
            />
            <ChartTooltip content={<ChartTooltipContent />} />
            <Line
              type="monotone"
              dataKey="busyness"
              stroke={lineColor}
              strokeWidth={3}
              dot={false}
              activeDot={{ r: 5 }}
              animationDuration={300}
              isAnimationActive={true}
            />
          </LineChart>
        </ResponsiveContainer>
      </ChartContainer>
    </div>
  )
}
