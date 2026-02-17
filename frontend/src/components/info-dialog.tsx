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
            Real-time dining hall busyness tracking for Middlebury College
            created by{" "}
            <a
              href="mailto:tpenner@middlebury.edu"
              className="underline hover:text-foreground"
            >
              Toby Penner
            </a>
            {" and "}
            <a
              href="mailto:cbitter@middlebury.edu"
              className="underline hover:text-foreground"
            >
              Cam Bitter
            </a>
            .
          </DialogDescription>
        </DialogHeader>
        <div className="space-y-4 text-sm">
          <div>
            <h3 className="font-semibold mb-1">How it works</h3>
            <p className="text-muted-foreground">
              We have sensors in each dining hall that monitor the relative
              amount of Bluetooth traffic from students&apos; phones, laptops,
              AirPods, and other devices to estimate how busy things are. We
              don't track any information about these devices, just their
              presence.
            </p>
          </div>
          <div>
            <h3 className="font-semibold mb-1">Feedback/Suggestions</h3>
            <p className="text-muted-foreground">
              We'd love to hear your feedback and suggestions! Feel free to
              email us or fill out{" "}
              <a
                href="https://forms.gle/hPbVUWJaQMqpWdqE8"
                className="underline hover:text-foreground"
              >
                this form
              </a>
              .
            </p>
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
