# Multi-Paxos-PoC
Proof of concept of Multi-Paxos Electrode optimization for Future Internet Technologies course at AGH University of Cracow 2023/24.

# Structure of the repository

In directory `docs` you can find our preliminary understanding of two NSDI conferences and SIGCOMM on the practical use of the eBPF service to optimize the performance of applications or other services.

In directory `Electrode` you can find implementation of Multi-Paxos algorithm with Electrode optimization, cloned from [its author repository](https://github.com/Electrode-NSDI23/Electrode). The implementation is based on the paper [Multi-Paxos with Electrodes: A Framework for Optimizing State Machine Replication Protocols](https://arxiv.org/abs/2202.13194), which we have considered to be the most interesting and promising for our proof of concept, so we decide to reproduce it on few different scenarios.

We decided to reproduce the results of the paper using Docker images and Docker Compose orchestration. In the root directory you can find Dockerfiles and Docker Compose files for each scenario. For each scenario you need to build the Docker image and run the Docker Compose file. The scenarios are:
* `scenario3` - three replicas, one client
```
docker build -t electrode:scenario3 -f ./scenario3/Dockerfile . 
docker compose -f ./scenario3/docker-compose.yaml up
# to cleanup containers after running the scenario
docker compose -f ./scenario3/docker-compose.yaml down
```
* `scenario5` - five replicas, one client
```
docker build -t electrode:scenario5 -f ./scenario5/Dockerfile . 
docker compose -f ./scenario5/docker-compose.yaml up
# to cleanup containers after running the scenario
docker compose -f ./scenario5/docker-compose.yaml down
```
* `scenario7` - seven replicas, one client
```
docker build -t electrode:scenario7 -f ./scenario7/Dockerfile . 
docker compose -f ./scenario7/docker-compose.yaml up
# to cleanup containers after running the scenario
docker compose -f ./scenario7/docker-compose.yaml down
```

Each scenario directory contains:
* `Dockerfile` - Dockerfile for building the Docker image
* `docker-compose.yaml` - Docker Compose file for running the scenario with according number of replicas
* `config.txt` - Text file with replicas IP addresses and ports (container names instead of IP addresses thanks to Docker Compose)
* `fast_user.c` - C script with replicas MAC addresses on line 281
* `fast_common.h` - Header file with cluster size on line 17

All Dockerfiles have in general the same structure:
* Install required apt packages on Ubuntu 20.04
* Install kernel 5.8.0 and headers
* Reboot to use the new kernel
* Clone the Electrode repository and files specific for the scenario
* Build the Electrode
* Configure NIC and irqbalance
* Execute script with:
    * mounting BPF filesystem 
    * running `fast` script on NIC 
    * command you specify in the Docker Compose file

Client container after proper initialization will send requests to the replicas and measure the latency of the responses. The results will be visible in the terminal.