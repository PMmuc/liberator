struct Node {
  int val;
  struct Node *next;
};

int sum_list(struct Node *head) {
  if (!head)
    return 0;
  head->val += 1;
  return head->val + sum_list(head->next);
}
