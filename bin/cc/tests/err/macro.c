// ERROR: unterminated call of macro 'F'
#define F(a) a
int main(void) { return F(1; }
