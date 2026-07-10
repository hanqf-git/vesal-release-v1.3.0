echo 4  > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
echo 10240 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
cpupower frequency-set -g performance
cpupower idle-set -d 2
