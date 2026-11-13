package main

import (
	"flag"
	"log"
)

func main() {
	cfg := DefaultConfig()
	flag.StringVar(&cfg.Addr, "addr", cfg.Addr, "address and port to listen on (TCP and UDP)")
	flag.IntVar(&cfg.MaxConns, "max-conns", cfg.MaxConns, "connections at once")
	flag.IntVar(&cfg.MaxConnsPerIP, "max-conns-per-ip", cfg.MaxConnsPerIP, "connections at once from one IP address")
	flag.IntVar(&cfg.MaxHubs, "max-hubs", cfg.MaxHubs, "hubs at once")
	flag.IntVar(&cfg.MaxHubsPerIP, "max-hubs-per-ip", cfg.MaxHubsPerIP, "hubs at once from one IP address")
	flag.IntVar(&cfg.ListMax, "list-max", cfg.ListMax, "hubs in one search answer (a random choice when there are more)")
	flag.Float64Var(&cfg.Bandwidth, "bandwidth", cfg.Bandwidth, "bytes a second per player link and direction")
	flag.BoolVar(&cfg.Verbose, "v", false, "log hubs and joins (never IP addresses)")
	flag.Parse()
	s := NewServer(cfg)
	if err := s.Listen(); err != nil {
		log.Fatal(err)
	}
	log.Printf("MHGURecomp hub server on %s (TCP and UDP)", s.Addr())
	log.Fatal(s.Serve())
}
