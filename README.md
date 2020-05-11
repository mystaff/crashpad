<!--
Copyright 2015 The Crashpad Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Crashpad

[Crashpad](https://crashpad.chromium.org/) is a crash-reporting system.

## Documentation

 * [Project status](doc/status.md)
 * [Developing Crashpad](doc/developing.md): instructions for getting the source
   code, building, testing, and contributing to the project.
 * [Crashpad interface documentation](https://crashpad.chromium.org/doxygen/)
 * [Crashpad tool man pages](doc/man.md)
 * [Crashpad overview design](doc/overview_design.md)

## Source Code

Crashpad’s source code is hosted in a Git repository at
https://chromium.googlesource.com/crashpad/crashpad.

## Other Links

 * Bugs can be reported at the [Crashpad issue
   tracker](https://crashpad.chromium.org/bug/).
 * The [Crashpad bots](https://ci.chromium.org/p/crashpad/g/main/console)
   perform automated builds and tests.
 * [crashpad-dev](https://groups.google.com/a/chromium.org/group/crashpad-dev)
   is the Crashpad developers’ mailing list.

## Clone and build mystaff's Crashpad fork (HOWTO)

 * Follow instruction detailed [here](https://github.com/mystaff/crashpad/blob/master/doc/developing.md).
 
After getting depot_tools, you need to:

 * In `depot_tools` folder you need to create `fetch_configs/crashpad_mystaff.py` with this content:

```
# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import sys

import config_util  # pylint: disable=import-error


# This class doesn't need an __init__ method, so we disable the warning
# pylint: disable=no-init
class CrashpadMyStaffConfig(config_util.Config):
  """Basic Config class for Timedoctor's Crashpad fork."""

  @staticmethod
  def fetch_spec(props):
    spec = {
      'solutions': [
        {
          'name': 'crashpad',
          'url': 'https://github.com/mystaff/crashpad.git',
          'managed': False,
        },
      ],
    }
    return {
      'type': 'gclient_git',
      'gclient_git_spec': spec,
    }

  @staticmethod
  def expected_root(_props):
    return 'crashpad'


def main(argv=None):
  return CrashpadMyStaffConfig().handle_args(argv)


if __name__ == '__main__':
  sys.exit(main(sys.argv))
```
 * Modify `recipes/recipe_modules/gclient/config.py` adding this

```
@config_ctx()
def crashpad_mystaff(c):
   soln = c.solutions.add()
   soln.name = 'crashpad_mystaff'
   soln.url = 'https://github.com/mystaff/crashpad.git'
```

 * After that run `fetch crashpad_mystaff`

