import { QueryClient } from "@tanstack/react-query";

export const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 30 * 1000,
      refetchInterval: 60 * 1000, // 60s auto-refresh
      refetchOnWindowFocus: true,
      retry: 2,
    },
  },
});
