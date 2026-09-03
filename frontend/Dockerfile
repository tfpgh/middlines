FROM node:24-trixie-slim AS build

WORKDIR /frontend

RUN npm install -g pnpm@11.25.0

# Copy dependency files first for better caching
COPY package.json pnpm-lock.yaml pnpm-workspace.yaml ./

RUN pnpm install --frozen-lockfile

COPY . .

RUN pnpm run build

FROM nginx:1.29-trixie
COPY --from=build /frontend/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/nginx.conf
