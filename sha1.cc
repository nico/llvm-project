#include <algorithm>
#include <map>
#include <stdio.h>

struct TrieNode {
  TrieNode(TrieNode *parent) : parent(parent) {
    std::fill_n(freq, 16, 0);
    std::fill_n(next, 16, nullptr);
  }
  int freq[16];
  TrieNode *next[16];
  TrieNode *parent;
};

int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  return c - 'a' + 10;
}

void print(TrieNode *t) {
  if (!t->parent) return;
  print(t->parent);
  int i;
  for (i = 0; i < 16; ++i)
    if (t->parent->next[i] == t)
      break;
  printf("%x", i);
}

void walk(TrieNode *t, int l, std::map<int, int> &pathfreq) {
  for (int i = 0; i < 16; ++i) {
    if (t->freq[i] > 1)
      walk(t->next[i], l + 1, pathfreq);
    else if (t->freq[i] == 1) {
      pathfreq[l]++;
      if (l >= 8) {
        print(t->next[i]);
        printf("\n");
      }
    }
  }
}

int main() {
  TrieNode trie(nullptr);
  char l[42]; // 40 chars sha1, 1 char \n, 1 char \0
  FILE *f = fopen("sha1.txt", "rb");
  while (fgets(l, sizeof(l), f)) {
    TrieNode *t = &trie;
    for (int i = 0; i < 40; ++i) {
      int n = hex(l[i]);
      if (!t->next[n])
        t->next[n] = new TrieNode(t);
      t->freq[n]++;
      t = t->next[n];
    }
  }

  std::map<int, int> pathfreq;
  walk(&trie, 0, pathfreq);

  for (const auto &p : pathfreq)
    printf("%d %d\n", p.first, p.second);
}
