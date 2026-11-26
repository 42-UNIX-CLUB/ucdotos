// ERROR: inline assembly is not supported
int main(void) { asm("i32.const 1 drop"); return 0; }
