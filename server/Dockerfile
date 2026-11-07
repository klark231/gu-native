FROM golang:1.22-alpine AS build
WORKDIR /src
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /mhgurecomp-hubserver .

FROM scratch
COPY --from=build /mhgurecomp-hubserver /mhgurecomp-hubserver
USER 65534:65534
EXPOSE 39510/tcp 39510/udp
ENTRYPOINT ["/mhgurecomp-hubserver", "-addr", ":39510"]
