#!/usr/bin/python3
#
# Copyright 2024 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys

import all_tests
import gki

if __name__ == '__main__':
  gki.IS_GKI = True
  if len(sys.argv) > 1:
    all_tests.RunTests(sys.argv[1:])
  else:
    all_tests.RunTests(all_tests.all_test_modules)
