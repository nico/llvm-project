#!/usr/bin/env python
import re
from subprocess import Popen, PIPE

def sizeof_fmt(num, suffix='B'):
    for unit in ['','Ki','Mi','Gi','Ti','Pi','Ei','Zi']:
        if abs(num) < 1024.0:
            return "%3.1f%s%s" % (num, unit, suffix)
        num /= 1024.0
    return "%.1f%s%s" % (num, 'Yi', suffix)

# clang-cl prints strings like this for
# extern int e();
# void f() {
#   ^{ [...] ^{ static int i = e(); }(); [...] }();
# }
# where [...] and [...] are N repetitions of `^{` and `}();`.
#s = '?i@' + n * '?1??foo@@YAXXZ' + '@4HA'
# ...however, here the recursive parse call always just returns 37 bytes
# which is boring.

# The below can't be produced by compilers as far as I know,
# but it causes n^2 memory use in theory.
#s = '?i@' + n*'?1??foo@' + n*'@4HA' + '@4HA'

with open('tmp.txt', 'w') as f:
  n = 10000
  f.write('?i@' + n*'?1??foo@' + n*'@4HA' + '@4HA')


# ?i@                        @4HA
# ?i@?1??foo@            @4HA@4HA
# ?i@?1??foo@?1??foo@@4HA@4HA@4HA

n = 1000
while n <= 100000000:
    #s = '?i@' + n * '?1??foo@@YAXXZ' + '@4HA'
    s = '?i@' + n*'?1??foo@' + n*'@4HA' + '@4HA'
    p = Popen(['/usr/bin/time', '-l', 'out/gn/bin/llvm-undname'],
              stdin=PIPE, stdout=PIPE, stderr=PIPE)
    p.stdin.write(s)
    stdout, stderr = p.communicate()
    p.stdin.close()
    t = float(re.search(r'(\d+\.\d+) real', stderr).group(1))
    mem = int(re.search(r'(\d+)  maximum resident set size', stderr).group(1))
    print n, t, sizeof_fmt(mem)
    n *= 10
