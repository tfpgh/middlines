import { defineConfig } from "orval";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const apiDirectory = fileURLToPath(new URL("../services/api", import.meta.url));

export default defineConfig({
  middlines: {
    input: {
      target: JSON.parse(
        execFileSync(
          "uv",
          [
            "run",
            "--project",
            apiDirectory,
            "python",
            "-c",
            "import json; from main import app; print(json.dumps(app.openapi()))",
          ],
          { cwd: apiDirectory, encoding: "utf8" },
        ),
      ),
      filters: {
        tags: ["dashboard"],
        schemas: ["LocationStatus", "DataPoint", "Status", "Trend"],
      },
    },
    output: {
      target: "./src/api/generated/endpoints.ts",
      schemas: "./src/api/generated/models",
      client: "react-query",
      mode: "tags-split",
      clean: true,
      override: {
        mutator: {
          path: "./src/api/client.ts",
          name: "customInstance",
        },
      },
    },
  },
});
