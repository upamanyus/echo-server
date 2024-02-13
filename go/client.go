package main

import (
	"encoding/binary"
	"log"
	"math/rand"
	"net"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

var msgSize uint32
var address string

func oneOperation(buf []byte, conn net.Conn) {
	rand.Read(buf)

	n, err := conn.Write(buf[0:msgSize])
	if err != nil || uint64(n) < uint64(msgSize) {
		panic(err)
	}

	n, err = conn.Read(buf[0:msgSize])
	if err != nil || uint64(n) < uint64(msgSize) {
		panic(err)
	}
}

var started sync.WaitGroup
var shouldMeasure atomic.Bool
var shouldReport atomic.Bool
var reported sync.WaitGroup

type clientArgs struct {
	numOps  uint64
	runtime uint64
}

func client(args *clientArgs) {
	conn, err := net.Dial("tcp", address+":12345")
	if err != nil {
		panic(err)
	}
	buf := make([]byte, 1024)

	n, err := conn.Write([]byte("Hi"))
	if err != nil || n < 2 {
		panic(err)
	}

	n, err = conn.Read(buf[0:4])
	if err != nil || n < 4 {
		panic(err)
	}
	msgSize = binary.BigEndian.Uint32(buf[0:4])

	started.Done()
	for {
		if shouldMeasure.Load() {
			break
		}
		oneOperation(buf, conn)
	}

	start := time.Now()
	for {
		if shouldReport.Load() {
			break
		}
		oneOperation(buf, conn)
		args.numOps += 1
	}
	args.runtime = uint64(time.Now().Sub(start).Nanoseconds())
	reported.Done()
	for {
		oneOperation(buf, conn)
	}
}

func bench(N, warmupSec, measureSec int) {
	args := make([]clientArgs, N)
	for i := uint64(0); i < uint64(N); i++ {
		started.Add(1)
		reported.Add(1)
		go client(&args[i])
	}

	started.Wait()

	log.Println("warming up")
	time.Sleep(time.Duration(warmupSec) * time.Second)
	shouldMeasure.Store(true)
	log.Println("measuring")
	time.Sleep(time.Duration(measureSec) * time.Second)
	shouldReport.Store(true)

	reported.Wait()

	numOps := uint64(0)
	throughput := uint64(0)
	for i := 0; i < N; i++ {
		numOps += args[i].numOps
		throughput += ((args[i].numOps * 1000000000) + (args[i].runtime / 2)) / args[i].runtime
	}
	log.Printf("%d total ops\n", numOps)
    log.Printf("%d ops/sec across %d threads\n", throughput, N);
}

func main() {
	if len(os.Args) < 3 {
		log.Println("must provide num_threads as first argument, and server address as second argument")
		return
	}
	N, err := strconv.Atoi(os.Args[1])
	if err != nil {
		panic(err)
	}
	address = os.Args[2]
	bench(N, 5, 5)
}
