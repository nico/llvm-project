#!/usr/bin/env python

"""to_isolate.py takes gn desc runtime_deps output and writes an isolate file.

Example usage:
    llvm/utils/gn/gn.py desc out/gn //clang/test:check-clang runtime_deps | \
        python to_isolate.py > out/gn/check-clang.isolate
"""

from __future__ import print_function
import pprint
import fileinput


print(pprint.pformat(
    { 'variables': { 'files': [s.rstrip() for s in fileinput.input()] } }))
