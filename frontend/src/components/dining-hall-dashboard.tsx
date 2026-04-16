import { useGetCurrentCurrentGet } from "@/api/generated/default/default";
import { DiningHallCard } from "@/components/dining-hall-card";
import { ThemeToggle } from "@/components/theme-toggle";
import { InfoDialog } from "@/components/info-dialog";
import { Loader2 } from "lucide-react";

export function DiningHallDashboard() {
  const isDev =
    new URLSearchParams(window.location.search).get("dev") === "true";

  const { data, isLoading, error } = useGetCurrentCurrentGet({
    query: { enabled: isDev },
  });

  if (isDev && isLoading) {
    return (
      <div className="flex min-h-screen items-center justify-center">
        <Loader2 className="h-8 w-8 animate-spin text-muted-foreground" />
      </div>
    );
  }

  if (isDev && error) {
    return (
      <div className="flex min-h-screen items-center justify-center p-4">
        <div className="text-center">
          <h2 className="text-lg font-semibold text-destructive">
            Failed to load dining hall data
          </h2>
          <p className="text-sm text-muted-foreground mt-2">
            Please try refreshing the page
          </p>
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
        {isDev ? (
          <div className="grid gap-4 grid-cols-1 md:grid-cols-2 lg:grid-cols-3 items-start">
            {data?.map((location) => (
              <DiningHallCard key={location.location} location={location} />
            ))}
          </div>
        ) : (
          <div className="flex min-h-[60vh] items-center justify-center">
            <div className="text-center">
              <h2 className="text-xl font-semibold">
                MiddLines is coming back soon
              </h2>
              <p className="text-sm text-muted-foreground mt-2">
                Just wait. We're making it way better.
              </p>
            </div>
          </div>
        )}
      </main>
    </div>
  );
}
