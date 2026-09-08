/*
 * Reduced model of c-ares' skip list, used to expose where the two parameter
 * analyses disagree about how deep an access path may go.
 *
 * The shape that matters is `struct node **head`: a heap-allocated array of
 * pointers. Reading head[i] is a load of a pointer *out of the heap*, so the
 * loaded node is not derived from `list` by any def-use chain - it is derived
 * from whatever store filled the array. Everything past that field is only
 * reachable by reasoning about memory, not about values.
 *
 * Field indices are chosen to mirror src/lib/dsa/ares_slist.c one for one,
 * except that ares_channeldata.servers (index 24 there) is index 1 here so the
 * dumped SVFG stays small enough to read:
 *
 *   c-ares                          here
 *   ares_channeldata.servers  24    channel.servers  1
 *   ares_slist.head            3    list.head        3
 *   ares_slist.cnt             8    list.cnt         6
 *   ares_slist_node.data       0    node.data        0
 *   ares_slist_node.prev       1    node.prev        1
 *   ares_slist_node.next       2    node.next        2
 *   ares_slist_node.parent     4    node.parent      4
 *
 * so the c-ares path [24,3,2,2] appears here as [1,3,2,2].
 */

/*
 * No <stdlib.h> and no allocator: every object below is a file-scope static.
 * Heap allocation would only add SVF's extapi summary nodes to the dumped
 * SVFG without changing what this test shows - the composition step under
 * test crosses a *load out of memory*, and a global array is loaded exactly
 * the same way a calloc'd one is.
 */
#define NULL ((void *)0)

struct list;

struct node {
  void          *data;   /* 0 */
  struct node  **prev;   /* 1 */
  struct node  **next;   /* 2 */
  unsigned long  levels; /* 3 */
  struct list   *parent; /* 4 - back edge, closes a cycle in the type graph */
};

struct list {
  unsigned long  seed;   /* 0 */
  unsigned long  bits;   /* 1 */
  unsigned long  spare;  /* 2 */
  struct node  **head;   /* 3 - the heap array of pointers */
  unsigned long  levels; /* 4 */
  struct node   *tail;   /* 5 */
  unsigned long  cnt;    /* 6 */
};

struct channel {
  unsigned int  flags;   /* 0 */
  struct list  *servers; /* 1 */
};

/*
 * ares_slist.c:153-162. `left = left->next[lvl]` is a loop back edge: each
 * iteration walks to a different node object, but an access path has no way to
 * say "the same field again", so a traversal turns into nested fields
 * .3 -> .3.2 -> .3.2.2 -> ...
 */
static struct node *seek_tail(struct list *list, unsigned long lvl) {
  struct node *left = list->head[lvl];

  while (left != NULL && left->next[lvl] != NULL) {
    left = left->next[lvl];
  }
  return left;
}

/* ares_slist.c:269 - node->next[i]->prev[i] = node->prev[i]; */
static void unlink_at(struct node *node, unsigned long lvl) {
  if (node->next[lvl] != NULL) {
    node->next[lvl]->prev[lvl] = node->prev[lvl];
  }
}

/*
 * The back-pointer cycle. Reached from the channel this is .1.3.4, which is a
 * `struct list *` again - so the whole list subtree can be appended a second
 * time (.1.3.4.3, .1.3.4.3.2, ...) without ever leaving one allocation.
 */
static unsigned long owner_count(struct node *node) {
  return node->parent->cnt;
}

/* The function under test. Only direct calls. */
unsigned long channel_walk(struct channel *c) {
  struct list *list = c->servers;
  struct node *n    = seek_tail(list, 0);

  if (n == NULL) {
    return 0;
  }
  unlink_at(n, 0);
  return owner_count(n) + list->cnt;
}

/*
 * Gives Andersen's concrete objects to point at, so head[] and next[] have a
 * real store behind them and the SVFG shows the memory edges the value-flow
 * walk would have to cross.
 */
static struct channel g_channel;
static struct list    g_list;
static struct node    g_a, g_b;
static struct node   *g_head[4];
static struct node   *g_a_next[4], *g_a_prev[4];
static struct node   *g_b_next[4], *g_b_prev[4];

struct channel *make_channel(void) {
  g_a.next = g_a_next;
  g_a.prev = g_a_prev;
  g_b.next = g_b_next;
  g_b.prev = g_b_prev;

  g_a.next[0] = &g_b;
  g_b.prev[0] = &g_a;
  g_a.parent  = &g_list;
  g_b.parent  = &g_list;

  g_head[0]     = &g_a;
  g_list.head   = g_head;
  g_list.levels = 4;
  g_list.tail   = &g_b;
  g_list.cnt    = 2;

  g_channel.servers = &g_list;
  return &g_channel;
}

int driver(void) {
  struct channel *c = make_channel();
  return (int)channel_walk(c);
}
