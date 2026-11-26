// ERROR: calls to the unprototyped function 'f' with different argument types
int f();
void a(void) { f(1); }
void b(void) { f(2.5); }
