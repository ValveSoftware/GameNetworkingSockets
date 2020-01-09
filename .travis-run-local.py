#!/usr/bin/python

import logging
import os
import subprocess
import sys
import time
import yaml

def read_travis_yml():
    return yaml.load(open('.travis.yml', 'r'))

def docker_arch(travis_arch):
    if travis_arch == 'arm64':
        return 'arm64v8'
    return travis_arch

def env_parse(env, arch=None):
    kv_str = env.split()
    kv = { k: v for k, v in [ s.split('=') for s in kv_str ] }

    # Ugly trick to prepare for running commands for this image
    try:
        del os.environ['IMAGE_PREFIX']
    except KeyError:
        pass
    os.environ.update(kv)

    if 'IMAGE' not in kv or 'IMAGE_TAG' not in kv:
        return None

    # e.g. ubuntu:latest
    image = '%s:%s' % (kv['IMAGE'], kv['IMAGE_TAG'])

    if arch is not None:
        image_prefix = docker_arch(arch) + '/'
        os.environ['IMAGE_PREFIX'] = image_prefix
        return image_prefix + image

    return image

def get_images(travis):
    for env in travis['env']['global']:
        env_parse(env)
    for env in travis['env']['jobs']:
        yield env_parse(env)
    for job in travis['jobs']['include']:
        yield env_parse(job['env'], job['arch'])

def docker_pull(image):
    subprocess.run(['docker', 'pull', image], check=True)

def pull_images(travis):
    for image in get_images(travis):
        docker_pull(image)

def init_logging(level=logging.INFO):
    root_logger = logging.getLogger('')
    root_logger.setLevel(level)

    # Log to stdout
    console_log_format = logging.Formatter(
        '%(asctime)s %(levelname)7s: %(message)s',
        '%Y-%m-%d %H:%M:%S')
    console_logger = logging.StreamHandler(sys.stdout)
    console_logger.setFormatter(console_log_format)
    console_logger.setLevel(level)
    root_logger.addHandler(console_logger)

    return root_logger

def kill_and_wait():
    log.info("Terminating build container")
    subprocess.run('docker kill $CONTAINER_NAME', shell=True, stdout=subprocess.DEVNULL)
    subprocess.run('docker container rm $CONTAINER_NAME', shell=True, stdout=subprocess.DEVNULL)

    # The container removal process is asynchronous and there's no
    # convenient way to wait for it, so we'll just poll and see if the
    # container still exists.
    log.info("Waiting for container to exit...")
    attempts = 0
    while True:
        proc = subprocess.run('docker container inspect $CONTAINER_NAME', shell=True, stdout=subprocess.DEVNULL)
        if proc.returncode != 0:
            break
        log.info("Container is still alive, waiting a few seconds")
        attempts += 1
        if attempts > 5:
            raise RuntimeError
        time.sleep(3)
    log.info("Container has exited.")

def needs_qemu_binaries():
    required_bins = [
        '/usr/bin/qemu-aarch64-static',
        '/usr/bin/qemu-ppc64le-static',
        '/usr/bin/qemu-s390x-static',
    ]
    for filename in required_bins:
        if not os.path.exists(filename):
            log.warning("QEMU userspace emulation binary %s is missing, will use binary from multiarch image", filename)
            return True
        log.info("QEMU userspace emulation binary %s is present", filename)
    return False

def main():
    global log
    log = init_logging()

    log.info("Parsing Travis configuration file")
    travis = read_travis_yml()

    if needs_qemu_binaries():
        multiarch_image = 'multiarch/qemu-user-static:latest'
    else:
        multiarch_image = 'multiarch/qemu-user-static:register'

    log.info("Will run image %s to enable support for foreign architecture containers", multiarch_image)

    # Pull the images down first
    log.info("Pulling Docker images")
    docker_pull(multiarch_image)
    pull_images(travis)

    # Initialize system environment
    log.info("Preparing system to run foreign architecture containers")
    subprocess.run(['docker', 'run', '--rm', '--privileged', multiarch_image, '--reset', '-p', 'yes'], check=True)

    # Run native tests first
    kill_and_wait()
    for image in get_images(travis):
        log.info("Running build and test process for image %s", image)
        stages = ['before_install', 'install', 'script', 'after_script']
        for stage in stages:
            for command in travis.get(stage, []):
                subprocess.run(command, shell=True, check=True)
        kill_and_wait()

    log.info("Build and test run complete!")


if __name__ == "__main__":
    main()
