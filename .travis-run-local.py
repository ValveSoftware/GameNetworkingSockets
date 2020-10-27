#!/usr/bin/python
"""
Usage:
    .travis-run-local.py [--arch=<arch>] [--image=<image> --image-tag=<tag>]

Options:
    -h --help          Print this help message.
    --arch=<arch>      Only build in containers with this CPU architecture. [default: any]
    --image=<image>    Only build for this image name. [default: any]
    --image-tag=<tag>  Only build for this image tag. [default: any]
"""

import logging
import os
import subprocess
import sys
import time
import yaml
from docopt import docopt

def read_travis_yml():
    return yaml.load(open('.travis.yml', 'r'), Loader=yaml.SafeLoader)

def docker_arch(travis_arch):
    if travis_arch == 'arm64':
        return 'arm64v8'
    return travis_arch

def image_name(image, tag, arch):
    # e.g. ubuntu:latest
    image = '%s:%s' % (image, tag)

    if arch != 'amd64':
        image_prefix = docker_arch(arch) + '/'
        os.environ['IMAGE_PREFIX'] = image_prefix
        return image_prefix + image

    return image

def env_parse(env, arch):
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

    if options['--arch'] != 'any' and arch != options['--arch']:
        return None

    if options['--image'] != 'any' and kv['IMAGE'] != options['--image']:
        return None

    if options['--image-tag'] != 'any' and kv['IMAGE_TAG'] != options['--image-tag']:
        return None

    return image_name(kv['IMAGE'], kv['IMAGE_TAG'], arch)

def get_images(travis):
    match_found = False

    for env in travis['env']['global']:
        env_parse(env, travis['arch'])

    for env in travis['env']['jobs']:
        image = env_parse(env, travis['arch'])
        if image is not None:
            match_found = True
            yield image

    for job in travis['jobs']['include']:
        image = env_parse(job['env'], job['arch'])
        if image is not None:
            match_found = True
            yield image

    # If we didn't find a match with our constraints, maybe the user wanted to
    # test a specific image not listed in .travis.yml.
    if not match_found:
        if 'any' not in [options['--image'], options['--image-tag'], options['--arch']]:
            image = env_parse(
                'IMAGE=%s IMAGE_TAG=%s' % (options['--image'], options['--image-tag']),
                options['--arch']
            )
            if image is not None:
                yield image

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

def main():
    global options
    global log
    log = init_logging()

    options = docopt(__doc__)

    # We do a shallow "git submodule update --init" in a real travis build, but
    # that's not useful for local development purposes. We should do a real
    # update before a container can make shallow clones.
    log.info("Updating submodules")
    subprocess.run(['git', 'submodule', 'update', '--init'])

    log.info("Parsing Travis configuration file")
    travis = read_travis_yml()

    # Pull the images down first
    log.info("Pulling Docker images")
    docker_pull('aptman/qus')
    pull_images(travis)

    # Initialize system environment
    log.info("Preparing system to run foreign architecture containers")
    subprocess.run(['docker', 'run', '--rm', '--privileged', 'aptman/qus', '-s', '--', '-r'], check=True)
    subprocess.run(['docker', 'run', '--rm', '--privileged', 'aptman/qus', '-s', '--', '-p'], check=True)

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
