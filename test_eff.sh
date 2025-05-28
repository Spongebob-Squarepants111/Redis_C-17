redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50
redis-benchmark -h 127.0.0.1 -p 6379 -t get -n 1000000 -c 50
redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 50 -P 16
redis-benchmark -h 127.0.0.1 -p 6379 -t set -n 1000000 -c 10000