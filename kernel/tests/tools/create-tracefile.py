#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""This utility generates a single lcov tracefile from a gcov tar file."""

import argparse
import collections
import fnmatch
import glob
import json
import logging
import os
import pathlib
import re
import shutil
import sys
import tarfile


LCOV = "lcov"

# Relative to the root of the source tree.
OUTPUT_COV_DIR = os.path.join("out", "coverage")

BUILD_CONFIG_CONSTANTS_PATH = os.path.join("common", "build.config.constants")

PREBUILT_CLANG_DIR = os.path.join("prebuilts", "clang", "host", "linux-x86")

PREBUILT_LLVM_COV_PATH_FORMAT = os.path.join(
    PREBUILT_CLANG_DIR, "clang-%s", "bin", "llvm-cov"
)

PREBUILT_STABLE_LLVM_COV_PATH = os.path.join(
    PREBUILT_CLANG_DIR, "llvm-binutils-stable", "llvm-cov"
)

EXCLUDED_FILES = [
    "*/security/selinux/av_permissions.h",
    "*/security/selinux/flask.h",
]


def create_llvm_gcov_sh(
    llvm_cov_filename: str,
    llvm_gcov_sh_filename: str,
) -> None:
  """Create a shell script that is compatible with gcov.

  Args:
    llvm_cov_filename: The absolute path to llvm-cov.
    llvm_gcov_sh_filename: The path to the script to be created.
  """
  file_path = pathlib.Path(llvm_gcov_sh_filename)
  file_path.parent.mkdir(parents=True, exist_ok=True)
  file_path.write_text(f'#!/bin/bash\nexec {llvm_cov_filename} gcov "$@"')
  os.chmod(llvm_gcov_sh_filename, 0o755)


def generate_lcov_tracefile(
    gcov_dir: str,
    kernel_source: str,
    gcov_filename: str,
    tracefile_filename: str,
    included_files: [],
) -> None:
  """Call lcov to create tracefile based on gcov data files.

  Args:
    gcov_dir: Directory that contains the extracted gcov data files as retrieved
      from debugfs.
    kernel_source: Directory containing the kernel source same as what was used
      to build system under test.
    gcov_filename: The absolute path to gcov or a compatible script.
    tracefile_filename: The name of tracefile to create.
    included_files: List of source file pattern to include in tracefile. Can be
      empty in which case include allo source.
  """
  exclude_args = " ".join([f'--exclude "{f}"' for f in EXCLUDED_FILES])
  include_args = (
      " ".join([f'--include "{f[0]}"' for f in included_files])
      if included_files is not None
      else ""
  )

  logging.info("Running lcov on %s", gcov_dir)
  lcov_cmd = (
      f"{LCOV} -q "
      "--ignore-errors=source "
      "--rc branch_coverage=1 "
      f"-b {kernel_source} "
      f"-d {gcov_dir} "
      f"--gcov-tool {gcov_filename} "
      f"{exclude_args} "
      f"{include_args} "
      "--ignore-errors gcov,gcov,unused,unused "
      "--capture "
      f"-o {tracefile_filename} "
  )
  os.system(lcov_cmd)


def update_symlink_from_mapping(filepath: str, prefix_mappings: {}) -> bool:
  """Update symbolic link based on prefix mappings.

  It will attempt to update the given symbolic link based on the prefix
  mappings. For every "from" prefix that matches replace with the new "to"
  value. If the resulting path doesn't exist, try the next.

  Args:
    filepath: Path of symbolic link to update.
    prefix_mappings: A multimap where the key is the "from" prefix to match, and
      the value is an array of "to" values to attempt to replace with.

  Returns:
    True or false depending on the whether symbolic link was successfully
      updated to a new path that exists.
  """

  link_target = os.readlink(filepath)
  for old_prefix, new_prefix_list in prefix_mappings.items():
    for new_prefix in new_prefix_list:
      if link_target.startswith(old_prefix):
        new_target = os.path.abspath(
            link_target.replace(old_prefix, new_prefix)
        )
        if not os.path.exists(new_target):
          continue
        os.unlink(filepath)  # Remove the old symbolic link
        os.symlink(new_target, filepath)  # Create the updated link
        return True

  return False


def correct_symlinks_in_directory(directory: str, prefix_mappings: {}) -> None:
  """Recursively traverses a directory, updating symbolic links.

  Replaces 'old_prefix' in the link destination with 'new_prefix'.

  Args:
    directory: The root directory to traverse.
    prefix_mappings: Dictionary where the keys are the old prefixes and the
      values are the new prefixes
  """

  logging.info("Fixing up symbolic links in %s", directory)

  for root, _, files in os.walk(directory):
    for filename in files:
      filepath = os.path.join(root, filename)
      if os.path.islink(filepath):
        if not update_symlink_from_mapping(filepath, prefix_mappings):
          logging.error(
              "Unable to update link at %s with any prefix mappings: %s",
              filepath,
              prefix_mappings,
          )
          sys.exit(-1)


def find_most_recent_tarfile(path: str, pattern: str = "*.tar.gz") -> str:
  """Attempts to find a valid tar file given the location.

  If location is a directory finds the most recent tarfile or if location is a
  a valid tar file returns, if neither of these return None.

  Args:
    path (str): The path to either a tarfile or a directory.
    pattern (str, optional): Glob pattern for matching tarfiles. Defaults to
      "*.tar.gz".

  Returns:
      str: The path to the most recent tarfile found, or the original path
           if it was a valid tarfile. None if no matching tarfiles are found.
  """

  if os.path.isfile(path):
    if tarfile.is_tarfile(path):
      return path  # Path is a valid tarfile
    return None  # Path is a file but not a tar file

  if os.path.isdir(path):
    results = []
    for root, _, files in os.walk(path):
      for file in files:
        if fnmatch.fnmatch(file, pattern):
          full_path = os.path.join(root, file)
          results.append((full_path, os.path.getmtime(full_path)))

    if results:
      return max(results, key=lambda item: item[1])[
          0
      ]  # Return path of the most recent one
    else:
      return None  # No tarfiles found in the directory

  return None  # Path is neither a tarfile nor a directory


def make_absolute(path: str, base_dir: str) -> str:
  if os.path.isabs(path):
    return path

  return os.path.join(base_dir, path)


def append_slash(path: str) -> str:
  if path is not None and path[-1] != "/":
    path += "/"
  return path


def update_multimap_from_json(
    json_file: str, base_dir: str, result_multimap: collections.defaultdict
) -> None:
  """Reads 'to' and 'from' fields from a JSON file and updates a multimap.

  'from' refers to a bazel sandbox directory.
  'to' refers to the output directory of gcno files.
  The multimap is implemented as a dictionary of lists allowing multiple 'to'
  values for each 'from' key.

  Sample input:
  [
    {
      "from": "/sandbox/1/execroot/_main/out/android-mainline/common",
      "to": "bazel-out/k8-fastbuild/bin/common/kernel_x86_64/kernel_x86_64_gcno"
    },
    {
      "from": "/sandbox/2/execroot/_main/out/android-mainline/common",
      "to": "bazel-out/k8-fastbuild/bin/common-modules/virtual-device/virtual_device_x86_64/virtual_device_x86_64_gcno"
    }
  ]

  Args:
    json_file: The path to the JSON file.
    base_dir: Used if either of the 'to' or 'from' paths are relative to make
      them absolute by prepending this base_dir value.
    result_multimap: A multimap that is updated with every 'to' and 'from'
      found.

  Returns:
    The updated dictionary.
  """
  with open(json_file, "r") as file:
    data = json.load(file)

  for item in data:
    to_value = append_slash(item.get("to"))
    from_value = append_slash(item.get("from"))
    if to_value and from_value:
      to_value = make_absolute(to_value, base_dir)
      from_value = make_absolute(from_value, base_dir)
      result_multimap[from_value].append(to_value)


def read_gcno_mapping_files(
    search_dir_pattern: str,
    base_dir: str,
    result_multimap: collections.defaultdict
) -> None:
  """Search a directory for gcno_mapping."""
  found = False
  pattern = os.path.join(search_dir_pattern, "gcno_mapping.*.json")
  for filepath in glob.iglob(pattern, recursive=False):
    found = True
    logging.info("Reading %s", filepath)
    update_multimap_from_json(filepath, base_dir, result_multimap)

  if not found:
    logging.error("No gcno_mapping in %s", search_dir_pattern)


def read_gcno_dir(
    gcno_dir: str, result_multimap: collections.defaultdict
) -> None:
  """Read a directory containing gcno_mapping and gcno files."""
  multimap = collections.defaultdict(list)
  read_gcno_mapping_files(gcno_dir, gcno_dir, multimap)

  to_value = append_slash(os.path.abspath(gcno_dir))
  for from_value in multimap:
    result_multimap[from_value].append(to_value)


def get_testname_from_filename(file_path: str) -> str:
  filename = os.path.basename(file_path)
  if "_kernel_coverage" in filename:
    tmp = filename[: filename.find("_kernel_coverage")]
    testname = tmp[: tmp.rfind("_")]
  else:
    testname = filename[: filename.rfind("_")]
  return testname


def unpack_gcov_tar(file_path: str, output_dir: str) -> str:
  """Unpack the tar file into the specified directory.

  Args:
    file_path: The path of the tar file to be unpacked.
    output_dir: The root directory where the unpacked folder will reside.

  Returns:
    The path of extracted data.
  """

  testname = get_testname_from_filename(file_path)
  logging.info(
      "Unpacking %s for test %s...", os.path.basename(file_path), testname
  )

  test_dest_dir = os.path.join(output_dir, testname)
  if os.path.exists(test_dest_dir):
    shutil.rmtree(test_dest_dir)
  os.makedirs(test_dest_dir)
  shutil.unpack_archive(file_path, test_dest_dir, "tar")
  return test_dest_dir


def get_parent_path(path: str, levels_up: int) -> str:
  """Goes up a specified number of levels from a given path.

  Args:
    path: The path to find desired ancestor.
    levels_up: The number of levels up to go.

  Returns:
    The desired ancestor of the given path.
  """
  p = pathlib.Path(path)
  for _ in range(levels_up):
    p = p.parent
  return str(p)


def get_kernel_repo_dir() -> str:
  # Assume this script is in a kernel source tree:
  # kernel_repo/kernel/tests/tools/<this_script>
  return get_parent_path(os.path.abspath(__file__), 4)


def load_kernel_clang_version(repo_dir: str) -> str:
  """Load CLANG_VERSION from build.config.constants."""
  config_path = os.path.join(repo_dir, BUILD_CONFIG_CONSTANTS_PATH)
  if not os.path.isfile(config_path):
    return ""
  clang_version = ""
  with open(config_path, "r") as config_file:
    for line in config_file:
      match = re.fullmatch(r"\s*CLANG_VERSION=(\S*)\s*", line)
      if match:
        clang_version = match.group(1)
  return clang_version


class Config:
  """The input and output paths of this script."""

  def __init__(self, repo_dir: str, llvm_cov_path: str, tmp_dir: str):
    """Each argument can be empty."""
    self._repo_dir = os.path.abspath(repo_dir) if repo_dir else None
    self._llvm_cov_path = (
        os.path.abspath(llvm_cov_path) if llvm_cov_path else None
    )
    self._tmp_dir = os.path.abspath(tmp_dir) if tmp_dir else None
    self._repo_out_dir = None

  @property
  def repo_dir(self) -> str:
    if not self._repo_dir:
      self._repo_dir = get_kernel_repo_dir()
    return self._repo_dir

  def _get_repo_path(self, rel_path: str) -> str:
    repo_path = os.path.join(self.repo_dir, rel_path)
    if not os.path.exists(repo_path):
      logging.error(
          "%s does not exist. If this script is not in the source directory,"
          " specify --repo-dir. If you do not have full kernel source,"
          " specify --llvm-cov, --gcno-dir, and --tmp-dir.",
          repo_path,
      )
      sys.exit(-1)
    return repo_path

  @property
  def llvm_cov_path(self) -> str:
    if not self._llvm_cov_path:
      # Load the clang version in kernel repo,
      # or use the stable version in platform repo.
      clang_version = load_kernel_clang_version(self.repo_dir)
      self._llvm_cov_path = self._get_repo_path(
          PREBUILT_LLVM_COV_PATH_FORMAT % clang_version if clang_version else
          PREBUILT_STABLE_LLVM_COV_PATH
      )
    return self._llvm_cov_path

  @property
  def repo_out_dir(self) -> str:
    if not self._repo_out_dir:
      self._repo_out_dir = self._get_repo_path("out")
    return self._repo_out_dir

  @property
  def tmp_dir(self) -> str:
    if not self._tmp_dir:
      # Temporary directory does not have to exist.
      self._tmp_dir = os.path.join(self.repo_dir, OUTPUT_COV_DIR)
    return self._tmp_dir

  @property
  def llvm_gcov_sh_path(self) -> str:
    return os.path.join(self.tmp_dir, "tmp", "llvm-gcov.sh")


def main() -> None:
  arg_parser = argparse.ArgumentParser(
      description="Generate lcov tracefiles from gcov file dumps"
  )

  arg_parser.add_argument(
      "-t",
      dest="tar_location",
      required=True,
      help=(
          "Either a path to a gcov tar file or a directory that contains gcov"
          " tar file(s). The gcov tar file is expected to be created from"
          " Tradefed. If a directory is used, will search the entire directory"
          " for files matching *_kernel_coverage*.tar.gz and select the most"
          " recent one."
      ),
  )
  arg_parser.add_argument(
      "-o",
      dest="out_file",
      required=False,
      help="Name of output tracefile generated. Default: cov.info",
      default="cov.info",
  )
  arg_parser.add_argument(
      "--include",
      action="append",
      nargs=1,
      required=False,
      help=(
          "File pattern of source file(s) to include in generated tracefile."
          " Multiple patterns can be specified by using multiple --include"
          " command line switches. If no includes are specified all source is"
          " included."
      ),
  )
  arg_parser.add_argument(
      "--repo-dir",
      required=False,
      help="Root directory of kernel source"
  )
  arg_parser.add_argument(
      "--dist-dir",
      dest="dist_dirs",
      action="append",
      default=[],
      required=False,
      help="Dist directory containing gcno mapping files"
  )
  arg_parser.add_argument(
      "--gcno-dir",
      dest="gcno_dirs",
      action="append",
      default=[],
      required=False,
      help="Path to an extracted .gcno.tar.gz"
  )
  arg_parser.add_argument(
      "--llvm-cov",
      required=False,
      help=(
          "Path to llvm-cov. Default: "
          + os.path.join("<repo_dir>", PREBUILT_LLVM_COV_PATH_FORMAT % "*")
          + " or " + os.path.join("<repo_dir>", PREBUILT_STABLE_LLVM_COV_PATH)
      )
  )
  arg_parser.add_argument(
      "--tmp-dir",
      required=False,
      help=(
          "Path to the directory where the temporary files are created."
          " Default: " + os.path.join("<repo_dir>", OUTPUT_COV_DIR)
      )
  )
  arg_parser.add_argument(
      "--verbose",
      action="store_true",
      default=False,
      help="Enable verbose logging",
  )

  args = arg_parser.parse_args()

  if args.verbose:
    logging.basicConfig(level=logging.DEBUG)
  else:
    logging.basicConfig(level=logging.WARNING)

  if shutil.which(LCOV) is None:
    logging.error(
        "%s is not found and is required for this script. Please install from:",
        LCOV,
    )
    logging.critical("       https://github.com/linux-test-project/lcov")
    sys.exit(-1)

  if args.repo_dir and not os.path.isdir(args.repo_dir):
    logging.error("%s is not a directory.", args.repo_dir)
    sys.exit(-1)

  if args.llvm_cov and not os.path.isfile(args.llvm_cov):
    logging.error("%s is not a file.", args.llvm_cov)
    sys.exit(-1)

  for gcno_dir in args.gcno_dirs + args.dist_dirs:
    if not os.path.isdir(gcno_dir):
      logging.error("%s is not a directory.", gcno_dir)
      sys.exit(-1)

  config = Config(args.repo_dir, args.llvm_cov, args.tmp_dir)

  gcno_mappings = collections.defaultdict(list)
  if not args.gcno_dirs and not args.dist_dirs:
    dist_dir_pattern = os.path.join(config.repo_out_dir, "**", "dist")
    read_gcno_mapping_files(dist_dir_pattern, config.repo_dir, gcno_mappings)

  for dist_dir in args.dist_dirs:
    read_gcno_mapping_files(dist_dir, config.repo_dir, gcno_mappings)

  for gcno_dir in args.gcno_dirs:
    read_gcno_dir(gcno_dir, gcno_mappings)

  if not gcno_mappings:
    # read_gcno_mapping_files prints the error messages
    sys.exit(-1)

  tar_file = find_most_recent_tarfile(
      args.tar_location, pattern="*kernel_coverage_*.tar.gz"
  )
  if tar_file is None:
    logging.error("Unable to find a gcov tar under %s", args.tar_location)
    sys.exit(-1)

  gcov_dir = unpack_gcov_tar(tar_file, config.tmp_dir)
  correct_symlinks_in_directory(gcov_dir, gcno_mappings)

  create_llvm_gcov_sh(
      config.llvm_cov_path,
      config.llvm_gcov_sh_path,
  )

  generate_lcov_tracefile(
      gcov_dir,
      config.repo_dir,
      config.llvm_gcov_sh_path,
      args.out_file,
      args.include,
  )


if __name__ == "__main__":
  main()
