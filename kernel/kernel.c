// Prevent name mangling if using a C++ compiler later
#ifdef __cplusplus
extern "C"
#endif
void kernel_main(void) {
    // Pointer to the x86 text-mode VGA frame buffer
    volatile char* vga_buffer = (volatile char*)0xB8000;

    const char* message = "SLS Kernel Booted Successfully!";
    
    // Clear screen and write the string
    // Each character takes 2 bytes: [ASCII character, Attribute/Color]
    for (int i = 0; message[i] != '\0'; i++) {
        vga_buffer[i * 2] = message[i];     // Set character byte
        vga_buffer[i * 2 + 1] = 0x0F;       // Attribute byte: White text on Black background
    }

    // Hang the kernel execution safely
    while (1) {
        __asm__ volatile("hlt");
    }
}