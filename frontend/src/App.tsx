import { ThemeProvider } from '@/components/theme-provider'
import { DiningHallDashboard } from '@/components/dining-hall-dashboard'

function App() {
  return (
    <ThemeProvider defaultTheme="system" storageKey="middlines-theme">
      <DiningHallDashboard />
    </ThemeProvider>
  )
}

export default App
