# 1. Assemble the bootloader into an ELF64 object file
nasm -f elf64 boot.asm -o boot.o

# 2. Compile our C kernel without host-system dependencies (freestanding)
x86_64-elf-gcc -c kernel.c -o kernel.o -ffreestanding -O2 -Wall -Wextra

# 3. Link everything together using our linker script
x86_64-elf-ld -T linker.ld -o my_sls_kernel.bin boot.o kernel.o
