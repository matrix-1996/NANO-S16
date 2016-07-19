; Based in bootloaders by
;     -George Klees
;     -E. Dehling
;     -Mike Saunders
; Boot Record for BIOS-based PCs
;
; This program is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License version 3 as
; published by the Free Software Foundation.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public Licens
; along with this program; if not, write to the Free Software
; Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

%include "ext2.inc"

; Code location constants
%define ORG_LOC           0x7C00        ; Initial MBR position in memory
%define STAGE_LOC         0x8000        ; Location of stage
%define DRIVE_LOC         0x0660        ; Location of the local data structure in memory

%defstr stgFile kernel
%strlen stgLen stgFile

[ORG ORG_LOC]
[BITS 16]
    jmp 0x0000:start

; Start of the bootstrap code
start:
    ; Set up a stack
    mov ax, 0

    cli                    ; Disable interrupts while changing stack
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov esp, ORG_LOC
    sti

.setup_data:
    mov [DRIVE_LOC], dl

    mov ah, 8            ; Get drive parameters
    int 13h
    jc error
    and cx, 3Fh          ; Maximum sector number
    mov [SECTORS], cx    ; Sector numbers start at 1
    movzx dx, dh         ; Maximum head number
    add dx, 1            ; Head numbers start at 0 - add 1 for total
    mov [SIDES], dx

    ; Load the second sector of the BR
    mov eax, ORG_LOC + 0x200
    mov ebx, 1
    mov ecx, 1
    call partition_read

; Read the superblock
read_superblock:
    ; Read the superblock into memory
    mov eax, SUPERBLOCK_LOC
    mov ebx, 2
    mov ecx, ebx
    call partition_read

    ; Check the signature
    mov ax, [SUPERBLOCK(signature)]
    cmp ax, 0xEF53
    jne error

    ; Calculate and store the block size
    mov ecx, [SUPERBLOCK(block_size)]
    mov edx, 1024
    shl edx, cl
    mov [SUPERBLOCK(block_size)], edx

; Read stage
read_stage:
    ; Clear the carry flag
    clc

    ; Find the inode for stage
    mov eax, 2
    mov ebx, stage
    call ext2_finddir

    ; Read the inode for stage
    mov ebx, eax
    mov eax, INODE_LOC
    call read_inode

    ; Load stage's data from the disk
    mov ebx, STAGE_LOC
    mov ecx, [INODE(eax, low_size)]
    call ext2_read

    ; Jump to stage
    mov dl, [DRIVE_LOC]
    jmp 0x0800:0x0000

; Read from the partition (eax = Buffer, ebx = Sector, ecx = Numsectors)
partition_read:
    pusha

    push ax
    push cx
    mov eax, 0
    mov ax, bx             ; Save the stack pointer

    call disk_convert_l2hts

    pop ax                ; AL: Number of sectors to read
    mov ah, 2             ; Params for int 13h: read disk sectors
    mov bx, ds            ; Set ES:BX to point the buffer
    mov es, bx
    pop bx

    pusha                 ; Prepare to enter loop

.read_loop:
    popa
    pusha

    stc                   ; A few BIOSes do not set properly on error
    int 13h               ; Read sectors

    jnc .read_finished
    call disk_reset       ; Reset controller and try again
    jnc .read_loop        ; Disk reset OK?

    popa
    jmp error             ; Fatal double error

.read_finished:
    popa                  ; Restore registers from main loop

    popa                  ; And restore from start of this system call
    ret

; Reset disk
disk_reset:
    push ax
    mov ax, 0
    stc
    int 13h
    pop ax
    ret

; disk_convert_l2hts -- Calculate head, track and sector for int 13h
; IN: logical sector in AX; OUT: correct registers for int 13h
disk_convert_l2hts:
    push bx
    push ax

    mov bx, ax            ; Save logical sector

    mov dx, 0             ; First the sector
    div word [SECTORS]    ; Sectors per track
    add dl, 01h           ; Physical sectors start at 1
    mov cl, dl            ; Sectors belong in CL for int 13h
    mov ax, bx

    mov dx, 0             ; Now calculate the head
    div word [SECTORS]    ; Sectors per track
    mov dx, 0
    div word [SIDES]      ; Disk sides
    mov dh, dl            ; Head/side
    mov ch, al            ; Track

    pop ax
    pop bx

    mov dl, [DRIVE_LOC]   ; Set device

    ret

SECTORS   dw 18
SIDES     dw 2


; Read a block (eax = Buffer, ebx = Block)
read_block:
    ; Calculate the starting sector and number of sectors
    mov ecx, [SUPERBLOCK(block_size)]
    shr ecx, 9
    imul ebx, ecx

    ; Read the block into memory and return
    call partition_read
    ret


; Read a block group descriptor (EAX = Block Group)
read_bgdesc:
    ; Calculate the block and offset, storing them in EAX and EDX
    shl eax, 5                          ; EAX = block_group * sizeof(bgdesc_t)
    xor edx, edx
    mov ebx, [SUPERBLOCK(block_size)]   ; EBX = (superblock->block_size)
    div ebx                             ; EAX = (block_group * sizeof(bgdesc_t)) / (superblock->block_size)
                                        ; EDX = (block_group * sizeof(bgdesc_t)) % (superblock->block_size)

    add eax, [SUPERBLOCK(superblock_block)]
    inc eax

    ; Read the block
    push edx                            ; Save the offset
    mov ebx, eax
    mov eax, BGDESC_LOC
    call read_block

    ; Return the offset
    pop eax                             ; Restore the offset into EAX
    ret

; Read an inode (eax = Buffer, ebx = Inode)
read_inode:
    ; Calculate the block group, placing it in EAX
    push eax                            ; Save the buffer
    xor edx, edx
    mov eax, ebx
    dec eax                             ; EAX = (inode - 1)
    push eax                            ; Save (inode - 1)
    mov ebx, [SUPERBLOCK(inodes_per_group)]        ; EBX = (superblock->inodes_per_group)
    div ebx                                        ; EAX = (inode - 1) / (superblock->inodes_per_group)
    call read_bgdesc                    ; Read the block group descriptor
    mov edi, eax                        ; Save the offset into the block group descriptor

    ; Calculate the table index, placing it in EDX
    xor edx, edx
    pop eax                                        ; EAX = (inode - 1)
    mov ebx, [SUPERBLOCK(inodes_per_group)]        ; EBX = (superblock->inodes_per_group)
    div ebx                                        ; EDX = (inode - 1) % (superblock->inodes_per_group)

    ; Calculate the block and offset, storing them in EAX and EDX
    mov eax, edx                                   ; EAX = table_index
    mov ebx, [EXT_SUPERBLOCK(inode_size)]          ; EBX = (ext_superblock->inode_size)

    ; second run: ebx=0x100, edx=0
    mul ebx                                        ; EAX = table_index * (ext_superblock->inode_size)
    xor edx, edx
    mov ebx, [SUPERBLOCK(block_size)]              ; EBX = (superblock->block_size)
    div ebx                                        ; EAX = (table_index * (ext_superblock->inode_size)) / (superblock->block_size)
                                                   ; EDX = (table_index * (ext_superblock->inode_size)) % (superblock->block_size)
    add eax, [BGDESC(edi, inode_table_block)]

    ; Read the block
    mov ebx, eax                                   ; EBX = (bgdesc->inode_table_block) + table_block
    pop eax                                        ; Restore the buffer into EAX
    push edx                                       ; Save the offset
    call read_block

    ; Return the offset
    pop eax                                        ; Restore the offset into EAX
    ret

; Raise a number to a power (EAX = Base, EBX = Exponent)
pow:
    cmp ebx, 1
    jne .loop
    ret
    mov ecx, eax
.loop:
    mul ecx
    dec ebx
    cmp ebx, 1
    ja .loop
    ret


; Read a block pointer (EAX = Buffer, EBX = Block, ECX = Length, EDX = Level)
read_block_pointer:
    ; Check if we're reading an indirect block pointer
    cmp edx, 0
    ja .indirect
.direct:
    ; Check whether we're reading the block size or less
    mov esi, [SUPERBLOCK(block_size)]
    cmp ecx, esi
    jb .direct_continue
    mov ecx, esi
.direct_continue:
    push ecx
    call read_block
    pop eax
    ret
.indirect:
    ; Make sure we're not way over the max level
    cmp edx, 3
    ja error

    ; Read the indirect block pointers
    push eax                    ; Save the buffer
    push ecx                    ; Save the length
    push edx                    ; Save the level we're at
    jmp .next

; ------------------------------------------------------------------
; END OF BOOT SECTOR AND BUFFER START

    times 510-($-$$) db 0        ; Pad remainder of boot sector with zeros
    dw 0AA55h                    ; Boot signature (DO NOT CHANGE!)

.next:
    mov eax, BLOCK_PTRS_LOC
    call read_block

    ; Check whether we're reading the block size or less
    mov eax, [SUPERBLOCK(block_size)]       ; EAX = block_size
    shr eax, 2                              ; EAX = block_size / 4

    pop ebx                                 ; Restore the level we're at into EBX
    mov esi, ebx                            ; Save the level we're at
    call pow                                ; EAX = (block_size / 4) ** level
    mov ebx, [SUPERBLOCK(block_size)]       ; EBX = block_size
    mul ebx                                 ; EAX = block_size * ((block_size / 4) ** level)

    pop ecx                                 ; Restore the length into ECX
    cmp ecx, eax
    jb .indirect_continue
    mov ecx, eax
.indirect_continue:
    pop eax                                 ; Restore the buffer into EAX
    push ecx                                ; Save the length
    push eax                                ; Save the buffer

    mov edx, esi                            ; EDX = level
    dec edx

    mov esi, ecx                            ; ESI = bytes_left
    mov edi, 0                              ; EDI = blocks_read

    mov eax, [SUPERBLOCK(block_size)]       ; EAX = block_size
    shr eax, 2                              ; EAX = block_size / 4
    mov [SUPERBLOCK_LOC + 0x400], eax       ; Cache block_size / 4
.indirect_loop:
    ; Check the two loop conditions
    cmp esi, 0                              ; If bytes_left == 0
    je .loop_end                            ; Goto .loop_end
    mov ecx, [SUPERBLOCK_LOC + 0x400]       ; ECX = block_size / 4
    cmp edi, ecx                            ; If blocks_read >= block_size / 4
    jae .loop_end                           ; Goto .loop_end

    ; Set up function args
    pop eax                                 ; EAX = buffer
    mov ebp, edi
    shl ebp, 2
    mov ebx, [BLOCK_PTRS_LOC + ebp]         ; EBX = block_ptrs[blocks_read]
    mov ecx, esi

    ; Save regs
    push eax                                ; Save the buffer
    push edx                                ; Save the level we're at
    push esi                                ; Save bytes_left
    push edi                                ; Save blocks_read

    ; Recursively call ourself
    call read_block_pointer

    ; Restore registers
    pop edi                                  ; EDI = blocks_read
    pop esi                                  ; ESI = bytes_left
    pop edx

    ; Update blocks read and bytes read
    sub esi, eax
    inc edi

    ; Update buffer pointer
    pop eax
    add eax, [SUPERBLOCK(block_size)]        ; EAX = buffer + block_size
    push eax

    ; Reenter the loop
    jmp .indirect_loop
.loop_end:
    pop eax
    pop eax                                   ; EAX = length
    sub eax, esi                              ; EAX = length - bytes_left
.end:
    ret


; Read data from an inode (EAX = Inode, EBX = Buffer, ECX = Length)
ext2_read:
    ; Keep track of bytes_read and direct_blocks_read
    mov esi, ecx                              ; ESI = bytes_left = length
    mov edi, 0                                ; EDI = direct_blocks_read = 0

    ; Save the inode to EBP
    mov ebp, eax
.read_direct:
    ; Check the two loop conditions
    cmp esi, 0                                ; If bytes_left == 0
    je .success                               ; Goto .success
    cmp edi, 48                               ; If blocks_read >= 12
    jae .read_single                          ; Goto .read_single

    ; Set up args
    mov eax, ebx
    mov ebx, [INODE(ebp, direct_block) + edi]
    mov ecx, esi
    mov edx, 0

    ; Save regs
    push ebp                                   ; Save the inode offset
    push eax                                   ; Save the buffer
    push esi                                   ; Save bytes_left
    push edi                                   ; Save direct_blocks_read

    ; Read the direct block pointer
    call read_block_pointer

    ; Restore regs
    pop edi
    add edi, 4
    pop esi
    pop ebx
    add ebx, [SUPERBLOCK(block_size)]
    pop ebp

    ; Subtract its return value from bytes_left
    sub esi, eax

    ; Reenter the loop
    jmp .read_direct
.read_single:
    ; Set up args
    mov eax, ebx
    mov ebx, [INODE(ebp, single_block)]
    mov ecx, esi
    mov edx, 1

    ; Save bytes_left
    push esi                                   ; Save bytes_left

    ; Read the direct block pointer
    call read_block_pointer

    ; Restore bytes_left and subtract the return value from it
    pop esi
    sub esi, eax

    ; If there are more bytes left to read, fail
    cmp esi, 0
    ja error
.success:
    ret


; Find a directory entry by name (EAX = Inode, EBX = Name)
ext2_finddir:
    ; Save the name
    push ebx

    ; Read the inode into memory
    mov ebx, eax
    mov eax, INODE_LOC
    call read_inode

    ; Load the inode's data from the disk
    mov ebx, DIRENT_LOC
    mov ecx, [INODE(eax, low_size)]
    push ecx
    call ext2_read

    ; Prepare the loop
    pop ebp
    xor ecx, ecx

    ; Restore the name into EAX
    pop eax
.loop:
    ; Make sure we haven't exceeded the length
    cmp ecx, ebp
    jae error

    ; Save regs
    push ebp
    push eax
    push ecx

    ; Compare the 2 string lengths (if not the same length, obviously not equal)
    mov bl, [DIRENT(ecx, name_length)]
    mov eax, stgLen
    cmp bl, al
    je .compare
    pop ecx
    xor edx, edx
    mov dx, [DIRENT(ecx, size)]
    add ecx, edx
    pop eax
    pop ebp
    jmp .loop
.compare:
    ; Set up loop stuff
    xor edx, edx
    mov ebp, eax

    ; ESI and EDI equal the searched-for and actual string
    pop ecx
    pop esi
    mov edi, DIRENT_LOC
    add edi, ecx
    add edi, 8
    push esi
    push ecx
.compare_loop:
    ; Success
    cmp edx, ebp
    jae .success

    ; Loop and compare, if there's ever a non-match, the compare failed
    mov al, byte [esi]
    mov bl, byte [edi]
    cmp al, bl
    jne .exit_comparison
    inc esi
    inc edi
    inc edx
    jmp .compare_loop
.exit_comparison:
    ; Restore and increment the directory entry offset
    pop ecx
    mov bp, [DIRENT(ecx, size)]
    add ecx, ebp

    ; Restore the filename and length of the directory before reentering the loop
    pop eax
    pop ebp
    jmp .loop
.success:
    inc byte [nfind]

    pop ecx
    pop eax
    mov eax, [DIRENT(ecx, inode)]
    pop ebp
    ret


stage    db 'kernel',0
nfind    db 0

; Error function
error:
    jmp $                        ; Hang forever

; Fill the remaining bytes with zeroes
times 1024 - ($ - $$) db 0