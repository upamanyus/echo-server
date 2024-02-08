package main

import (
	"fmt"
	"math/rand"
	"sync"
	"sync/atomic"
	"time"
	// "runtime"
)

func pow(x float64, n uint64) float64 {
	if n == 0 {
		return 1
	}
	y := 1.0
	for n > 0 {
		if (n % 2) == 1 {
			y *= x
		}
		x = x*x
		n = n/2
	}
	return y;
}

var numIters uint64
var threads sync.WaitGroup
var shouldStart atomic.Bool
var printPi sync.Once

// computes pi using
func oneThread() {
	x := rand.Float64()
	fmt.Println(x)
	// x = 1.0
	a := float64(0)
	for !shouldStart.Load() {
	}

	for n := uint64(0); n < numIters; n++ {
		k := 2*n + 1
		a += pow(-1, n) * pow(x,k)/float64(k)
		// runtime.Gosched()
	}
	threads.Done()
	// printPi.Do(func() {
		fmt.Println(a)
	// })
	// fmt.Println(4*a)
}

func bench(numThreads, numOps uint64) {
	numIters = numOps
	for i := uint64(0); i < numThreads; i++ {
		threads.Add(1)
		go func() {
			oneThread()
		}()
	}

	start := time.Now()
	shouldStart.Store(true)

	threads.Wait()
	runtime := time.Now().Sub(start)
	fmt.Printf("Took %v\n", runtime)
}

func main() {
	bench(6, 10_000_000)
}
