FROM golang:1.27-alpine AS build

ARG GOOSE_VERSION=v3.27.3
RUN CGO_ENABLED=0 go install github.com/pressly/goose/v3/cmd/goose@${GOOSE_VERSION}

FROM scratch

COPY --from=build /go/bin/goose /goose
COPY migrations /migrations

ENTRYPOINT ["/goose"]
