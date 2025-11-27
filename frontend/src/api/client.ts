export type ErrorType<Error> = Error;

export type BodyType<BodyData> = BodyData;

export const customInstance = <T>(
  config: {
    url: string;
    method: "GET" | "POST" | "PUT" | "DELETE" | "PATCH";
    params?: Record<string, unknown>;
    data?: BodyType<unknown>;
    headers?: HeadersInit;
    signal?: AbortSignal;
  },
  options?: RequestInit,
): Promise<T> => {
  const { url, method, params, data, headers, signal } = config;

  // Build query string from params
  const queryString = params
    ? "?" +
      Object.entries(params)
        .filter(([, value]) => value !== undefined && value !== null)
        .map(
          ([key, value]) =>
            `${encodeURIComponent(key)}=${encodeURIComponent(String(value))}`,
        )
        .join("&")
    : "";

  const fullUrl = `/api${url}${queryString}`;

  return fetch(fullUrl, {
    method,
    headers: {
      "Content-Type": "application/json",
      ...headers,
    },
    body: data ? JSON.stringify(data) : undefined,
    signal,
    ...options,
  }).then(async (response) => {
    if (!response.ok) {
      const error = await response.json().catch(() => ({
        message: `HTTP Error: ${response.status} ${response.statusText}`,
      }));
      throw error;
    }

    // Handle empty responses
    const text = await response.text();
    if (!text) return {} as T;

    return JSON.parse(text) as T;
  });
};

export default customInstance;
