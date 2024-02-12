#!/usr/bin/env python3

import paramiko
import redis
import sys
import time

def start_redis(addr):
    cl = paramiko.SSHClient()
    cl.load_system_host_keys()
    cl.connect(addr)
    stdin, stdout, stderr = cl.exec_command("killall redis-server")
    stdout.channel.recv_exit_status()
    stdin, stdout, stderr = cl.exec_command('nohup redis-server --save "" --appendonly no &')
    stdout.channel.recv_exit_status()

def start_client(addr, path_to_client, redis_address):
    cl = paramiko.SSHClient()
    cl.load_system_host_keys()
    cl.connect(addr)
    stdin, stdout, stderr = cl.exec_command("killall multiclient")
    stdout.channel.recv_exit_status()
    stdin, stdout, stderr = cl.exec_command(f"nohup {path_to_client} {redis_address} > /tmp/client.out &")
    stdout.channel.recv_exit_status()

class BenchmarkParams:
    def __init__(self):
        self.num_threads = None
        self.server_address = None
        self.server_port = None
        self.warmup = None
        self.measure = None

def write_benchmark_params_to_redis(redis_address, params):
    r = redis.Redis(host=redis_address, decode_responses=True)
    r.set("numclients", str(params.num_clients))
    r.set("numthreads", str(params.num_threads))
    r.set("address", str(params.server_address))
    r.set("port", str(params.server_port))
    r.set("warmup_sec", str(params.warmup))
    r.set("measure_sec", str(params.measure))

    # initialize semaphores
    r.set("should_measure", 0)
    r.set("should_report", 0)
    r.set("reported", 0)
    r.set("started", 0)

def start_clients(client_addresses, path_to_client, redis_address):
    for addr in client_addresses:
        start_client(addr, path_to_client, redis_address)

def main():
    if len(sys.argv) < 3:
        print("Usage: ./driver.py <redis_address> <server_address> [<client_address> ...]")
        exit(-1)

    redis_address = sys.argv[1]
    client_addresses = sys.argv[3:]
    start_redis(redis_address)
    time.sleep(1.5) # XXX: wait for redis to have started

    p = BenchmarkParams()
    p.num_clients = len(client_addresses)
    p.num_threads = 2
    p.server_address = sys.argv[2]
    p.server_port = 12345
    p.warmup = 5
    p.measure = 5

    write_benchmark_params_to_redis(redis_address, p)

    # start_clients(client_addresses, "~/research/grove/echo-server/bin/multiclient", redis_address)

if __name__=="__main__":
    main()
