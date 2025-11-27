import { defineConfig } from "orval";

export default defineConfig({
  middlines: {
    input: {
      target: "http://localhost:8000/api/openapi.json",
    },
    output: {
      target: "./src/api/generated/endpoints.ts",
      schemas: "./src/api/generated/models",
      client: "react-query",
      mode: "tags-split",
      override: {
        mutator: {
          path: "./src/api/client.ts",
          name: "customInstance",
        },
      },
    },
  },
});
