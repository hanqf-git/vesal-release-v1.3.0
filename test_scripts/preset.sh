echo 4  > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
echo 8192 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
cpupower frequency-set -g performance
