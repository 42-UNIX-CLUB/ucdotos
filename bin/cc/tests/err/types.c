// ERROR: incompatible types
struct a { int x; };
struct b { int x; };
int main(void) { struct a p; struct b q = p; return 0; }
