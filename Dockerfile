# =============================================================================
# NOVA Compiler Studio — production image
# Stage 1: build the React frontend (vite build -> dist/)
# Stage 2: build the native C compiler backend (nova_server)
# Stage 3: runtime — serves dist/ and proxies /api to nova_server
# =============================================================================

# --- Stage 1: frontend build -------------------------------------------------
FROM node:22-alpine AS frontend-build
WORKDIR /app
COPY package*.json ./
RUN npm ci --no-audit --no-fund
COPY index.html vite.config.js tailwind.config.js postcss.config.js ./
COPY public ./public
COPY src ./src
RUN npm run build

# --- Stage 2: native backend build --------------------------------------------
FROM alpine:3.20 AS backend-build
RUN apk add --no-cache gcc musl-dev make
WORKDIR /app/c_backend
COPY c_backend/ ./
RUN make nova_server CFLAGS="-std=c99 -Wall -Wextra -O2 -static"

# --- Stage 3: runtime ----------------------------------------------------------
FROM node:22-alpine
WORKDIR /app
ENV NODE_ENV=production
COPY --from=frontend-build /app/dist ./dist
COPY --from=backend-build /app/c_backend/nova_server ./nova_server
COPY scripts/serve.mjs ./scripts/serve.mjs

ENV PORT=3000
EXPOSE 3000

# Health check hits the front server; the native backend is supervised by it.
HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
  CMD wget -qO- http://127.0.0.1:3000/api/health || exit 1

CMD ["node", "scripts/serve.mjs"]
