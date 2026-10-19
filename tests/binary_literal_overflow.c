/* Regression test: the 0b fold accumulated into a signed long long, so a
 * literal whose top bit lands in the sign position shifted into it. That is
 * undefined behaviour, not a wrapped value, and UBSan flagged it. The fold now
 * accumulates unsigned and converts once on the way out.
 */
int main(void){ switch (0) { case 0b1111111111111111111111111111111111111111111111111111111111111111: return 1; } return 0; }
