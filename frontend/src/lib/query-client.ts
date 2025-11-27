import { QueryClient } from "@tanstack/react-query";

export const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 30 * 1000, // 30s (matches API cache)
      refetchInterval: 45 * 1000, // 45s auto-refresh
      refetchOnWindowFocus: true,
      retry: 2,
    },
  },
});
