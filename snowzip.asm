default rel

section .text
global snowzip_crc32

; uint32_t snowzip_crc32(const void *data, size_t size)
; Windows x64 ABI:
; RCX = data
; RDX = size
; EAX = CRC-32

snowzip_crc32:
    push rbx
    mov rbx, rcx
    mov rcx, rdx
    mov eax, 0FFFFFFFFh

.loop:
    test rcx, rcx
    jz .finish

    xor al, byte [rbx]
    inc rbx
    mov r8d, 8

.bit:
    shr eax, 1
    jnc .next_bit
    xor eax, 0EDB88320h

.next_bit:
    dec r8d
    jnz .bit

    dec rcx
    jmp .loop

.finish:
    not eax
    pop rbx
    ret
