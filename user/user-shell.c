int main(void) {
    __asm__ volatile("mov %0, %%eax" : : "r"(0xDEADBEEFu));
    return 0;
}
