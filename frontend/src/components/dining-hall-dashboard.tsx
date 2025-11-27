import { useGetCurrentCurrentGet } from "@/api/generated/default/default";
import { DiningHallCard } from "@/components/dining-hall-card";
import { ThemeToggle } from "@/components/theme-toggle";
import { InfoDialog } from "@/components/info-dialog";
import { Loader2 } from "lucide-react";

export function DiningHallDashboard() {
  const { data, isLoading, error } = useGetCurrentCurrentGet();

  if (isLoading) {
    return (
      <div className="flex min-h-screen items-center justify-center">
        <Loader2 className="h-8 w-8 animate-spin text-muted-foreground" />
      </div>
    );
  }

  if (error) {
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
        <div className="grid gap-4 grid-cols-1 md:grid-cols-2 lg:grid-cols-3">
          {data?.map((location) => (
            <DiningHallCard key={location.location} location={location} />
          ))}
        </div>
      </main>
    </div>
  );
}
