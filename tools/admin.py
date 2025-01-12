#!/usr/bin/python3
"""Admin tool

This tool is used to control a cluster of SLOG servers. For example,
starting a cluster, stopping a cluster, getting status, and more.
"""
import collections
import ipaddress
import itertools
import logging
import os
import shutil
import time

import docker
import google.protobuf.text_format as text_format

from datetime import datetime
from multiprocessing.dummy import Pool
from typing import Dict, List, Tuple

from docker.models.containers import Container
from paramiko.ssh_exception import PasswordRequiredException

from common import Command, initialize_and_run_commands
from gen_data import add_exported_gen_data_arguments
from proto.configuration_pb2 import Configuration

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(process)d - %(levelname)s: %(message)s'
)
LOG = logging.getLogger("admin")

USER = "ubuntu"
CONTAINER_DATA_DIR = "/var/tmp"
HOST_DATA_DIR = "/var/tmp"

SLOG_IMG = "ctring/slog"
SLOG_CONTAINER_NAME = "slog"
SLOG_DATA_MOUNT = docker.types.Mount(
    target=CONTAINER_DATA_DIR,
    source=HOST_DATA_DIR,
    type="bind",
)
SLOG_CONFIG_FILE_NAME = "slog.conf"
CONTAINER_SLOG_CONFIG_FILE_PATH = os.path.join(
    CONTAINER_DATA_DIR,
    SLOG_CONFIG_FILE_NAME
)

BENCHMARK_CONTAINER_NAME = "benchmark"

RemoteProcess = collections.namedtuple(
    'RemoteProcess',
    [
        'docker_client',
        'address',
        'replica',
        'partition',
        'procnum',
    ]
)


def cleanup_container(
    client: docker.DockerClient,
    name: str,
    addr="",
) -> None:
    """
    Cleans up a container with a given name.
    """
    try:
        c = client.containers.get(name)
        c.remove(force=True)
        LOG.info(
            "%sCleaned up container \"%s\"",
            f"{addr}: " if addr else "",
            name,
        )
    except:
        pass


def get_container_status(client: docker.DockerClient, name: str) -> str:
    if client is None:
        return "network unavailable"
    else:
        try:
            c = client.containers.get(name)
            return c.status
        except docker.errors.NotFound:
            return "container not started"
        except:
            pass
    return "unknown"


def wait_for_containers(
    containers: List[Tuple[Container, str]]
) -> None:
    """
    Waits until all given containers stop.
    """
    for c, addr in containers:
        res = c.wait()
        if res['StatusCode'] == 0:
            LOG.info("%s: Done", addr)
        else:
            LOG.error(
                "%s: Finished with non-zero status (%d). "
                "Check the logs of the container \"%s\" for more details",
                addr,
                res['StatusCode'],
                c.name,
            )


def parse_envs(envs: List[str]) -> Dict[str, str]:
    '''Parses a list of environment variables

    This function transform a list of strings such as ["env1=a", "env2=b"] into:
    {
        env: a,
        env: b,
    }

    '''
    if envs is None:
        return {}
    env_var_tuples = [env.split('=') for env in envs]
    return {env[0]: env[1] for env in env_var_tuples}


class AdminCommand(Command):
    """Base class for a command.

    This class contains the common implementation of all commands in this tool
    such as parsing of common arguments, loading config file, and pulling new
    SLOG image from docker repository.

    All commands must extend from this class. A command may or may not override
    any method but it must override and implement the `do_command` method as
    well as all class variables to describe the command.
    """

    CONFIG_FILE_REQUIRED = True

    def __init__(self):
        self.config = None

    def add_arguments(self, parser):
        parser.add_argument(
            "config",
            nargs='?',
            default="",
            metavar="config_file",
            help="Path to a config file",
        )
        parser.add_argument(
            "--no-pull",
            action="store_true",
            help="Skip image pulling step"
        )
        parser.add_argument(
            "--image",
            default=SLOG_IMG,
            help="Name of the Docker image to use"
        )
        parser.add_argument(
            "--user", "-u",
            default=USER,
            help="Username of the target machines"
        )

    def initialize_and_do_command(self, args):
        # The initialization phase is broken down into smaller methods so
        # that subclasses can override them with different behaviors
        self.load_config(args)
        self.init_remote_processes(args)
        self.pull_slog_image(args)
        # Perform the command
        self.do_command(args)

    def load_config(self, args):
        if not os.path.isfile(args.config):
            if self.CONFIG_FILE_REQUIRED:
                raise FileNotFoundError(f'Config file does not exist: "{args.config}"')
            else:
                return
        with open(args.config, "r") as f:
            self.config = Configuration()
            text_format.Parse(f.read(), self.config)

    def init_remote_processes(self, args):
        procs = []
        # Create a docker client for each node
        for rep, rep_info in enumerate(self.config.replicas):
            for part, addr in enumerate(rep_info.addresses):
                # Use None as a placeholder for the first value
                procs.append([None, addr.decode(), rep, part])
        
        def init_docker_client(remote_proc):
            _, addr, _, _ = remote_proc
            try:
                remote_proc[0] = self.new_client(args.user, addr)
                LOG.info("Connected to %s", addr)
            except PasswordRequiredException as e:
                LOG.error(
                    "Failed to authenticate when trying to connect to %s. "
                    "Check username or key files",
                    f"{args.user}@{addr}",
                )
            except Exception as e:
                LOG.error("Failed to connect to %s", addr)

        with Pool(processes=len(procs)) as pool:
            pool.map(init_docker_client, procs)

        self.remote_procs = [RemoteProcess(*m, None) for m in procs if m[0] is not None]

    def pull_slog_image(self, args):
        if len(self.remote_procs) == 0 or args.no_pull:
            LOG.info(
                "Skipped image pulling. Using the local version of \"%s\"", 
                args.image,
            )
            return
        LOG.info(
            'Pulling image "%s" for each node. '
            "This might take a while.",
            args.image
        )

        clients = {client for client, *_ in self.remote_procs}
        with Pool(processes=len(clients)) as pool:
            pool.map(lambda client : client.images.pull(args.image), clients)

    def do_command(self, args):
        raise NotImplementedError

    ##############################
    #       Helper methods
    ##############################
    def new_client(self, user, addr):
        """
        Gets a new Docker client for a given address.
        """
        return docker.DockerClient(
            base_url=f'ssh://{user}@{addr}',
        )


class GenDataCommand(AdminCommand):

    NAME = "gen_data"
    HELP = "Generate data for one or more SLOG servers"
    DESCRIPTION = """
    For data with non-numeric keys, there is no way for the benchmarking script
    to know the location of a key so that it can, for example, vary the
    percentage of single-home/multi-home transactions. In that case, we need to
    generate two copies of data, one for the server, and one for the
    benchmarking tool.
    """

    def add_arguments(self, parser):
        super().add_arguments(parser)
        add_exported_gen_data_arguments(parser)

    def do_command(self, args):
        shell_cmd = (
            f"tools/gen_data.py {CONTAINER_DATA_DIR} "
            f"--num-replicas {len(self.config.replicas)} "
            f"--num-partitions {self.config.num_partitions} "
            f"--partition-bytes {self.config.hash_partitioning.partition_key_num_bytes} "
            f"--partition {args.partition} "
            f"--size {args.size} "
            f"--size-unit {args.size_unit} "
            f"--record-size {args.record_size} "
            f"--max-jobs {args.max_jobs} "
        )
        containers = []
        for client, addr, *_ in self.remote_procs:
            cleanup_container(client, self.NAME, addr=addr)
            LOG.info(
                "%s: Running command: %s",
                addr,
                shell_cmd
            )
            c = client.containers.create(
                args.image,
                name=self.NAME,
                command=shell_cmd,
                # Mount a directory on the host into the container
                mounts=[SLOG_DATA_MOUNT],
            )
            c.start()
            containers.append((c, addr))
        
        wait_for_containers(containers)
        

class StartCommand(AdminCommand):

    NAME = "start"
    HELP = "Start an SLOG cluster"

    def add_arguments(self, parser):
        super().add_arguments(parser)
        parser.add_argument(
            "-e",
            nargs="*",
            help="Environment variables to pass to the container. For example, "
                 "use -e GLOG_v=1 to turn on verbose logging at level 1."
        )
        
    def do_command(self, args):
        if len(self.remote_procs) == 0:
            return

        # Prepare a command to update the config file
        config_text = text_format.MessageToString(self.config)
        sync_config_cmd = (
            f"echo '{config_text}' > {CONTAINER_SLOG_CONFIG_FILE_PATH}"
        )

        # Clean up everything first so that the old running session does not
        # mess up with broker synchronization of the new session
        def clean_up(remote_proc):
            client, addr, *_ = remote_proc
            cleanup_container(client, SLOG_CONTAINER_NAME, addr=addr)

        with Pool(processes=len(self.remote_procs)) as pool:
            pool.map(clean_up, self.remote_procs)

        def start_container(remote_proc):
            client, addr, rep, part, *_ = remote_proc
            shell_cmd = (
                f"slog "
                f"--config {CONTAINER_SLOG_CONFIG_FILE_PATH} "
                f"--address {addr} "
                f"--data-dir {CONTAINER_DATA_DIR} "
            )
            client.containers.run(
                args.image,
                name=SLOG_CONTAINER_NAME,
                command=[
                    "/bin/sh", "-c",
                    f"{sync_config_cmd} && {shell_cmd}",
                ],
                # Mount a directory on the host into the container
                mounts=[SLOG_DATA_MOUNT],
                # Expose all ports from container to host
                network_mode="host",
                # Avoid hanging this tool after starting the server
                detach=True,
                environment=parse_envs(args.e),
            )
            LOG.info(
                "%s: Synced config and ran command: %s",
                addr,
                shell_cmd
            )

        with Pool(processes=len(self.remote_procs)) as pool:
            pool.map(start_container, self.remote_procs)


class StopCommand(AdminCommand):

    NAME = "stop"
    HELP = "Stop an SLOG cluster"

    def pull_slog_image(self, args):
        """
        Override this method to skip the image pulling step.
        """
        pass
        
    def do_command(self, args):
        if len(self.remote_procs) == 0:
            return

        def stop_container(remote_proc):
            client, addr, *_  = remote_proc
            try:
                LOG.info("Stopping SLOG on %s...", addr)
                c = client.containers.get(SLOG_CONTAINER_NAME)
                # Set timeout to 0 to kill the container immediately
                c.stop(timeout=0)
            except docker.errors.NotFound:
                pass

        with Pool(processes=len(self.remote_procs)) as pool:
            pool.map(stop_container, self.remote_procs)


class StatusCommand(AdminCommand):

    NAME = "status"
    HELP = "Show the status of an SLOG cluster"

    def pull_slog_image(self, args):
        """
        Override this method to skip the image pulling step.
        """
        pass
        
    def do_command(self, args):
        key_func = lambda p : p.replica
        remote_procs = sorted(self.remote_procs, key=key_func)
        for rep, g in itertools.groupby(remote_procs, key_func):
            print(f"Replica {rep}:")
            for client, addr, _, part, *_ in g:
                status = get_container_status(client, SLOG_CONTAINER_NAME)
                print(f"\tPartition {part} ({addr}): {status}")


class LogsCommand(AdminCommand):

    NAME = "logs"
    HELP = "Stream logs from a server"
    CONFIG_FILE_REQUIRED = False

    def add_arguments(self, parser):
        super().add_arguments(parser)
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument(
            "-a",
            metavar='ADDRESS',
            help="Address of the machine to stream logs from"
        )
        group.add_argument(
            "-rp",
            nargs=2,
            type=int,
            metavar=('REPLICA', 'PARTITION'),
            help="Two numbers representing the replica and"
                 "partition of the machine to stream log from."
        )
        parser.add_argument(
            "-f", "--follow", 
            action="store_true",
            help="Follow log output"
        )
        parser.add_argument(
            "--container",
            default=SLOG_CONTAINER_NAME,
            help="Name of the Docker container to show logs from"
        )

    def init_remote_processes(self, args):
        """
        Override this method because we only need one client
        """
        self.client = None
        self.addr = None
        try:
            if args.a is not None:
                if self.config is None:
                    self.addr = args.a
                else:
                    # Check if the given address is specified in the config
                    addr_is_in_replica = (
                        args.a.encode() in rep.addresses
                        for rep in self.config.replicas
                    )
                    if any(addr_is_in_replica):
                        self.addr = args.a
                    else:
                        LOG.error(
                            "Address \"%s\" is not specified in the config", args.a
                        )
                        return
            else:
                if self.config is None:
                    raise Exception('The "-rp" flag requires a valid config file')
                r, p = args.rp
                self.addr = self.config.replicas[r].addresses[p].decode()

            self.client = self.new_client(args.user, self.addr)
            LOG.info("Connected to %s", self.addr)
        except PasswordRequiredException as e:
            LOG.error(
                "Failed to authenticate when trying to connect to %s. "
                "Check username or key files",
                f"{args.user}@{self.addr}",
            )

    def pull_slog_image(self, args):
        """
        Override this method to skip the image pulling step.
        """
        pass

    def do_command(self, args):
        if self.client is None:
            return

        try:
            c = self.client.containers.get(args.container)
        except docker.errors.NotFound:
            LOG.error("Cannot find container \"%s\"", args.container)
            return

        if args.follow:
            try:
                for log in c.logs(stream=True, follow=True):
                    print(log.decode(), end="")
            except KeyboardInterrupt:
                print()
        else:
            print(c.logs().decode(), end="")


class LocalCommand(AdminCommand):

    NAME = "local"
    HELP = "Control a cluster that run on the local machine"
    DESCRIPTION = """
    This command is used to run a cluster on the local machine. It has
    different flags corresponding to the different commands similar to
    those used to interact with the remote machines.
    """

    # Local network
    NETWORK_NAME = "slog_nw"
    SUBNET = "172.28.0.0/16"
    IP_RANGE = "172.28.5.0/24"

    def add_arguments(self, parser):
        super().add_arguments(parser)
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument(
            "--start",
            action='store_true',
            help="Start the local cluster"
        )
        group.add_argument(
            "--stop",
            action='store_true',
            help="Stop the local cluster"
        )
        group.add_argument(
            "--remove",
            action="store_true",
            help="Remove all containers of the local cluster"
        )
        group.add_argument(
            "--status",
            action="store_true",
            help="Get status of the local cluster",
        )
        parser.add_argument(
            "-e",
            nargs="*",
            help="Environment variables to pass to the container. For example, "
                 "use -e GLOG_v=1 to turn on verbose logging at level 1."
        )

    def load_config(self, args):
        super().load_config(args)
        # Replace the addresses in the config with auto-generated addresses
        address_generator = ipaddress.ip_network(self.IP_RANGE).hosts()
        for rep in self.config.replicas:
            del rep.addresses[:]
            for p in range(self.config.num_partitions):
                ip_address = str(next(address_generator))
                rep.addresses.append(ip_address.encode())

    def init_remote_processes(self, args):
        """
        Override this method because we only need one client
        """
        self.client = docker.from_env()

    def pull_slog_image(self, args):
        """
        Override this method because we only pull image for one client
        """
        if self.client is None or args.no_pull:
            LOG.info(
                "Skipped image pulling. Using the local version of \"%s\"", 
                args.image,
            )
            return
        LOG.info("Pulling latest docker image \"%s\"...", args.image)
        self.client.images.pull(args.image)

    def do_command(self, args):
        if self.client is None:
            return

        if args.start:
            self.__start(args)
        elif args.stop:
            self.__stop()
        elif args.remove:
            self.__remove()
        elif args.status:
            self.__status()

    def __start(self, args):
        #
        # Create a local network if one does not exist
        #
        nw_list = self.client.networks.list(names=[self.NETWORK_NAME])
        if nw_list:
            slog_nw = nw_list[0]
            LOG.info("Reused network \"%s\"", self.NETWORK_NAME)
        else:
            ipam_pool = docker.types.IPAMPool(
                subnet=self.SUBNET,
                iprange=self.IP_RANGE
            )
            ipam_config = docker.types.IPAMConfig(pool_configs=[ipam_pool])
            slog_nw = self.client.networks.create(
                name=self.NETWORK_NAME,
                driver="bridge",
                check_duplicate=True,
                ipam=ipam_config,
            )
            LOG.info("Created network \"%s\"", self.NETWORK_NAME)

        #
        # Spin up local Docker containers and connect them to the network
        #
        config_text = text_format.MessageToString(self.config)
        sync_config_cmd = (
            f"echo '{config_text}' > {CONTAINER_SLOG_CONFIG_FILE_PATH}"
        )

        # Clean up everything first so that the old running session does not
        # mess up with broker synchronization of the new session
        for r, rep in enumerate(self.config.replicas):
            for p, _ in enumerate(rep.addresses):
                container_name = f"slog_{r}_{p}"
                cleanup_container(self.client, container_name)

        for r, rep in enumerate(self.config.replicas):
            for p, addr_b in enumerate(rep.addresses):
                addr = addr_b.decode()
                shell_cmd = (
                    f"slog "
                    f"--config {CONTAINER_SLOG_CONFIG_FILE_PATH} "
                    f"--address {addr} "
                    f"--data-dir {CONTAINER_DATA_DIR} "
                )
                container_name = f"slog_{r}_{p}"
                # Create and run the container
                container = self.client.containers.create(
                    args.image,
                    name=container_name,
                    command=[
                        "/bin/sh", "-c",
                        f"{sync_config_cmd} && {shell_cmd}",
                    ],
                    mounts=[SLOG_DATA_MOUNT],
                    environment=parse_envs(args.e)
                )

                # Connect the container to the custom network.
                # This has to happen before we start the container.
                slog_nw.connect(container, ipv4_address=addr)

                # Actually start the container
                container.start()

                LOG.info(
                    "%s: Synced config and ran command: %s",
                    addr,
                    shell_cmd,
                )
    
    def __stop(self):
        for r in range(len(self.config.replicas)):
            for p in range (self.config.num_partitions):
                try:
                    container_name = f"slog_{r}_{p}"
                    LOG.info("Stopping \"%s\"", container_name)
                    c = self.client.containers.get(container_name)
                    c.stop(timeout=0)
                except docker.errors.NotFound:
                    pass
    
    def __remove(self):
        for r in range(len(self.config.replicas)):
            for p in range (self.config.num_partitions):
                container_name = f"slog_{r}_{p}"
                cleanup_container(self.client, container_name)

    def __status(self):
        for r, rep in enumerate(self.config.replicas):
            print(f"Replica {r}:")
            for p, addr in enumerate(rep.addresses):
                container_name = f"slog_{r}_{p}"
                status = get_container_status(self.client, container_name)
                print(f"\tPartition {p} ({addr.decode()}): {status}")


class BenchmarkCommand(AdminCommand):

    NAME = "benchmark"
    HELP = "Spawn distributed clients to run benchmark"
    DESCRIPTION = """
    The format of the client config file is:
    [
        {
            '<addr_0>': <number_of_processes>,
            '<addr_1>': <number_of_processes>,
            ...
        },
        ...
    ]

    Example:
    [
        {
            '192.168.0.10': 2,
            '192.168.0.11': 3
        },
        {
            '192.168.0.13': 2
        }
    ]

    This config will start 2 client processes for machine at '192.168.0.10',
    3 clients for machine at '192.168.0.11', and 2 clients for machine at
    '192.168.0.13'. The first two machines will send transactions to the first
    region and the last machine will send transactions to the second region.
    """

    def add_arguments(self, parser):
        super().add_arguments(parser)
        group = parser.add_mutually_exclusive_group(required=True)
        group.add_argument(
            "--num-txns", type=int,
            help="Number of transactions sent per client"
        )
        group.add_argument(
            "--duration", type=int,
            help="How long the benchmark is run in seconds"
        )
        parser.add_argument(
            "--steps", type=int, default=1,
            help=(
                "Can only be used with --duration. Step up from 0 client to "
                "all clients in given number of steps"
            )
        )
        parser.add_argument(
            "--tag",
            help="Tag of this benchmark run. Auto-generated if not provided"
        )
        parser.add_argument(
            "--workload", "-wl",
            default="basic",
            help="Name of the workload to run benchmark with"
        )
        parser.add_argument(
            "--params",
            default="",
            help="Parameters of the workload"
        )
        parser.add_argument(
            "--rate", type=int,
            default=1000,
            help="Maximum number of transactions sent per second"
        )
        parser.add_argument(
            "--workers", type=int,
            default=1,
            help="Number of threads for each client"
        )
        parser.add_argument(
            "--sample", type=int,
            default=10,
            help="Percent of sampled transactions to be written to result files"
        )
        parser.add_argument(
            "-e",
            nargs="*",
            help="Environment variables to pass to the container. For example, "
                 "use -e GLOG_v=1 to turn on verbose logging at level 1."
        )
        parser.add_argument(
            "--cleanup", action="store_true",
            help="Clean up all running benchmarks then exit"
        )

    def init_remote_processes(self, args):
        """
        Override this method because the clients of this command are created from
        the addresses specified in a json file instead of from the configuration file
        """
        proc_lists = []
        for rep, r in enumerate(self.config.replicas):
            for c in r.clients:
                # Create a list of processes per machine
                proc_lists.append([
                    [None, c.address.decode(), rep, None, procnum] for procnum in range(c.procs)
                ])
        
        # Interleave the lists of processes across machines
        proc_lists = itertools.zip_longest(*proc_lists)
        procs = [
            proc for proc_list in proc_lists 
            for proc in proc_list if proc is not None
        ]

        def init_docker_client(proc):
            _, addr, rep, _, procnum = proc
            try:
                proc[0] = self.new_client(args.user, addr)         
                LOG.info("Connected to %s", addr)
            except PasswordRequiredException as e:
                LOG.error(
                    "Failed to authenticate when trying to connect to %s. "
                    "Check username or key files",
                    f"{args.user}@{addr}",
                )
            except Exception as e:
                LOG.exception(e)
        
        with Pool(processes=len(procs)) as pool:
            pool.map(init_docker_client, procs)

        self.remote_procs = [
            RemoteProcess(docker_client, addr, rep, None, procnum)
            for docker_client, addr, rep, _, procnum in procs
        ]

    def do_command(self, args):
        # Prepare a command to update the config file
        config_text = text_format.MessageToString(self.config)
        sync_config_cmd = (
            f"echo '{config_text}' > {CONTAINER_SLOG_CONFIG_FILE_PATH}"
        )

        if args.tag:
            tag = args.tag
        else:
            tag = datetime.now().strftime("%Y-%m-%d-%H-%M-%S")

        parent_dir = os.path.join(CONTAINER_DATA_DIR, f"slog-benchmark-{tag}")

        # Clean up everything
        def clean_up(remote_proc):
            client, addr, _, _, proc = remote_proc
            container_name = f'{BENCHMARK_CONTAINER_NAME}_{proc}'
            cleanup_container(client, container_name, addr=addr)
            client.containers.run(
                args.image,
                name=container_name,
                command=["/bin/sh", "-c", f"rm -rf {parent_dir}"],
                # Mount a directory on the host into the container
                mounts=[SLOG_DATA_MOUNT],
            )
            LOG.info("%s: Removed old data directory", addr)
            cleanup_container(client, container_name, addr=addr)

        with Pool(processes=len(self.remote_procs)) as pool:
            pool.map(clean_up, self.remote_procs)
        
        if args.cleanup:
            return

        def benchmark_runner(proc_and_params):
            proc, num_txns, duration = proc_and_params
            client, addr, rep, _, procnum = proc

            if num_txns is None:
                num_txns = args.rate * duration

            out_dir = os.path.join(parent_dir, str(procnum))
            mkdir_cmd = f"mkdir -p {out_dir}"
            shell_cmd = (
                f"benchmark "
                f"--config {CONTAINER_SLOG_CONFIG_FILE_PATH} "
                f"--r {rep} "
                f"--data-dir {CONTAINER_DATA_DIR} "
                f"--out-dir {out_dir} "
                f"--wl {args.workload} "
                f'--params "{args.params}" '
                f"--rate {args.rate} "
                f"--txns {num_txns} "
                f"--workers {args.workers} "
                f"--sample {args.sample} "
                f"--txn_profiles"
            )
            container = client.containers.run(
                args.image,
                name=f'{BENCHMARK_CONTAINER_NAME}_{procnum}',
                command=[
                    "/bin/sh", "-c",
                    f"{sync_config_cmd} && {mkdir_cmd} && {shell_cmd}"
                ],
                # Mount a directory on the host into the container
                mounts=[SLOG_DATA_MOUNT],
                # Expose all ports from container to host
                network_mode="host",
                # Avoid hanging this tool after starting the server
                detach=True,
                environment=parse_envs(args.e),
            )
            LOG.info(
                "%s: Synced config and ran command: %s",
                addr,
                shell_cmd
            )
            return container, addr
        
        num_procs = len(self.remote_procs)
        duration = args.duration
        batch_size = (num_procs + 1) // args.steps if duration else num_procs
        
        containers = []
        COMPENSATE = 1 # compensation for the overhead at each step
        steps = range(0, num_procs, batch_size)
        remaining_steps = len(steps)
        step_duration = duration // len(steps) if duration else 0
        for s, i in enumerate(steps):
            remaining_steps -= 1
            batch = self.remote_procs[i:i + batch_size]
            proc_and_params = zip(
                batch,
                itertools.repeat(args.num_txns),
                itertools.repeat(duration + remaining_steps * COMPENSATE if duration else None),
            )
            with Pool(processes=len(batch)) as pool:
                containers += pool.map(benchmark_runner, proc_and_params)
            LOG.info("Step %d: started %d clients", s + 1, len(batch))

            if duration:
                time.sleep(step_duration)
                duration -= step_duration
                # Avoid duration reach 0, which makes the benchmark run forever
                duration = max(duration, 1)

        wait_for_containers(containers)

        LOG.info("Tag: %s", tag)


class CollectCommand(AdminCommand):

    NAME = "collect"
    HELP = "Collect benchmark data from the clients"

    def add_arguments(self, parser):
        parser.add_argument(
            "config",
            metavar="config_file",
            help="Path to a config file",
        )
        parser.add_argument(
            "tag",
            help="Tag of the benchmark data"
        )
        parser.add_argument(
            "--out-dir",
            default='',
            help="Directory to put the collected data"
        )
        parser.add_argument(
            "--user", "-u",
            default=USER,
            help="Username of the target machines"
        )

    def init_remote_processes(self, args):
        pass

    def pull_slog_image(self, args):
        pass

    def do_command(self, args):
        name = f"slog-benchmark-{args.tag}"
        out_path = os.path.join(args.out_dir, name)
        
        if os.path.exists(out_path):
            shutil.rmtree(out_path, ignore_errors=True)
            LOG.info("Removed existing directory: %s", out_path)

        os.makedirs(out_path)
        LOG.info(f"Created directory: {out_path}")

        commands = []
        for r in self.config.replicas:
            for c in r.clients:
                addr = c.address.decode()
                data_path = os.path.join(HOST_DATA_DIR, name)
                data_tar_file = f'{addr}.tar.gz'
                data_tar_path = os.path.join(HOST_DATA_DIR, data_tar_file)
                out_tar_path = os.path.join(out_path, data_tar_file)
                out_addr_path = os.path.join(out_path, addr)

                os.makedirs(out_addr_path, exist_ok=True)
                cmd = (
                    f'ssh {args.user}@{addr} "tar -czf {data_tar_path} -C {data_path} ." && '
                    f'rsync -vh --inplace {args.user}@{addr}:{data_tar_path} {out_path} && '
                    f'tar -xzf {out_tar_path} -C {out_addr_path}'
                )
                commands.append(f'({cmd}) & ')
    
        LOG.info("Executing commands:\n%s", '\n'.join(commands))
        os.system(''.join(commands) + ' wait')


if __name__ == "__main__":
    start_time = time.time()
    initialize_and_run_commands(
        "Controls deployment and experiment of SLOG",
        [
            BenchmarkCommand,
            CollectCommand,
            GenDataCommand,
            StartCommand,
            StopCommand,
            StatusCommand,
            LogsCommand,
            LocalCommand,
        ]
    )
    LOG.info("Elapsed time: %.1f sec", time.time() - start_time)