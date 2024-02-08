package main

import (
	"encoding/binary"
	"log"
	"net"
)

const BufSize = 1024

var msgSize int

func handleConn(conn net.Conn) {
	buf := make([]byte, BufSize)

    // announce the message size
	log.Printf("sending message size")
	binary.BigEndian.PutUint32(buf[:8], uint32(msgSize))
	n, err := conn.Write(buf[:8])
	log.Printf("sent message size")
	if err != nil || uint64(n) < 8 {
		log.Fatalf("ending conn: %v\n", err)
		return
	}

    // receive the 2 byte initial value from the client.
	n, err = conn.Read(buf[:2])
	if err != nil || uint64(n) < 2 {
		log.Fatalf("ending conn: %v\n", err)
		return
	}

    // echo loop
	for {
		n, err = conn.Read(buf[:msgSize])
		if err != nil || n < msgSize {
			log.Printf("ending conn: read %d/%d bytes, %v\n", n, msgSize, err)
			return
		}
		n, err = conn.Write(buf[:msgSize])
		if err != nil || n < msgSize {
			panic("x")
			log.Printf("ending conn: wrote %d/%d bytes, %v\n", n, msgSize, err)
			return
		}
	}
}

func startServer() {
	l, err := net.Listen("tcp", ":12345")
	if err != nil {
		panic(err)
	}
	for {
		conn, err := l.Accept()
		if err != nil {
			panic(err)
		}
		go handleConn(conn)
	}
}

func main() {
	msgSize = 64
	startServer();
}
