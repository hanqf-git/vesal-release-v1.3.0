echo 8  > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
echo 2048 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
cpupower frequency-set -g performance
