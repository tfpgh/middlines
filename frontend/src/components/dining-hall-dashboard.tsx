import { useEffect, useState } from "react";
import { useGetCurrentCurrentGet } from "@/api/generated/dashboard/dashboard";
import { DiningHallCard } from "@/components/dining-hall-card";
import { ThemeToggle } from "@/components/theme-toggle";
import { InfoDialog } from "@/components/info-dialog";
import { Loader2 } from "lucide-react";

export function DiningHallDashboard() {
  const { data, isLoading, error, refetch } = useGetCurrentCurrentGet();
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 30_000);
    return () => window.clearInterval(timer);
  }, []);

  if (isLoading) {
    return (
      <div className="flex min-h-screen items-center justify-center">
        <Loader2 className="h-8 w-8 animate-spin text-muted-foreground" />
      </div>
    );
  }

  if (error && !data) {
    return (
      <div className="flex min-h-screen items-center justify-center p-4">
        <div className="text-center">
          <h2 className="text-lg font-semibold text-destructive">
            Failed to load dining hall data
          </h2>
          <button
            className="mt-2 text-sm underline"
            onClick={() => void refetch()}
          >
            Try again
          </button>
        </div>
      </div>
    );
  }

  return (
    <div className="min-h-screen bg-background">
      <header className="border-b">
        <div className="container mx-auto px-4 py-6 flex items-center justify-between">
          <h1 className="text-2xl font-bold md:text-3xl">MiddLines</h1>
          <div className="flex items-center gap-2">
            <InfoDialog />
            <ThemeToggle />
          </div>
        </div>
      </header>

      <main className="container mx-auto px-4 py-6">
        {Boolean(error) && (
          <p role="status" className="mb-4 text-sm text-destructive">
            Couldn't refresh. Showing the last available readings.{" "}
            <button className="underline" onClick={() => void refetch()}>
              Try again
            </button>
          </p>
        )}
        <div className="grid gap-4 grid-cols-1 md:grid-cols-2 lg:grid-cols-3 items-start">
          {data?.map((location) => (
            <DiningHallCard
              key={location.location}
              location={location}
              now={now}
            />
          ))}
        </div>
      </main>
    </div>
  );
}
