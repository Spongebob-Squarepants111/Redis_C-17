#!/usr/bin/env bash
for impl in simple_redis redis-server; do
  PORT=$([ "$impl" = "simple_redis" ] && echo 6379 || echo 6380)
  for clients in 1 4 16 64; do
    for ratio in "1:0" "0:1" "1:1"; do
      echo "$impl c$clients r$ratio"
      # 用 redis-benchmark 模拟客户端并发和读写比例
      redis-benchmark -h 127.0.0.1 -p $PORT \
        -c $clients -n 1000000 \
        --csv \
        -t set,get \
        -P 1 \
        ${ratio/1:0/-t set} \
        ${ratio/0:1/-t get} \
        ${ratio/1:1/"-t set,get"}
    done
  done
done
