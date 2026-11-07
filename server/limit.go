package main

import (
	"sync"
	"time"
)

// bucket is a token bucket: rate tokens a second, at most burst saved up.
type bucket struct {
	mu          sync.Mutex
	rate, burst float64
	tokens      float64
	last        time.Time
}

func newBucket(rate, burst float64) *bucket {
	return &bucket{rate: rate, burst: burst, tokens: burst, last: time.Now()}
}

func (b *bucket) refill(now time.Time) {
	b.tokens += now.Sub(b.last).Seconds() * b.rate
	if b.tokens > b.burst {
		b.tokens = b.burst
	}
	b.last = now
}

// take spends n tokens if there are that many.
func (b *bucket) take(n float64) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refill(time.Now())
	if b.tokens < n {
		return false
	}
	b.tokens -= n
	return true
}

// wait spends n tokens and returns how long to wait before they are really there (a debt is paid by waiting).
func (b *bucket) wait(n float64) time.Duration {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refill(time.Now())
	b.tokens -= n
	if b.tokens >= 0 {
		return 0
	}
	return time.Duration(-b.tokens / b.rate * float64(time.Second))
}
