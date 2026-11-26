// ERROR: no member named 'y'
struct s { int x; };
int main(void) { struct s v; return v.y; }
