
def insert(trie, s):
  t = trie
  for c in s:
    n = int(c, 16)
    if t[n] is None:
      t[n] = [0, [None] * 16]
    t[n][0] += 1
    t = t[n][1]
    

# Key: a hex digit, 0-15
# Value: (Frequency, Subtrie) pair
trie = [None] * 16


for l in open('sha1.txt').readlines():
  insert(trie, l.rstrip())
