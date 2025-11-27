import { Info } from "lucide-react";
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from "@/components/ui/dialog";

export function InfoDialog() {
  return (
    <Dialog>
      <DialogTrigger asChild>
        <button
          className="rounded-md p-2 hover:bg-accent transition-colors"
          aria-label="Information"
        >
          <Info className="h-5 w-5" />
        </button>
      </DialogTrigger>
      <DialogContent>
        <DialogHeader>
          <DialogTitle>About MiddLines</DialogTitle>
          <DialogDescription>
            Real-time dining hall line tracking for Middlebury College.
          </DialogDescription>
        </DialogHeader>
        <div className="space-y-4 text-sm">
          <div>
            <h3 className="font-semibold mb-1">How it works</h3>
            <p className="text-muted-foreground">
              ESP32 sensors count people entering and exiting each dining hall.
              Data updates automatically every 45 seconds.
            </p>
          </div>
          <div>
            <h3 className="font-semibold mb-1">Border colors</h3>
            <ul className="text-muted-foreground space-y-1">
              <li>🟢 Green shades: Low busyness (0-60%)</li>
              <li>🟡 Yellow/Lime: Medium busyness (60-80%)</li>
              <li>🟠 Orange/Red: High busyness (80-100%)</li>
            </ul>
          </div>
          <div>
            <h3 className="font-semibold mb-1">Trends</h3>
            <p className="text-muted-foreground">
              ↗ Increasing • → Steady • ↘ Decreasing
            </p>
          </div>
          <div>
            <h3 className="font-semibold mb-1">vs Typical</h3>
            <p className="text-muted-foreground">
              Compares current busyness to historical averages for this time and
              day.
            </p>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
