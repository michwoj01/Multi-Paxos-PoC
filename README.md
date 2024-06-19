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
```
* `scenario5` - five replicas, one client
```
docker build -t electrode:scenario5 -f ./scenario5/Dockerfile . 
docker compose -f ./scenario3/docker-compose.yaml up
```
* `scenario7` - seven replicas, one client
```
docker build -t electrode:scenario7 -f ./scenario7/Dockerfile . 
docker compose -f ./scenario7/docker-compose.yaml up
```

